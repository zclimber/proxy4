#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dispatch.h"
#include "dns_dispatched.h"
#include "http.h"
#include "loaders.h"
#include "relay.h"
#include "util.h"

using std::string;

class proxy_connection: public std::enable_shared_from_this<proxy_connection> {
	dispatch::fd_ref client_sock, server_sock;
	string buf;
	std::vector<dispatch::event_ref> event_vec;
	std::future<int> fut;
	int relaycount = 0;
	// 0 - fail_client
	// 1 - fail_server
public:
	proxy_connection(int client_sock) :
			client_sock(client_sock,
			EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET) {
	}
	void start() {
		load_request_headers();
	}
private:
	void load_request_headers() {
		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::fail_loading_client,
						shared_from_this()));
		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::fail_connecting_to_server,
						shared_from_this()));
		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_request_headers,
						shared_from_this()));
		util::name_fd(client_sock.fd(),
				string("client") + std::to_string(client_sock.fd()));
		util::log() << "Start loading request headers on socket "
				<< client_sock;
		async_load::headers(buf, client_sock, event_vec.back(), event_vec[0]);
	}
	void process_request_headers() {
//		log << "Got headers on socket " << client_sock << "\n";
		header_parser hp;
		hp.set_string(buf);
		std::string host = hp.headers()["Host"];
		std::string port = "80";
		std::size_t colon = host.find(':');
		if (colon != std::string::npos) {
			port = host.substr(colon + 1, host.length());
			host = host.substr(0, colon);
		}
		std::ofstream os(std::string("log/") + "->" + host + ":" + port
				, std::ios::out | std::ios::binary | std::ios::ate);
		os << "\nNEW CONNECTION\n";
		os << buf;
		os.flush();
//		log << "Connecting to server " << host << ":" << port << "\n";

		event_vec.push_back(dispatch::event_ref());

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_request_headers_2,
						shared_from_this(), hp));

		fut = connect_to_remote_server(host, port, event_vec.back());
	}
	void process_request_headers_2(header_parser hp) {
		int ssock = fut.get();
		if (ssock == -1) {
			util::log() << "Could not connect to server " << hp.headers()["Host"];
			dispatch::arm_manual(event_vec[1]);
			return;
		}
		server_sock = dispatch::fd_ref(ssock,
		EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
		util::log() << hp.request() << " " << client_sock << " -> "
				<< server_sock;
		util::name_fd(server_sock.fd(), hp.headers()["Host"]);
//		log << "Connected to server " << host << ":" << port << " at socket "
//				<< server_sock << "\n";
		if (hp.request().compare(0, 7, "CONNECT") == 0) {
			start_connect_tunnel();
		} else {
			upload_request_to_server();
		}
	}
	void upload_request_to_server() {
		header_parser hp;
		hp.set_string(buf);
		hp.headers()["Connection"] = "close";
		buf = hp.assemble_head() + hp.excess();

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::upload_request_to_server_2,
						shared_from_this()));

		if (hp.headers().count("Transfer-Encoding")
				&& hp.headers()["Transfer-Encoding"].find("chunked")
						!= std::string::npos) {
			// chunked

			async_load::chunked(buf, client_sock, event_vec.back(),
					event_vec[1]);
		} else if (hp.headers().count("Content-Length")) {
			async_load::fixed(buf, client_sock,
					stol(hp.headers()["Content-Length"]) - hp.excess().length(),
					event_vec.back(), event_vec[1]);
			// body
		} else {
			upload_request_to_server_2();
			return;
		}
	}
	void upload_request_to_server_2() {

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::upload_request_to_server_3,
						shared_from_this()));

		async_load::upload(buf, server_sock, event_vec.back(), event_vec[1]);
	}
	void upload_request_to_server_3() {

//		log << "Uploaded client request from " << client_sock << " to "
//				<< server_sock << "\n";
		buf.clear();

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_response_headers,
						shared_from_this()));
		async_load::headers(buf, server_sock, event_vec.back(), event_vec[1]);
	}

	void process_response_headers() {
		util::log() << "Got response from server at " << server_sock;
		header_parser hp;
		hp.set_string(buf);
		hp.headers()["Connection"] = "close";
		buf = hp.assemble_head() + hp.excess();

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_response_headers_2,
						shared_from_this()));
		if (hp.headers().count("Transfer-Encoding")
				&& hp.headers()["Transfer-Encoding"].find("chunked")
						!= std::string::npos) {
			// chunked

			async_load::chunked(buf, server_sock, event_vec.back(),
					event_vec[1]);
		} else if (hp.headers().count("Content-Length")) {
			// fixed length body
			async_load::fixed(buf, server_sock,
					stol(hp.headers()["Content-Length"]) - hp.excess().length(),
					event_vec.back(), event_vec[1]);
		} else {
			async_load::fixed(buf, server_sock,
			INT_LEAST32_MAX, event_vec.back(), event_vec.back());
			// pump until ends
		}

	}
	void process_response_headers_2() {
//		log << "Downloaded server response from " << server_sock.fd() << " "
//				<< buf.length() << " bytes\n";
		auto thisptr = shared_from_this();
		event_vec.emplace_back( // @suppress("Ambiguous problem")
				[this, thisptr] {
					util::log() << "Uploaded server response from " << server_sock << " to "
					<< client_sock;
					cleanup();
				});
		async_load::upload(buf, client_sock, event_vec.back(), event_vec[1]);
	}

	void start_connect_tunnel() {
		header_parser hp;
		hp.request() = "HTTP/1.1 200 Connection established";
		hp.headers()["Proxy-agent"] = "mylittleproxy 0.1";
		std::string newbuf = hp.assemble_head();
		auto thisptr = shared_from_this();
		auto fin = [thisptr]() -> void {
			thisptr->relaycount--;
			if(thisptr->relaycount == 0) {
				thisptr->cleanup();
			}
		};
		relaycount = 2;
		std::string empty;
		make_relay(client_sock, server_sock, empty, fin);
		make_relay(server_sock, client_sock, newbuf, fin);
		util::log() << "Started http tunnel between " << client_sock << " and "
				<< server_sock.fd();
	}

	void fail_loading_client() {
		util::log() << "Failed loading client fd " << client_sock;
		cleanup();
	}
	void fail_connecting_to_server() {
		header_parser hp;
		if (server_sock.fd() > 0) {
			hp.request() = "HTTP/1.1 502 Bad Gateway";
		} else {
			hp.request() = "HTTP/1.1 504 Gateway Timeout";
		}
		util::log() << "Failed connection to server " << server_sock
				<< " with client fd " << client_sock;

		buf = hp.assemble_head();

		auto thisptr = shared_from_this();
		auto fin = [thisptr]() -> void {
			thisptr->cleanup();
		};
		event_vec.emplace_back( // @suppress("Ambiguous problem")
				fin);

		async_load::upload(buf, client_sock, event_vec.back(),
				event_vec.back());
	}
	void cleanup() {
		if (client_sock.fd() != -1) {
			int fd = client_sock.fd();
			client_sock.recycle();
			close(fd);
		}
		if (server_sock.fd() != -1) {
			int fd = server_sock.fd();
			server_sock.recycle();
			close(fd);
		}
		buf = std::string();
		event_vec.clear();
	}
public:
	~proxy_connection() {
	}
};

int main(int argc, char** argv) {
	if (argc <= 1) {
		printf("Usage: proxy port (dns_threads)\n");
		exit(0);
	}
	int port = atoi(argv[1]);
	if (port < 1 || port > UINT16_MAX) {
		printf("Invalid port number %s", argv[1]);
		exit(0);
	}
	int accept_fd = socket(AF_INET, SOCK_STREAM, 0);

	if (accept_fd == -1) {
		printf("Unable to open socket");
		exit(0);
	}

	int dns_threads = 0;
	if (argc > 2) {
		dns_threads = atoi(argv[2]);
		if (dns_threads < 1 || dns_threads > 20) {
			printf("Invalid amount of DNS resolver threads %s", argv[1]);
			exit(0);
		}
	} else {
		dns_threads = 3;
	}

	int incr = 1;
	int reuseaddr = setsockopt(accept_fd, SOL_SOCKET, SO_REUSEADDR, &incr,
			sizeof(incr));
	int reuseport = setsockopt(accept_fd, SOL_SOCKET, SO_REUSEPORT, &incr,
			sizeof(incr));

	if (reuseaddr == -1 || reuseport == -1) {
		printf("Unable to set socket options");
		exit(0);
	}

	struct sockaddr_in srv_addr;
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_addr.s_addr = INADDR_ANY;
	srv_addr.sin_port = htons((short) port);
	if (bind(accept_fd, (sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
		printf("Unable to bind accepting socket to port %d", port);
		exit(0);
	}
	if (listen(accept_fd, 10) == -1) {
		printf("Unable to listen on port %d", port);
		exit(0);
	}

	dispatch::fd_ref acceptor(accept_fd, EPOLLIN);

	dispatch::event_ref accept_ev([accept_fd] {
		struct sockaddr_in cli_addr;
		socklen_t cli_size = sizeof(cli_addr);
		int new_client = accept(accept_fd, (sockaddr *) &cli_addr, &cli_size);
		if(new_client == -1) {
			util::log() << "Error accepting new client: " << util::error();
			return;
		}
		util::log() << "Accepted client " << new_client;
		auto prox = std::make_shared<proxy_connection>(new_client);
		prox->start();
	});

	dispatch::link(acceptor, EPOLLIN, accept_ev);

	std::vector<std::thread> dns_threads_list(dns_threads);

	for (int i = 0; i < dns_threads; i++) {
		dns_threads_list[i] = init_dispatched_dns();
	}

	util::log() << "Started proxy server on port " << port;

	dispatch::run_dispatcher_in_current_thread();
}
