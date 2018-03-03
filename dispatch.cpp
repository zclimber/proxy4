#include "dispatch.h"
#include "util.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <thread>
#include <unordered_set>
#include <mutex>

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

static std::unordered_map<int, event> events;
static std::unordered_map<int, fd_hold> fds;
static std::unordered_set<int> armed;
static int epoll_fd = epoll_create1(0);
static int manual_fd = eventfd(0, EFD_CLOEXEC);
static int event_id_counter = 1;
static int current_action = 0;
static std::mutex data_mutex;
static std::recursive_mutex armed_mutex;

int dispatch::add_event(std::function<void()> action) {
	std::lock_guard<std::mutex> lg(data_mutex);
	int current_number = event_id_counter++;
	events.insert( { current_number, event(action) });
	return current_number;
}

bool dispatch::add_fd(int fd, int epoll_mode) {
	std::lock_guard<std::mutex> lg(data_mutex);
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
	epoll_event ev { epoll_mode, { .fd = fd } }; // @suppress("Symbol is not resolved")
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	return true;
}

void dispatch::link(int fd, int epoll_target, int event_id) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto fdd = fds.find(fd);
	auto evd = events.find(event_id);
	link_holer l { event_id, epoll_target };
	if (fdd == fds.end()) {
		throw new std::invalid_argument("Linking unregistered fd");
	}
	if (evd == events.end()) {
		throw new std::invalid_argument("Linking unregistered event");
	}
	fdd->second.threads.push_back(l);
	evd->second.trigger_count++;
}

void dispatch::unlink(int fd, int event_id) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto fdd = fds.find(fd);
	auto evd = events.find(event_id);
	if (fdd == fds.end()) {
		throw new std::invalid_argument("Unlinking unregistered fd");
	}
	if (evd == events.end()) {
		throw new std::invalid_argument("Unlinking unregistered event");
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
	std::lock_guard<std::mutex> lg(data_mutex);
	auto evd = events.find(event_id);
	if (evd != events.end()) {
		evd->second.recycle_marked = true;
	}
}

void dispatch::recycle_fd(int fd) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto fdd = fds.find(fd);
	if (fdd != fds.end()) {
		fdd->second.recycle_marked = true;
	}
}

void dispatch::recycle_event_current() {
	if (current_action) {
		recycle_event(current_action);
	}
}

void dispatch::arm_manual(int event_id) {
	std::unique_lock<std::recursive_mutex> arm(armed_mutex);
	if (events.count(event_id)) {
		armed.insert(event_id);
		eventfd_write(manual_fd, 1);
	}
}

static std::thread dispatcher;

void epoll_mark() {
	epoll_event poll[1000];
	int ev = epoll_wait(epoll_fd, poll, 1000, -1);
	std::lock_guard<std::mutex> lg(data_mutex);
	std::unique_lock<std::recursive_mutex> arm(armed_mutex);
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
	std::unique_lock<std::recursive_mutex> arm(armed_mutex);
	while (!armed.empty()) {
		current_action = *armed.begin();
		armed.erase(armed.begin());
		auto it = events.find(current_action);
		if (it != events.end()) {
			it->second.action();
		}
	}
	current_action = 0;
}

void gc() {
	std::lock_guard<std::mutex> lg(data_mutex);
	for (auto it = events.begin(); it != events.end(); it++) {
		if (it->second.recycle_marked && it->second.trigger_count == 0) {
			it = events.erase(it);
		}
	}
	for (auto it = fds.begin(); it != fds.end(); ) {
		if (it->second.recycle_marked && it->second.threads.size() == 0) {
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, it->first, nullptr);
			it = fds.erase(it);
		} else {
			it++;
		}
	}
}

void dispatch_loop() {
	dispatch::add_fd(manual_fd, EPOLLIN);
	int read_manual = dispatch::add_event([] {
		unsigned long int l;
		eventfd_read(manual_fd, &l);
	});
	dispatch::link(manual_fd, EPOLLIN, read_manual);
	for (int cntr = 1;; cntr++) {
		epoll_mark();
		run_events();
		if (cntr % 10 == 0) {
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
