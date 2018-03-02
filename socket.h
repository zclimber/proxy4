/*
 * socket.h
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#ifndef SOCKET_H_
#define SOCKET_H_

#include <functional>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <deque>
#include <string_view>

#include <sys/epoll.h>

class epoll_looper {
	class epoll_holder {
		int epoll_fd;
	public:
		int get_epoll_fd();
		epoll_holder();
		~epoll_holder();
	};
	std::shared_ptr<epoll_holder> epoll_obj;
	bool remove_requested = false;
	int current_fd = 0;
	std::unordered_map<int, const std::function<void(const uint32_t)>> action;
public:
	epoll_looper();
	epoll_looper(const epoll_looper & oth) = delete;
	epoll_looper(epoll_looper && oth) = default;

	void add_fd(int fd, uint32_t watch_event,
			const std::function<void(const uint32_t)> & action);
	void remove_fd(int fd);
	void remove_current_fd();

	void loop_once(int timeout);
	void loop();
};

class signal_handler {
	int signal_fd;
	sigset_t signals;
	std::unordered_map<int,
			std::unordered_map<int, const std::function<void(const uint64_t &)>>> action;
public:
	signal_handler(sigset_t signals);
	signal_handler(const signal_handler & oth) = delete;
	signal_handler(signal_handler && oth) = delete;

	void add_sig(int signal, int sigcode,
			const std::function<void(const uint64_t &)> & action);
	void remove_sig(int signal, int sigcode);

	void add_self_to_looper(epoll_looper & looper);
	void remove_self_from_looper(epoll_looper& looper);

	void read_signals();

	~signal_handler();
};

class timer {
	struct note {
		std::function<void()> action;
		int elapsed = -1;
		int total = 0;
		note() = default;
		note(const note & oth) = default;
		note(note && oth) = default;
		note(std::function<void()> action, int time, bool once) :
				action(action) {
			if (once) {
				elapsed = -time;
				total = 0;
			} else {
				elapsed = 0;
				total = time;
			}
		}
	};
	int timer_fd;
	int events;
	itimerspec timer_time;
	std::unordered_map<int, note> actions;
	void loop_once();
public:
	timer(timespec time);

	int add_action_once(const std::function<void()> & action, int delay);
	int add_action_periodic(const std::function<void()> & action, int period);
	void remove_action(int id);
	void add_self_to_looper(epoll_looper & looper);
	void remove_self_from_looper(epoll_looper& looper);

	~timer();
};

class buffer {
protected:
	int last_error = 0;
	virtual const std::string_view get_from_buffer() = 0;
	virtual void written(ssize_t bytes) = 0;

protected:
	buffer() = default;
public:
	static const int DEFAULT_READ_SIZE = 4096;
	virtual size_t size() = 0;

	ssize_t read_once(int fd);
	ssize_t write_once(int fd);
	virtual void add_to_buffer(const char * data, size_t length) = 0;
	virtual void add_to_buffer_str(const std::string & str);
//	virtual std::string get_buffer_copy() = 0;
	virtual void reset();

	virtual bool eof();
	int error();

	virtual ~buffer() = default;
};

class fd_buffer: public buffer {
protected:
	static const int BUFFER_CHUNK_SIZE = 4096;
	std::deque<std::string> buf;
	std::string last_read;
	size_t buffer_size = 0;

	const std::string_view get_from_buffer();
	void written(ssize_t bytes);
public:
	fd_buffer() :
			buffer() {
	}
	fd_buffer(const fd_buffer &) = delete;
	fd_buffer(fd_buffer && oth) = default;

	size_t size();
	void add_to_buffer(const char * data, size_t length);
	std::string get_buffer_copy();
	const std::string & get_last_read();

	void reset();

	~fd_buffer() = default;
};

class relay {
	enum state {
		READ_WRITE, WRITE_REST, FINISHED
	};
	int in_fd, out_fd;
	state st;
	std::shared_ptr<buffer> buf;
	std::function<void()> finisher;
	epoll_looper & loop;
	bool do_close = false;

	void add_self_to_looper(epoll_looper & looper);
	void close_fd(int & fd);

public:
	void loop_once();

	relay(int in_fd, int out_fd, epoll_looper & loop);

	relay & set_buffer(std::shared_ptr<buffer> data);
	relay & set_finisher(std::function<void()> on_finish);
	relay & set_socket_close(bool state);

	~relay();
};

class acceptor {
	int accept_fd;
	std::function<void(int)> new_socket_processor;
public:
	acceptor(int port, const std::function<void(int)> & new_socket);
	acceptor(const acceptor &) = delete;
	acceptor(acceptor && oth) = default;

	void try_accept();

	void add_self_to_looper(epoll_looper & looper);
	void remove_self_from_looper(epoll_looper& looper);

	~acceptor();
};

#endif /* SOCKET_H_ */
