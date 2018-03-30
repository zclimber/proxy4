#include "relay.h"
#include "util.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <memory>
#include <cassert>

void relay::loop_once() {
	ssize_t clr, clw;
	int clr_errno, clw_errno;
	char t[8192];
	while (true) {
		auto log = util::log();
		errno = 0;
		clr = clw = 0;
		switch (st) {
		case READ_WRITE:

			clr = recv(in_fd.fd(), t, sizeof(t), MSG_DONTWAIT);

			if (clr > 0) {
				buf.append(t, t + clr);
			}

			clr_errno = errno;
			errno = 0;

			clw = send(out_fd.fd(), buf.c_str(), buf.size(), MSG_DONTWAIT);

			if (clw > 0) {
				buf.erase(buf.begin(), buf.begin() + clw);
			}

			clw_errno = errno;
			errno = 0;


			log << "Relay " << in_fd << " -> " << out_fd << " in "
					<< clr << " out " << clw << " bufs " << buf.size() << util::newl;

			if (clw < 0) {
				st = FINISHED;
				log << "Relay " << in_fd << " -> " << out_fd
						<< " done";
			} else if (clr == 0 || (clr == -1 && clr_errno != EAGAIN)) {
				log << "Relay " << in_fd << " -> " << out_fd
						<< " read all";
				st = WRITE_REST;
			} else if (clr_errno == EAGAIN
					&& (buf.size() == 0 || clw_errno == EAGAIN)) {
				return;
			}
			break;
		case WRITE_REST:
			clw = 0;
			if (buf.size()) {
				clw = send(out_fd.fd(), buf.c_str(), buf.size(), MSG_DONTWAIT);
				clw_errno = errno;
				errno = 0;
			}

			if (buf.empty() || (clw_errno == -1 && clw_errno != EAGAIN)) {
				log << "Relay " << in_fd << " -> " << out_fd
						<< " done";
				st = FINISHED;
			} else if (clw_errno == EAGAIN) {
				return;
			}
			break;
		case FINISHED:
			dispatch::unlink_current(in_fd);
			dispatch::unlink_current(out_fd);
			dispatch::recycle_event_current();
			finisher();
			return;
		}
	}
}

relay & relay::set_finisher(std::function<void()> on_finish) {
	finisher = on_finish;
	return *this;
}

relay::~relay() {
	assert(st == FINISHED);
}

relay::relay(dispatch::fd_ref & in_fd, dispatch::fd_ref & out_fd) :
		in_fd(in_fd), out_fd(out_fd), st(READ_WRITE) {
}

relay & relay::set_buffer(std::string && data) {
	buf = data;
	return *this;
}

relay& relay::set_buffer(const std::string& data) {
	buf = data;
	return *this;
}

void make_relay(dispatch::fd_ref& in_fd, dispatch::fd_ref& out_fd,
		std::string data, std::function<void()> on_finish) {
	auto ptr = std::make_shared<relay>(in_fd, out_fd);
	ptr->set_buffer(data).set_finisher(on_finish);

	dispatch::event_ref d(std::bind(&relay::loop_once, ptr));
	dispatch::link(in_fd, EPOLLIN | EPOLLRDHUP | EPOLLHUP, d);
	dispatch::link(out_fd, EPOLLOUT | EPOLLRDHUP | EPOLLHUP, d);
	dispatch::arm_manual(d);
}
