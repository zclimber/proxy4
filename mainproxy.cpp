#include <asm-generic/socket.h>
#include <bits/exception.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dispatch.h"
#include "http.h"
#include "relay.h"
#include "util.h"

using std::string;

#include "dns_dispatched.h"

#include "loaders.h"

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
		util::log() << "Start loading request headers on socket "
				<< client_sock.fd();
		auto xfut = async_load::headers(buf, client_sock, event_vec.back(),
				event_vec[0]);
//		auto t = xfut.wait_for(std::chrono::seconds(20));
//		if (t == std::future_status::timeout || xfut.get() == -1) {
//			return;
//		}
//		process_request_headers();
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
//		log << "Connecting to server " << host << ":" << port << "\n";

		event_vec.push_back(dispatch::event_ref());

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_request_headers_2,
						shared_from_this(), hp));

		fut = connect_to_remote_server(host, port, event_vec.back());

//		process_request_headers_2(hp);
	}
	void process_request_headers_2(header_parser hp) {
		int ssock = fut.get();
		if (ssock == -1) {
			util::log() << "Cannot connect to server " << hp.headers()["Host"];
			cleanup();
		}
		server_sock = dispatch::fd_ref(ssock,
		EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
		util::log() << hp.request() << " " << client_sock.fd() << " -> "
				<< server_sock.fd();
		if (server_sock.fd() == -1) {
			fail_connecting_to_server();
			return;
		}
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
		std::future<int> xfut;

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::upload_request_to_server_2,
						shared_from_this()));

		if (hp.headers().count("Transfer-Encoding")
				&& hp.headers()["Transfer-Encoding"].find("chunked")
						!= std::string::npos) {
			// chunked

			xfut = async_load::chunked(buf, client_sock, event_vec.back(),
					event_vec[0]);
		} else if (hp.headers().count("Content-Length")) {
			xfut = async_load::fixed(buf, client_sock,
					stol(hp.headers()["Content-Length"]) - hp.excess().length(),
					event_vec.back(), event_vec[1]);
			// body
		} else {
			upload_request_to_server_2();
			return;
//			std::promise<int> pr;
//			pr.set_value(0);
//			xfut = pr.get_future();
			// no body
		}

//		std::future_status t = xfut.wait_for(std::chrono::seconds(20));
//		if (t == std::future_status::timeout || xfut.get() == -1) {
//			return;
//		}
//
////		log << "Downloaded client request from " << client_sock << " "
////				<< buf.length() << " bytes\n";
//
//		upload_request_to_server_2();
	}
	void upload_request_to_server_2() {

//		event_vec.push_back(dispatch::event_ref());

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::upload_request_to_server_3,
						shared_from_this()));

		std::future<int> xfut = async_load::upload(buf, server_sock,
				event_vec.back(), event_vec[1]);
//		auto tw = xfut.wait_for(std::chrono::seconds(20));
//		if (tw == std::future_status::timeout || xfut.get() == -1) {
//			return;
//		}
//
//		upload_request_to_server_3();
	}
	void upload_request_to_server_3() {

//		log << "Uploaded client request from " << client_sock << " to "
//				<< server_sock << "\n";
		buf.clear();

//		event_vec.push_back(dispatch::event_ref());

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_response_headers,
						shared_from_this()));
		std::future<int> xfut = async_load::headers(buf, server_sock,
				event_vec.back(), event_vec[0]);
//		auto t = xfut.wait_for(std::chrono::seconds(20));
//		if (t == std::future_status::timeout || xfut.get() == -1) {
//			return;
//		}

//		process_response_headers();
	}

	void process_response_headers() {
		util::log() << "Got response from server at " << server_sock.fd();
		header_parser hp;
		hp.set_string(buf);
		hp.headers()["Connection"] = "close";
		buf = hp.assemble_head() + hp.excess();
		std::future<int> xfut;

//		event_vec.push_back(dispatch::event_ref());

		event_vec.emplace_back( // @suppress("Ambiguous problem")
				std::bind(&proxy_connection::process_response_headers_2,
						shared_from_this()));
		if (hp.headers().count("Transfer-Encoding")
				&& hp.headers()["Transfer-Encoding"].find("chunked")
						!= std::string::npos) {
			// chunked

			xfut = async_load::chunked(buf, server_sock, event_vec.back(),
					event_vec[1]);
		} else if (hp.headers().count("Content-Length")) {
			// fixed length body
			xfut = async_load::fixed(buf, server_sock,
					stol(hp.headers()["Content-Length"]) - hp.excess().length(),
					event_vec.back(), event_vec[1]);
		} else {
			xfut = async_load::fixed(buf, server_sock,
			INT_LEAST32_MAX, event_vec.back(), event_vec.back());
			// pump until ends
		}
//		t = xfut.wait_for(std::chrono::seconds(20));
//		if (t == std::future_status::timeout || (!pass && xfut.get() == -1)) {
//			return;
//		}
//
//
//		process_response_headers_2();

	}
	void process_response_headers_2() {
//		log << "Downloaded server response from " << server_sock.fd() << " "
//				<< buf.length() << " bytes\n";

//		event_vec.push_back(dispatch::event_ref());
		auto thisptr = shared_from_this();
		event_vec.emplace_back( // @suppress("Ambiguous problem")
				[this, thisptr] {
					util::log() << "Uploaded server response from " << server_sock.fd() << " to "
					<< client_sock.fd();
					cleanup();
				});
		auto xfut = async_load::upload(buf, client_sock, event_vec.back(),
				event_vec[1]);
//		auto tw = xfut.wait_for(std::chrono::seconds(20));
//		if (tw == std::future_status::timeout || xfut.get() == -1) {
//			return;
//		}
//
//		log << "Uploaded server response from " << server_sock.fd() << " to "
//				<< client_sock.fd() << "\n";
//		cleanup();
	}

	void start_connect_tunnel() {
		header_parser hp;
		hp.request() = "HTTP/1.1 200 Connection established";
		hp.headers()["Proxy-agent"] = "mylittleproxy 0.1";
		std::string newbuf = buf;
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
		util::log() << "Started http tunnel between " << client_sock.fd()
				<< " and " << server_sock.fd();
	}

	void fail_loading_client() {
		// TODO
		util::log() << "Failed loading client fd " << client_sock.fd();
		cleanup();
	}
	void fail_connecting_to_server() {
		// TODO
		header_parser hp;
		if (server_sock.fd() > 0) {
			hp.request() = "HTTP/1.1 502 Bad Gateway";
		} else {
			hp.request() = "HTTP/1.1 504 Gateway Timeout";
		}
		util::log() << "failed connection to server " << server_sock.fd()
				<< " with client fd " << client_sock.fd();
		cleanup();
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

std::mutex mtx;

void connection_proc(int client_socket) {
//	std::lock_guard<std::mutex> lg(mtx);
	auto prox = std::make_shared<proxy_connection>(client_socket);
	prox->start();
//	lg.~lock_guard();
	std::this_thread::sleep_for(std::chrono::seconds(10));
}

int main(int argc, char** argv) {
	if (argc <= 1) {
		printf("Usage: proxy port\n");
		exit(0);
	}
	int port = atoi(argv[1]);
	if(port < 1 || port > UINT16_MAX){
		printf("Invalid port number %s", argv[1]);
		exit(0);
	}
	int accept_fd = socket(AF_INET, SOCK_STREAM, 0);

	if (accept_fd == -1) {
		printf("Unable to open socket");
		exit(0);
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

	util::log() << "Started proxy server on port " << port;

	dispatch::run_dispatcher_in_current_thread();
}
