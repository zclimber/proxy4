#include "dispatch.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "util.h"

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

int add_event(std::function<void()> action) {
	std::lock_guard<std::mutex> lg(data_mutex);
	int current_number = event_id_counter++;
	events.insert( { current_number, event(action) });
	return current_number;
}

void add_fd(int fd, int epoll_mode) {
	if (fd == -1) {
		return;
	}
	std::lock_guard<std::mutex> lg(data_mutex);
	auto it = fds.find(fd);
	if (it != fds.end()) {
		if (it->second.recycle_marked && it->second.threads.size() == 0) {
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
			fds.erase(it);
		} else {
			auto log = util::log();
			log << "Descriptor " << std::to_string(fd)
					<< " already added to dispatch" << util::newl;
			log << "It can" << (it->second.recycle_marked ? "   " : "not")
					<< "be recycled and has "
					<< std::to_string(it->second.threads.size()) << " triggers";
			throw new std::logic_error(
					"Descriptor " + std::to_string(fd)
							+ " already added to dispatch");
		}
	}
	fds.insert( { fd, fd_hold(fd) });
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
	epoll_event ev { epoll_mode, { .fd = fd } }; // @suppress("Symbol is not resolved")
#pragma GCC diagnostic pop
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	return;
}

void link(int fd, int epoll_target, int event_id) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto fdd = fds.find(fd);
	auto evd = events.find(event_id);
	link_holer l { event_id, epoll_target };
	if (fdd == fds.end()) {
		util::log() << "Linking unregistered fd to event " << event_id;
		throw new std::invalid_argument("Linking unregistered fd");
	}
	if (evd == events.end()) {
		util::log() << "Linking unregistered event to fd " << fd;
		throw new std::invalid_argument("Linking unregistered event");
	}
	fdd->second.threads.push_back(l);
	evd->second.trigger_count++;
}

void unlink(int fd, int event_id) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto fdd = fds.find(fd);
	auto evd = events.find(event_id);
	if (fdd == fds.end()) {
		util::log() << "Unlinking unregistered fd from event " << event_id;
		throw new std::invalid_argument("Unlinking unregistered fd");
	}
	if (evd == events.end()) {
		util::log() << "Unlinking unregistered event from fd " << fd;
		throw new std::invalid_argument("Unlinking unregistered event");
	}
	std::vector<link_holer> & vec = fdd->second.threads;
	for (unsigned i = 0; i < vec.size(); i++) {
		if (vec[i].event_id == event_id) {
			vec.erase(vec.begin() + i);
			evd->second.trigger_count--;
			break;
		}
	}
}

void unlink_current(int fd) {
	if (current_action) {
		unlink(fd, current_action);
	}
}

void recycle_event(int event_id) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto evd = events.find(event_id);
	if (evd != events.end()) {
		evd->second.recycle_marked = true;
	}
}

void recycle_fd(int fd) {
	std::lock_guard<std::mutex> lg(data_mutex);
	auto fdd = fds.find(fd);
	if (fdd != fds.end()) {
		fdd->second.recycle_marked = true;
		for (auto x : fdd->second.threads) {
			events.find(x.event_id)->second.trigger_count--;
		}
	}
}

void recycle_event_current() {
	if (current_action) {
		recycle_event(current_action);
	}
}

void arm_manual(int event_id) {
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
	int ev_start = events.size(), fd_start = fds.size();
	int ev_hastriggers = 0, ev_notrecycled = 0;
	for (auto it = events.begin(); it != events.end();) {
		if (it->second.recycle_marked && it->second.trigger_count == 0) {
			it = events.erase(it);
		} else {
			if (it->second.recycle_marked) {
				ev_hastriggers++;
			} else if (it->second.trigger_count == 0) {
				ev_notrecycled++;
			}
			it++;
		}
	}
	for (auto it = fds.begin(); it != fds.end();) {
		if (it->second.recycle_marked && it->second.threads.size() == 0) {
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, it->first, nullptr);
			it = fds.erase(it);
		} else {
			it++;
		}
	}
	int ev_end = events.size(), fd_end = fds.size();
	auto log = util::log();
	log << "GC Events: " << ev_start - ev_end << " deleted, " << ev_end
			<< " left";
	log << "GC Files : " << fd_start - fd_end << " deleted, " << fd_end
			<< " left";
}

void dispatch_loop() {
	add_fd(manual_fd, EPOLLIN);
	int read_manual = add_event([] {
		unsigned long int l;
		eventfd_read(manual_fd, &l);
	});
	link(manual_fd, EPOLLIN, read_manual);
	for (int cntr = 1;; cntr++) {
		epoll_mark();
		run_events();
		if (cntr % 10 == 0) {
			gc();
		}
	}
}

namespace dispatch {

void run_dispatcher_in_current_thread() {
	if (!dispatcher.joinable()) {
		dispatch_loop();
	}
}

void link(const fd_ref& fd, int epoll_target, const event_ref& ev) {
	::link(fd.fd(), epoll_target, ev.id());
}

void unlink(const fd_ref&fd, const event_ref&ev) {
	::unlink(fd.fd(), ev.id());
}

void unlink_current(const fd_ref& fd) {
	::unlink_current(fd.fd());
}

void recycle_event(const event_ref& ev) {
	::recycle_event(ev.id());
}

void recycle_event_current() {
	::recycle_event_current();
}

void recycle_fd(const fd_ref& fd) {
	::recycle_fd(fd.fd());
}

void arm_manual(const event_ref& ev) {
	::arm_manual(ev.id());
}

void create_dispatcher_thread() {
	dispatcher = std::thread(dispatch_loop);
}

}

dispatch::event_ref::event_ref() :
		event_id(-1) {
}

dispatch::event_ref::event_ref(const std::function<void()> & event) {
	event_id = add_event(event);
}

int dispatch::event_ref::id() const {
	return event_id;
}

void dispatch::event_ref::recycle() {
	if (event_id != -1) {
		::recycle_event(event_id);
		event_id = -1;
	}
}

dispatch::event_ref::event_ref(event_ref&& oth) {
	event_id = oth.event_id;
	oth.event_id = -1;
}

dispatch::event_ref& dispatch::event_ref::operator =(event_ref&& oth) {
	if (this != &oth) {
		event_id = oth.event_id;
		oth.event_id = -1;
	}
	return *this;
}

dispatch::event_ref::~event_ref() {
	recycle();
}

dispatch::fd_ref::fd_ref() :
		fd_id(-1) {
}

dispatch::fd_ref::fd_ref(int id, int epoll_mode) :
		fd_id(id) {
	add_fd(id, epoll_mode);
}

int dispatch::fd_ref::fd() const {
	return fd_id;
}

void dispatch::fd_ref::recycle() {
	if (fd_id != -1) {
		::recycle_fd(fd_id);
		fd_id = -1;
	}
}

dispatch::fd_ref::fd_ref(fd_ref&& oth) {
	fd_id = oth.fd_id;
	oth.fd_id = -1;
}

dispatch::fd_ref& dispatch::fd_ref::operator =(fd_ref&& oth) {
	if (this != &oth) {
		fd_id = oth.fd_id;
		oth.fd_id = -1;
	}
	return *this;
}

dispatch::fd_ref::~fd_ref() {
	recycle();
}
