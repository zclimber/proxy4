#include "dispatch.h"

#include <sys/epoll.h>

#include <thread>
#include <unordered_set>

struct event {
	std::function<void()> action;
	int trigger_count = 0;
	bool recycle_marked = false;
	event(std::function<void()> func) :
			action(func) {
	}
};

struct link_holer {
	int event_id;
	int epoll_actions;
};

struct fd_hold {
	std::vector<link_holer> threads;
	int fd;
	bool recycle_marked = false;

	fd_hold(int fd) :
			fd(fd) {
	}
};

std::unordered_map<int, event> events;
std::unordered_map<int, fd_hold> fds;
std::unordered_set<int> armed;
int epoll_fd = epoll_create1(0);
int event_id_counter = 1;
int current_action = 0;

int dispatch::add_event(std::function<void()> action) {
	int current_number = event_id_counter++;
	events.insert( { current_number, event(action) });
	return current_number;
}

bool dispatch::add_fd(int fd, int epoll_mode) {
	auto it = fds.find(fd);
	if (it != fds.end()) {
		if (it->second.recycle_marked && it->second.threads.size() == 0) {
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
			fds.erase(it);
		} else {
			return false;
		}
	}
	fds.insert( { fd, fd_hold(fd) });
	epoll_event ev { fd, {.fd = epoll_mode} }; // @suppress("Symbol is not resolved")
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	return true;
}

void dispatch::link(int fd, int epoll_target, int event_id) {
	auto fdd = fds.find(fd);
	auto evd = events.find(event_id);
	link_holer l { event_id, epoll_target };
	if (fdd == fds.end()) {
		throw new std::exception(); // TODO
	}
	if (evd == events.end()) {
		throw new std::exception();
	}
	fdd->second.threads.push_back(l);
	evd->second.trigger_count++;
}

void dispatch::unlink(int fd, int event_id) {
	auto fdd = fds.find(fd);
	auto evd = events.find(event_id);
	if (fdd == fds.end()) {
		throw new std::exception(); // TODO
	}
	if (evd == events.end()) {
		throw new std::exception();
	}
	std::vector<link_holer> & vec = fdd->second.threads;
	for (unsigned i = 0; i < vec.size(); i++) {
		if (vec[i].event_id == event_id) {
			vec.erase(vec.begin() + i);
		}
	}
}

void dispatch::unlink_current(int fd) {
	if (current_action) {
		unlink(fd, current_action);
	}
}

void dispatch::recycle_event(int event_id) {
	auto evd = events.find(event_id);
	if (evd != events.end()) {
		evd->second.recycle_marked = true;
	}
}

void dispatch::recycle_fd(int fd) {
	auto fdd = fds.find(fd);
	if (fdd != fds.end()) {
		fdd->second.recycle_marked = true;
	}
}

void dispatch::arm_manual(int event_id) {
	armed.insert(event_id);
}

std::thread dispatcher;

void epoll_mark() {
	epoll_event poll[100];
	int ev = epoll_wait(epoll_fd, poll, 100, 0);
	for (int i = 0; i < ev; i++) {
		auto fdd = fds.find(poll[i].data.fd);
		for (auto x : fdd->second.threads) {
			if (x.epoll_actions & poll[i].events) {
				armed.insert(x.event_id);
			}
		}
	}
}

void run_events() {
	while (!armed.empty()) {
		current_action = *armed.begin();
		armed.erase(armed.begin());
		events.find(current_action)->second.action();
	}
	current_action = 0;
}

void gc() {
	for (auto it = events.begin(); it != events.end(); it++) {
		if (it->second.recycle_marked && it->second.trigger_count == 0) {
			it = events.erase(it);
		}
	}
	for (auto it = fds.begin(); it != fds.end(); it++) {
		if (it->second.recycle_marked && it->second.threads.size() == 0) {
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, it->first, nullptr);
			it = fds.erase(it);
		}
	}
}

void dispatch_loop() {
	time_t last_gc = time(nullptr);
	for (;;) {
		epoll_mark();
		run_events();
		if (last_gc + 5000 < time(nullptr)) {
			last_gc = time(nullptr);
			gc();
		}
	}
}

void dispatch::run_dispatcher_in_current_thread() {
	if (!dispatcher.joinable()) {
		dispatch_loop();
	}
}

void dispatch::create_dispatcher_thread() {
	dispatcher = std::thread(dispatch_loop);
}
