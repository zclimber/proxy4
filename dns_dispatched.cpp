#include "dns_dispatched.h"

#include <type_traits>
#include <unistd.h>

struct task {
	std::promise<int> promise;
	int next_event;
	task(int next_event) :
			next_event(next_event) {
	}
	task(task && other) = default;
	task(const task & other) = default;
};

std::unordered_map<int, task> tasks;

std::future<int> connect_to_remote_server(const std::string& host,
		const std::string& port, int next_event) {
	int request_id = enqueue_request(host, port, 10000);
	tasks.emplace(request_id, next_event);
	return tasks.find(request_id)->second.promise.get_future();
}

int init_dispatched_dns() {
	int dns_event = dispatch::add_event([] {
		long long ev = 0;
		read(get_dns_eventfd(), &ev, sizeof(ev));
		auto new_ids = get_ready_requests();
		for(auto x : new_ids) {
			auto it = tasks.find(x.identifier);
			task & current = it->second;
			current.promise.set_value(x.result);
			dispatch::arm_manual(current.next_event);
			tasks.erase(it);
		}
	});
	dispatch::add_fd(get_dns_eventfd(), EPOLLIN);
	dispatch::link(get_dns_eventfd(), EPOLLIN, dns_event);
	return 0;
}

static int init_int = init_dispatched_dns();
