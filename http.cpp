/*
 * http.cpp
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#include "http.h"

#include "util.h"

#include <sstream>

#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

using namespace util;

bool header_parser::set_string(const std::string& s) {
	std::istringstream ss;
	ss.str(s);
	// select request string
	char nxt;
	std::getline(ss, request_str, '\r').get(nxt);
	if (nxt != '\n') {
		return false;
	}
	// select all headers
	for (std::string header;
			getline(ss, header, '\r').get(nxt) && header.size();) {
		if (nxt != '\n') {
			return false;
		}
		size_t colon = header.find(':');
		size_t nxt = colon + 1;
		while (isspace(header[nxt])) {
			nxt++;
		}
		headers_map.insert(
				{ header.substr(0, colon), header.substr(nxt, header.size()) });
	}
	excess_str = ss.str().substr(ss.tellg(), s.size());
	return true;
}

std::string header_parser::assemble_head() {
	std::ostringstream ss;
	ss << request_str << "\r\n";
	for (auto x : headers_map) {
		ss << x.first << ": " << x.second << "\r\n";
	}
	ss << "\r\n";
	return ss.str();
}

std::string& header_parser::request() {
	return request_str;
}

std::unordered_map<std::string, std::string>& header_parser::headers() {
	return headers_map;
}

std::string& header_parser::excess() {
	return excess_str;
}

void load_http_head(int sock, epoll_looper& loop, fd_buffer& buf,
		std::function<void()> on_load, std::function<void()> on_failure) {
	loop.add_fd(sock, EPOLLIN | EPOLLRDHUP | EPOLLET,
			[sock, &buf, &loop, on_load, on_failure](const uint32_t ev) {
				epoll_looper & looper = loop;
				if(ev & EPOLLIN) {
					bool end_found = false;
					while(buf.read_once(sock) > 0) {
						log << "Headers from " << sock << " : r " << buf.get_last_read().size() << "\n";
						end_found = end_found || buf.get_last_read().find("\r\n\r\n") != std::string::npos;
					}
					log << "Header errors from " << sock << " : eof " << buf.eof() << " error " << buf.error() << "\n";
					if(end_found) {
						on_load();
						looper.remove_current_fd();
					}
				} else {
					on_failure();
					looper.remove_current_fd();
				}
			});
}

void http_connector::try_connect() {
	int lookup_result = gai_error(&request);
	if (lookup_result == 0) {
		// TODO async connect

		timer_obj.remove_action(timer_event_id);

		addrinfo * current_addr = request.ar_result;
		int srv_fd;
		while (current_addr != nullptr) {
			srv_fd = socket(current_addr->ai_family, current_addr->ai_socktype,
					current_addr->ai_protocol);
			int conn_srv = connect(srv_fd, current_addr->ai_addr,
					current_addr->ai_addrlen);
			if (conn_srv == 0) {
				break;
			}
			close(srv_fd);
			current_addr = current_addr->ai_next;
		}
		freeaddrinfo(request.ar_result);

		if (current_addr == nullptr) {
			if (on_failure) {
				on_failure();
			}
		} else {
			fcntl(srv_fd, F_SETFL, fcntl(srv_fd, F_GETFL) | O_NONBLOCK);
			if (on_connect) {
				on_connect(srv_fd);
			}
		}
	} else {
		// TODO actions on different errors
		timer_obj.remove_action(timer_event_id);
		if (on_failure) {
			on_failure();
		}
	}
}

void http_connector::disable() {
	on_connect = std::function<void(int)>();
	on_failure = std::function<void()>();
}

http_connector::~http_connector() {
	disable();
	gai_cancel(&request);
}

constexpr addrinfo getaddrinfo_hint { 0, AF_INET, SOCK_STREAM, 0, 0, 0, 0,
		nullptr };

int connector_id = 0;
std::unordered_map<int, std::shared_ptr<http_connector>> connmap;

std::shared_ptr<http_connector> new_connector(const std::string & address,
		const std::string & port, signal_handler & loop, timer & timer_obj,
		const std::function<void(int)> & on_connect,
		const std::function<void()> & on_failure) {
	int curr_id = connector_id++;
	std::shared_ptr<http_connector> res(
			new http_connector(address, port, loop, timer_obj, on_connect,
					on_failure, curr_id));
	connmap.insert( { curr_id, res });
	return res;
}

http_connector::http_connector(const std::string& address_in,
		const std::string& port_in, signal_handler& sigloop, timer& timer_obj,
		const std::function<void(int)> & on_connect,
		const std::function<void()> & on_failure, int id) :
		self_id(id), timer_obj(timer_obj), address(address_in), port(port_in) {

	log << "Creating http_connector for host " << address << "\n";

	const int reject_delay = 15;

	timer_event_id = timer_obj.add_action_once([this, &sigloop, on_failure]() {
		int cancel_result = gai_cancel(&request);
		//TODO process cancel_result
		if(on_failure) {
			on_failure();
		}
		connmap.erase(self_id);
	}, reject_delay);

	sigloop.add_sig(SIGUSR1, SI_ASYNCNL, [](const uint64_t & data) {
		long long self_id = (long long) data;
		auto rs = connmap.find(self_id);
		if(rs != connmap.end()) {
			rs->second->try_connect();
			connmap.erase(rs);
		}});

	sigevent sig;
	sig.sigev_notify = SIGEV_SIGNAL;
	sig.sigev_signo = SIGUSR1;
	sig.sigev_value.sival_ptr = (void *) (long long) self_id;
	this->on_connect = on_connect;
	this->on_failure = on_failure;
	gaicb * request_ptr = &request;
	request.ar_name = address.c_str();
	request.ar_service = port.c_str();
	request.ar_request = &getaddrinfo_hint;
	getaddrinfo_a(GAI_NOWAIT, &request_ptr, 1, &sig);
}

const std::string_view http_chunked_buffer::get_from_buffer() {
	if (buf.size() == 0) {
		return std::string_view();
	}
	return std::string_view(buf.front());
}

void http_chunked_buffer::written(ssize_t bytes) {
	buf.front().erase(buf.front().begin(), buf.front().begin() + bytes);
	buffer_size -= bytes;
	if (buf.front().empty()) {
		buf.pop_front();
	}
}

#include <cassert>

void http_chunked_buffer::add_chunk_border(std::string_view & data) {
	size_t addsize = std::min((long long) data.size(), chunk_left);
	chunk_left -= addsize;
	std::string_view added_data = data;
	data.remove_prefix(addsize);
	added_data.remove_suffix(data.size());
	while (!added_data.empty()) {
		if (buf.size() == 0 || buf.back().size() == BUFFER_CHUNK_SIZE) {
			this->buf.emplace_back();
			this->buf.back().reserve(BUFFER_CHUNK_SIZE);
		}
		size_t range = std::min(added_data.size(),
				(size_t) BUFFER_CHUNK_SIZE - buf.back().size());
		buf.back().append(added_data.begin(), added_data.begin() + range);
		added_data.remove_prefix(range);
	}
}
void http_chunked_buffer::add_char(std::string_view & s) {
	if (buf.size() == 0 || buf.back().size() == BUFFER_CHUNK_SIZE) {
		this->buf.emplace_back();
		this->buf.back().reserve(BUFFER_CHUNK_SIZE);
	}
	buf.back().push_back(s[0]);
	s.remove_prefix(1);
}

void http_chunked_buffer::add_to_buffer(const char* data, size_t length) {
	buffer_size += length;
	std::string_view added(data, length);
	while (!added.empty()) {
		switch (bufstate) {
		case CHUNK_START:
			assert(added[0] >= '0' || added[0] <= 'F');
			chunk_left = added[0] - '0';
			add_char(added);
			bufstate = CHUNK_NUMBER;
			break;
		case CHUNK_NUMBER:
			if (added[0] >= '0' || added[0] <= 'F') {
				chunk_left = added[0] - '0';
				add_char(added);
			} else {
				bufstate = CHUNK_CR;
			}
			break;
		case CHUNK_CR:
			if (added[0] == '\r') {
				bufstate = CHUNK_LF;
			}
			add_char(added);
			break;
		case CHUNK_LF:
			assert(added[0] == '\n');
			add_char(added);
			if (chunk_left == 0) {
				bufstate = CHUNK_ZERO_CR;
			} else {
				bufstate = CHUNK_BODY;
			}
			break;
		case CHUNK_BODY:
			add_chunk_border(added);
			if (chunk_left == 0) {
				bufstate = CHUNK_BODY_CR;
			}
			break;
		case CHUNK_BODY_CR:
			assert(added[0] == '\r');
			add_char(added);
			bufstate = CHUNK_BODY_LF;
			break;
		case CHUNK_BODY_LF:
			assert(added[0] == '\n');
			add_char(added);
			bufstate = CHUNK_START;
			break;
		case CHUNK_ZERO_CR:
			assert(added[0] == '\r');
			add_char(added);
			bufstate = CHUNK_ZERO_LF;
			break;
		case CHUNK_ZERO_LF:
			assert(added[0] == '\n');
			add_char(added);
			bufstate = CHUNK_START;
			break;
		case CHUNK_TRAILER:
			if (added[0] == '\r') {
				bufstate = FINAL_CR;
			} else {
				bufstate = CHUNK_TRAILER_BODY;
			}
			break;
		case CHUNK_TRAILER_BODY:
			if (added[0] == '\r') {
				bufstate = CHUNK_TRAILER_CR;
			} else {
				add_char(added);
			}
			break;
		case CHUNK_TRAILER_CR:
			assert(added[0] == '\r');
			add_char(added);
			bufstate = CHUNK_TRAILER_LF;
			break;
		case CHUNK_TRAILER_LF:
			assert(added[0] == '\n');
			add_char(added);
			bufstate = CHUNK_TRAILER;
			break;
		case FINAL_CR:
			assert(added[0] == '\r');
			add_char(added);
			bufstate = FINAL_LF;
			break;
		case FINAL_LF:
			assert(added[0] == '\n');
			add_char(added);
			bufstate = DONE;
			break;
		case DONE:
			return;
		}
	}
}

//std::string http_chunked_buffer::get_buffer_copy() {
//}

void http_chunked_buffer::reset() {
	buf.clear();
	buffer_size = chunk_left = 0;
	bufstate = CHUNK_START;
}

size_t http_chunked_buffer::size() {
	return buffer_size;
}

bool http_chunked_buffer::eof() {
	return buffer::eof() || bufstate == DONE;
}

http_size_buffer::http_size_buffer(long long bytes) :
		bytes(bytes) {
}

void http_size_buffer::add_to_buffer(const char* data, size_t length) {
	bytes -= length;
	fd_buffer::add_to_buffer(data, length);
}

void http_size_buffer::reset() {
	bytes = -1;
}

http_size_buffer::http_size_buffer(std::string headers, long long bytes) :
		bytes(bytes) {
	buffer_size += headers.size();
	buf.push_back(headers);
}

bool http_size_buffer::eof() {
	return bytes <= 0 || fd_buffer::eof();
}

http_chunked_buffer::http_chunked_buffer(std::string headers) {
	buffer_size += headers.size();
	buf.push_back(headers);
}

//http_chunked_buffer::~http_chunked_buffer() {
//}

