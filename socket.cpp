/*
 * socket.cpp
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#include "socket.h"

#include "util.h"

#include <cassert>
#include <array>

#include <unistd.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>

using namespace util;

int epoll_looper::epoll_holder::get_epoll_fd() {
	return epoll_fd;
}

epoll_looper::epoll_holder::epoll_holder() {
	epoll_fd = epoll_create1(0);
	name_fd(epoll_fd, "epoll");
}

epoll_looper::epoll_holder::~epoll_holder() {
	close(epoll_fd);
}

epoll_looper::epoll_looper() :
		epoll_obj(std::make_shared<epoll_holder>()) {
}

void epoll_looper::add_fd(int fd, uint32_t watch_events,
		const std::function<void(const uint32_t)>& on_event) {
	remove_fd(fd);
	const int epoll_fd = epoll_obj->get_epoll_fd();
	epoll_event watch_event { watch_events, { .fd = fd } }; // @suppress("Symbol is not resolved")
	action.insert( { fd, on_event });
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &watch_event);
}

void epoll_looper::remove_fd(int fd) {
	if (fd == current_fd) {
		remove_current_fd();
	} else {
		const int epoll_fd = epoll_obj->get_epoll_fd();
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
		action.erase(fd);
	}
}

void epoll_looper::remove_current_fd() {
	remove_requested = true;
}

void epoll_looper::loop() {
	for (;;) {
		loop_once(-1);
	}
}

void epoll_looper::loop_once(int timeout) {
	const int MAX_EPOLL_EVENTS = 200;
	const int epoll_fd = epoll_obj->get_epoll_fd();
	epoll_event events[MAX_EPOLL_EVENTS];
	int happened = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, timeout);
	for (int i = 0; i < happened; i++) {
		current_fd = events[i].data.fd;
		log << "Event on socket " << get_name(current_fd) << "\n";
		if (action.count(current_fd)) {
			remove_requested = false;
			action[current_fd](events[i].events);
			if (remove_requested) {
				remove_fd(current_fd);
			}
		}
	}
	current_fd = 0;
}

signal_handler::signal_handler(sigset_t signals) :
		signals(signals) {
	signal_fd = signalfd(-1, &signals, 0);
	name_fd(signal_fd, "signal");
	if (signal_fd == -1) {
		perror("signalfd creation");
		exit(-1);
	}
	sigprocmask(SIG_BLOCK, &signals, nullptr);
}

void signal_handler::add_sig(int sig, int sigcode,
		const std::function<void(const uint64_t&)>& action) {
	if (sigismember(&signals, sig)) {
		this->action[sig].insert( { sigcode, action });
	}
}

void signal_handler::remove_sig(int sig, int sigcode) {
	action[sig].erase(sigcode);
	if (action[sig].empty()) {
		action.erase(sig);
	}
}

void signal_handler::add_self_to_looper(epoll_looper& looper) {
	looper.add_fd(signal_fd, EPOLLIN, [this](const uint32_t) {read_signals();});
}

void signal_handler::remove_self_from_looper(epoll_looper& looper) {
	looper.remove_fd(signal_fd);
}

void signal_handler::read_signals() {
	signalfd_siginfo info;
	::read(signal_fd, &info, sizeof(info));
	if (action[info.ssi_signo].count(info.ssi_code)) {
		action[info.ssi_signo][info.ssi_code](info.ssi_ptr);
	}
}

signal_handler::~signal_handler() {
	sigprocmask(SIG_UNBLOCK, &signals, nullptr);
}

timer::timer(timespec timen) {
	timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
	name_fd(timer_fd, "timer");
	timer_time.it_value = timen;
	timer_time.it_interval = {0, 0};
	timerfd_settime(timer_fd, 0, &timer_time, nullptr);
	events = 0;
}

void timer::add_self_to_looper(epoll_looper& looper) {
	looper.add_fd(timer_fd, EPOLLIN, [this](const uint32_t) {
		loop_once();});
}

void timer::remove_self_from_looper(epoll_looper& looper) {
	looper.remove_fd(timer_fd);
}

int timer::add_action_once(const std::function<void()>& action, int delay) {
	int id = events++;
	actions.insert( { id, note(action, delay, true) });
	return id;
}

int timer::add_action_periodic(const std::function<void()>& action,
		int period) {
	int id = events++;
	actions.insert( { id, note(action, period, false) });
	return id;
}

void timer::remove_action(int id) {
	actions.erase(id);
}

void timer::loop_once() {
	timerfd_settime(timer_fd, 0, &timer_time, nullptr);
	for (auto it = actions.begin(); it != actions.end();) {
		note & n = it->second;
		n.elapsed++;
		if (n.elapsed == n.total) {
			n.action();
			if (n.total > 0) { // periodic
				n.elapsed = 0;
				it++;
			} else { // once
				it = actions.erase(it);
			}
		}
	}
}

timer::~timer() {
	::close(timer_fd);
}

ssize_t buffer::read_once(int fd) {
	static thread_local std::array<char, DEFAULT_READ_SIZE> buf;

	assert(fd > 0);

	ssize_t in = util::read(fd, buf.data(), (int) DEFAULT_READ_SIZE);

	if (in > 0) {
		add_to_buffer(buf.data(), in);
	} else if (in == 0) {
		in = -1;
		last_error = -1;
	} else {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			in = 0;
			break;
		default:
			last_error = errno;
			in = -1;
			perror("in buffer_read ");
			//log
			break;
		}
	}
	return in;
}

ssize_t buffer::write_once(int fd) {
	assert(fd > 0);

	const std::string_view chunk = get_from_buffer();

	if (chunk == nullptr || chunk.size() == 0) {
		return 0;
	}

	ssize_t out = util::send(fd, chunk.data(), chunk.size(),
	MSG_NOSIGNAL);

	if (out >= 0) {
		written(out);
		return out;
	} else {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			return 0;
		case ENOBUFS:
		case ENOMEM:
			perror("in buffer_write noncrit ");
			// log
			return 0;
		default:
			last_error = errno;
			perror("in buffer_write ");
			// log
			return -1;
		}
	}
}

void buffer::add_to_buffer_str(const std::string& str) {
	add_to_buffer(str.c_str(), str.length());
}

void buffer::reset() {
	last_error = 0;
}

bool buffer::eof() {
	return (last_error == -1);
}

int buffer::error() {
	return last_error;
}

size_t fd_buffer::size() {
	return buffer_size;
}

const std::string_view fd_buffer::get_from_buffer() {
	if (buf.size() == 0) {
		return std::string_view();
	}
	return std::string_view(buf.front());
}

void fd_buffer::written(ssize_t bytes) {
	buf.front().erase(buf.front().begin(), buf.front().begin() + bytes);
	buffer_size -= bytes;
	if (buf.front().empty()) {
		buf.pop_front();
	}
}

void fd_buffer::add_to_buffer(const char* data, size_t length) {
	last_read = std::string(data, length);
	buffer_size += length;
	while (length) {
		if (buf.size() == 0 || buf.back().size() == BUFFER_CHUNK_SIZE) {
			this->buf.emplace_back();
			this->buf.back().reserve(BUFFER_CHUNK_SIZE);
		}
		size_t range = std::min(length,
				(size_t) BUFFER_CHUNK_SIZE - buf.back().size());
		buf.back().append(data, data + range);
		length -= range;
		data += range;
	}
}

std::string fd_buffer::get_buffer_copy() {
	std::string result;
	result.reserve(buffer_size);
	for (auto & x : buf) {
		result.append(x);
	}
	return result;
}

const std::string & fd_buffer::get_last_read() {
	return last_read;
}

//void fd_buffer::skip_chars(size_t amount) {
//	if (amount >= buffer_size) {
//		buffer_size = 0;
//		buf.clear();
//	} else {
//		buffer_size -= amount;
//		while (amount) {
//			if (buf.front().size() <= amount) {
//				amount -= buf.front().size();
//				buf.pop_front();
//			} else {
//				buf.front() = buf.front().substr(amount, buf.front().size());
//			}
//		}
//	}
//}

void fd_buffer::reset() {
	buffer_size = 0;
	buf.clear();
	last_read.clear();
}

relay::relay(int in_fd, int out_fd, epoll_looper & loop) :
		in_fd(in_fd), out_fd(out_fd), st(READ_WRITE), loop(loop) {
}

void relay::loop_once() {
// TODO look at event
	ssize_t clr, clw;
	while (true) {
		assert(buf);
		switch (st) {
		case READ_WRITE:
			clr = clw = 0;

			if (!buf->eof()) {
				clr = buf->read_once(in_fd);
			}
			if (buf->size()) {
				clw = buf->write_once(out_fd);
			}

//			if (clr != 0 || clw != 0) {
//			log << in_fd << " -> " << out_fd << " : r " << clr << " w " << clw
//					<< " bufs " << buf->size() << "\n";
//			}
			if (clw < 0) {
				st = FINISHED;
				log << in_fd << " -> " << out_fd << " done\n";
			} else if (buf->eof()) {
				log << in_fd << " -> " << out_fd << " read all\n";
				close_fd(in_fd);
				st = WRITE_REST;
			} else if (clr == 0 && clw == 0) {
				return;
			}
			break;
		case WRITE_REST:
			clw = 0;
			if (buf->size()) {
				clw = buf->write_once(out_fd);
			}
//			log << in_fd << " -> " << out_fd << " r -2 w " << clw << " bufs "
//					<< buf->size() << "\n";
			if (clw < 0 || buf->size() == 0) {
				log << in_fd << " -> " << out_fd << " done\n";
				st = FINISHED;
			} else if (clw == 0) {
				return;
			}
			break;
		case FINISHED:
			close_fd(in_fd);
			close_fd(out_fd);
			finisher();
			return;
		}
	}
}

void relay::add_self_to_looper(epoll_looper& looper) {
	looper.add_fd(in_fd, EPOLLIN | EPOLLRDHUP | EPOLLET,
			[this](const uint32_t) {loop_once();});
	looper.add_fd(out_fd, EPOLLOUT | EPOLLRDHUP | EPOLLET,
			[this](const uint32_t) {loop_once();});
}

relay & relay::set_buffer(std::shared_ptr<buffer> data) {
	buf = data;
	return *this;
}

relay & relay::set_finisher(std::function<void()> on_finish) {
	finisher = on_finish;
	return *this;
}

relay& relay::set_socket_close(bool state) {
	do_close = state;
	return *this;
}

void relay::close_fd(int& fd) {
	if (fd != -1) {
		loop.remove_fd(fd);
		if (do_close) {
			::close(fd);
		}
		fd = -1;
	}
}

relay::~relay() {
	assert(st == FINISHED);
}

acceptor::acceptor(int port, const std::function<void(int)> & new_socket) :
		new_socket_processor(new_socket) {
	struct sockaddr_in srv_addr;
	accept_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (accept_fd == -1) {
		printf("Unable to open socket");
		throw new std::exception();
	}
	name_fd(accept_fd, "acceptor");

	fcntl(accept_fd, F_SETFL, fcntl(accept_fd, F_GETFL) | O_NONBLOCK);

	int incr = 1;

	setsockopt(accept_fd, SOL_SOCKET, SO_REUSEADDR, &incr, sizeof(incr));
	setsockopt(accept_fd, SOL_SOCKET, SO_REUSEPORT, &incr, sizeof(incr));

	srv_addr.sin_family = AF_INET;
	srv_addr.sin_addr.s_addr = INADDR_ANY;
	srv_addr.sin_port = htons((short) port);
	if (bind(accept_fd, (sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
		printf("Unable to bind to %d", port);
		throw new std::exception();
	}
	listen(accept_fd, 10);
}

void acceptor::try_accept() {
	struct sockaddr_in cli_addr;
	socklen_t cli_size = sizeof(cli_addr);
	int res;
	while ((res = accept4(accept_fd, (sockaddr *) &cli_addr, &cli_size,
	SOCK_NONBLOCK)) > 0) {
		name_fd(res, "client_" + std::to_string(res));
		new_socket_processor(res);
	}
	if (res == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
		return;
	} else {
		// TODO process errors
	}
}

void acceptor::add_self_to_looper(epoll_looper & looper) {
	looper.add_fd(accept_fd, EPOLLIN, [this](const uint32_t) {try_accept();});
}
void acceptor::remove_self_from_looper(epoll_looper& looper) {
	looper.remove_fd(accept_fd);
}

acceptor::~acceptor() {
	close(accept_fd);
}
