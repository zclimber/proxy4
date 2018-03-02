/*
 * http.h
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#ifndef HTTP_H_
#define HTTP_H_

#include "socket.h"

#include <netdb.h>

#include <unordered_map>
#include <string>
#include <functional>

class header_parser {
	std::string request_str, excess_str;
	std::unordered_map<std::string, std::string> headers_map;
public:
	bool set_string(const std::string & s);
	std::string assemble_head();
	std::string & request();
	std::unordered_map<std::string, std::string> & headers();
	std::string & excess();
};

void load_http_head(int sock, epoll_looper & loop, fd_buffer & buf,
		std::function<void()> on_load, std::function<void()> on_failure);

class http_connector {
	gaicb request;
	int start_seconds = 0;
	int timer_event_id;
	int self_id;
	timer & timer_obj;
	std::string address, port;
	std::function<void(int)> on_connect;
	std::function<void()> on_failure;
	void try_connect();
	http_connector(const std::string & address, const std::string & port,
			signal_handler & loop, timer & timer_obj,
			const std::function<void(int)> & on_connect,
			const std::function<void()> & on_failure, int id);
public:
	http_connector(const http_connector &) = delete;
	http_connector(http_connector &&) = delete;
	void disable();
	~http_connector();
	friend std::shared_ptr<http_connector> new_connector(
			const std::string & address, const std::string & port,
			signal_handler & loop, timer & timer_obj,
			const std::function<void(int)> & on_connect,
			const std::function<void()> & on_failure);
};

std::shared_ptr<http_connector> new_connector(const std::string & address,
		const std::string & port, signal_handler & loop, timer & timer_obj,
		const std::function<void(int)> & on_connect,
		const std::function<void()> & on_failure);

class http_chunked_buffer: public buffer {
	enum state {
		CHUNK_START,
		CHUNK_NUMBER,
		CHUNK_CR,
		CHUNK_LF,
		CHUNK_BODY,
		CHUNK_BODY_CR,
		CHUNK_BODY_LF,
		CHUNK_ZERO_CR,
		CHUNK_ZERO_LF,
		CHUNK_TRAILER,
		CHUNK_TRAILER_BODY,
		CHUNK_TRAILER_CR,
		CHUNK_TRAILER_LF,
		FINAL_CR,
		FINAL_LF,
		DONE
	};
	static const int BUFFER_CHUNK_SIZE = 4096;
	std::deque<std::string> buf;
	size_t buffer_size = 0;
	long long chunk_left = 0;
	state bufstate = CHUNK_START;

	void add_chunk_border(std::string_view &);
	void add_char(std::string_view &);

	const std::string_view get_from_buffer();
	void written(ssize_t bytes);
public:
	http_chunked_buffer() = default;
	http_chunked_buffer(std::string headers);
	http_chunked_buffer(const http_chunked_buffer &) = delete;
	http_chunked_buffer(http_chunked_buffer &&) = default;

	virtual size_t size();

	virtual void add_to_buffer(const char * data, size_t length);
//	virtual std::string get_buffer_copy();
	virtual void reset();
	virtual bool eof();

	virtual ~http_chunked_buffer() = default;
};

class http_size_buffer: public fd_buffer {
	long long bytes;
public:
	http_size_buffer(long long bytes);
	http_size_buffer(std::string headers, long long bytes);
	http_size_buffer(const http_size_buffer &) = delete;
	http_size_buffer(http_size_buffer &&) = default;

	virtual void add_to_buffer(const char * data, size_t length);

	virtual void reset();
	virtual bool eof();

	virtual ~http_size_buffer() = default;

};
#endif /* HTTP_H_ */

