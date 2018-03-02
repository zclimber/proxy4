#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>
#include <thread>

#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "socket.h"
#include "http.h"
#include "util.h"

using util::log;

using std::string;

bool sync_load_headers(std::string & buf, int sock) {
	char t[4096];
	while (true) {
		int res = read(sock, t, sizeof(t));
		if (res < 0) {
			return false;
		} else {
			buf.append(t, t + res);
			auto find = std::string(t, res).find("\r\n\r\n");
			if (find != buf.npos) {
				return true;
			}
		}
	}
}

bool sync_load_fixed(std::string & buf, int sock, int length) {
	char t[4096];
	while (length > 0) {
		int res = read(sock, t, sizeof(t));
		if (res <= 0) {
			return false;
		} else {
			buf.append(t, t + res);
			length -= res;
		}
	}
	return true;
}

std::pair<int, int> get_num(const string & buf, int offs) {
	int rs = 0;
	while(offs < (int)buf.length() && isxdigit(buf[offs])){
		rs = rs * 16 + toupper(buf[offs]) - '0';
		offs++;
	}
	size_t newt = buf.find("\r\n");
	if(newt == buf.npos){
		return {-1, -1};
	} else {
		return {rs, (int)newt};
	}
}

bool sync_load_chunked(string & buf, int sock) {
	char t[4096];
	int cpos = buf.find("\r\n\r\n") + 4;
	int chunkl = 0;
	std::pair<int, int> gn = {0, 0};
	while (true) {
//		o << cpos << " " << chunkl << " " << gn.first << " " << gn.second << "\n";
		if (cpos >= (int)buf.size()) {
			int res = read(sock, t, sizeof(t));
			if (res <= 0) {
//				o << buf;
//				o.flush();
				log << "Error logged " << buf.size() << "\n";
//				exit(0);
				return false;
			} else {
				buf.append(t, t + res);
			}
		} else {
			if (chunkl > 0) {
				int mn = std::min(chunkl, (int) buf.length() - cpos);
				cpos += mn;
				chunkl -= mn;
			} else if (chunkl == 0) {
				if (gn.first != 0) {
					cpos = gn.first;
					gn.first = gn.second = 0;
				}
				gn = get_num(buf, cpos);
				if (gn.first < 0) {
					gn.first = cpos;
					cpos = buf.length();
					continue;
				}
				cpos = gn.second;
				if (gn.first > 0) {
					chunkl = gn.first + 2;
				} else {
					chunkl = -1;
				}
			} else {
				if (buf.find("\r\n\r\n", cpos - 3)) {
					return true;
				} else {
					cpos = buf.length();
				}
			}
		}
	}
}

int get_server_sock(const std::string & host, const std::string & port) {
	constexpr addrinfo getaddrinfo_hint { 0, AF_INET, SOCK_STREAM, 0, 0, 0, 0,
			nullptr };
	addrinfo *cr;
	int res = getaddrinfo(host.c_str(), port.c_str(), &getaddrinfo_hint, &cr);

	switch(res){
	case 0:
		break;
	case EAI_AGAIN:
		log << "Temporarily unable to resolve " << host << ":" << port << "\n";
		return -1;
	case EAI_NONAME:
		log << "Host " << host << ":" << port << " not found\n";
		return -1;
	case EAI_FAIL:
		log << "Failed to resolve " << host << ":" << port << "\n";
		return -1;
	case EAI_SERVICE:
		log << "Wrong service name in " << host << ":" << port << "\n";
		return -1;
	default:
		log << "   DNS ERROR: " << gai_strerror(res) << " " << strerror(errno) << "\n";
	}

	if(res != 0){

		log << " DNS ERROR " << gai_strerror(res) << "\n";
		log << host << "\n" << port << "\n";
	}

	int srv_fd;

	const addrinfo * result = cr;

	for (; cr != nullptr; cr = cr->ai_next) {
		srv_fd = socket(cr->ai_family, cr->ai_socktype, cr->ai_protocol);
		int conn_srv = connect(srv_fd, cr->ai_addr, cr->ai_addrlen);
		if (conn_srv == 0) {
			break;
		}
		close(srv_fd);
	}
	freeaddrinfo(const_cast<addrinfo*>(result));
	if (cr == nullptr) {
		return -1;
	} else {
		return srv_fd;
	}
}

bool sync_upload(const std::string & buf, int sock) {
	size_t offs = 0;
	while (offs < buf.size()) {
		int res = write(sock, buf.c_str() + offs, buf.size() - offs);
		if (res < 0) {
			return false;
		} else {
			offs += res;
		}
	}
	return true;
}

class proxy_connection {
	int client_sock;
	string buf;
public:
	proxy_connection(int client_sock) :
			client_sock(client_sock) {
	}
	void load_request_headers() {
		log << "Start loading request headers on socket " << client_sock
				<< "\n";
		sync_load_headers(buf, client_sock);
		process_request_headers();
	}
	void process_request_headers() {
		log << "Got headers on socket " << client_sock
				<< "\n";
		header_parser hp;
		hp.set_string(buf);
		std::string host = hp.headers()["Host"];
		std::string port = "80";
		std::size_t colon = host.find(':');
		if (colon != std::string::npos) {
			port = host.substr(colon + 1, host.length());
			host = host.substr(0, colon);
		}
		log << "Connecting to server " << host << ":" << port
				<< "\n";
		int server_sock = get_server_sock(host, port);
		if(server_sock == -1){
			log << "Cannot connect to server " << host << ":" << port << "\n";
			close(client_sock);
			return;
		}
		log << "Connected to server " << host << ":" << port << " at socket " << server_sock
				<< "\n";
		if (hp.request().compare(0, 7, "CONNECT") == 0) {
			start_connect_tunnel(server_sock);
		} else {
			upload_request_to_server(server_sock);
		}
	}
	void upload_request_to_server(int server_sock) {
		header_parser hp;
		hp.set_string(buf);
		hp.headers()["Connection"] = "close";
		buf = hp.assemble_head() + hp.excess();
		if (hp.headers().count("Transfer-Encoding")
				&& hp.headers()["Transfer-Encoding"].find("chunked")
						!= std::string::npos) {
			// chunked
			sync_load_chunked(buf, client_sock);
		} else if (hp.headers().count("Content-Length")) {
			sync_load_fixed(buf, client_sock,
					stol(hp.headers()["Content-Length"]) - hp.excess().length());
			// body
		} else {
			// no body
		}


		log << "Downloaded client request from " << client_sock << " " << buf.length() << " bytes\n";

		sync_upload(buf, server_sock);
		buf.clear();

		log << "Uploaded client request from " << client_sock << " to " << server_sock << "\n";

		sync_load_headers(buf, server_sock);
		process_response_headers(server_sock);
	}

	void process_response_headers(int server_sock) {
		log << "Got response from server at " << server_sock << "\n";
		header_parser hp;
		hp.set_string(buf);
		hp.headers()["Connection"] = "close";
		buf = hp.assemble_head() + hp.excess();

		if (hp.headers().count("Transfer-Encoding")
				&& hp.headers()["Transfer-Encoding"].find("chunked")
						!= std::string::npos) {
			// chunked
			sync_load_chunked(buf, server_sock);
		} else if (hp.headers().count("Content-Length")) {
			// fixed length body
			sync_load_fixed(buf, server_sock,
					stol(hp.headers()["Content-Length"]));
		} else {
			sync_load_fixed(buf, server_sock, INT_LEAST32_MAX);
			// pump until ends
		}

		log << "Downloaded server response from " << server_sock << " " << buf.length() << " bytes\n";

		sync_upload(buf, client_sock);

		log << "Uploaded server response from " << server_sock << " to " << client_sock << "\n";

		close(server_sock);
		close(client_sock);
	}

	void start_connect_tunnel(int server_sock) {
		close(server_sock);
		close(client_sock);
		return;


		header_parser hp;
		hp.request() = "HTTP/1.1 200 Connection established";
		hp.headers()["Proxy-agent"] = "mylittleproxy 0.1";
		log << "Started http tunnel between " << client_sock << " and "
				<< server_sock << "\n";
	}

	void fail_loading_client() {
		// TODO
		log << "failed loading client fd " << client_sock << "\n";
		close(client_sock);
	}
	void fail_connecting_to_server(int server_sock) {
		// TODO
		header_parser hp;
		if (server_sock > 0) {
			hp.request() = "HTTP/1.1 502 Bad Gateway";
		} else {
			hp.request() = "HTTP/1.1 504 Gateway Timeout";
			close(server_sock);
		}
		log << "failed connection to server with client fd " << client_sock
				<< "\n";
		close(client_sock);
	}
	~proxy_connection() {
	}
};

void connection_proc(int client_socket){
	proxy_connection conn(client_socket);
	conn.load_request_headers();
}

int main(int argc, char** argv) {
	if (argc <= 1) {
		printf("Usage: proxy port\n");
		exit(0);
	}
	int port = atoi(argv[1]);

	int accept_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (accept_fd == -1) {
		printf("Unable to open socket");
		throw new std::exception();
	}

	int incr = 1;
	setsockopt(accept_fd, SOL_SOCKET, SO_REUSEADDR, &incr, sizeof(incr));
	setsockopt(accept_fd, SOL_SOCKET, SO_REUSEPORT, &incr, sizeof(incr));

	struct sockaddr_in srv_addr;
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_addr.s_addr = INADDR_ANY;
	srv_addr.sin_port = htons((short) port);
	if (bind(accept_fd, (sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
		printf("Unable to bind to %d", port);
		throw new std::exception();
	}
	listen(accept_fd, 10);

	for(;;){
		struct sockaddr_in cli_addr;
		socklen_t cli_size = sizeof(cli_addr);
		int new_client = accept(accept_fd, (sockaddr *) &cli_addr, &cli_size);
		std::thread thr(connection_proc, new_client);
		thr.detach();
	}
}
