#include "dns_dispatched.h"

#include <type_traits>
#include <unistd.h>

struct task {
	std::promise<int> promise;
	const dispatch::event_ref & next_event;
	task(const dispatch::event_ref & event) :
			next_event(event) {
	}
	task(task && other) = default;
	task(const task & other) = default;
};

std::unordered_map<int, task> tasks;

std::future<int> connect_to_remote_server(const std::string& host,
		const std::string& port, const dispatch::event_ref & event) {
	int request_id = enqueue_request(host, port, 10000);
	tasks.emplace(request_id, event);
	return tasks.find(request_id)->second.promise.get_future();
}

dispatch::fd_ref event_fd;

#include "util.h"

int init_dispatched_dns() {
	event_fd = dispatch::fd_ref(get_dns_eventfd(), EPOLLIN);
	dispatch::event_ref event(
			[] {
				long long ev = 0;
				read(get_dns_eventfd(), &ev, sizeof(ev));
				auto new_ids = get_ready_requests();
				for(auto x : new_ids) {
					auto it = tasks.find(x.identifier);
					if(it == tasks.end()) {
						util::log << "DNS error : request " << x.identifier << " doesn't exist but resolved\n";
					} else {
						task & current = it->second;
						current.promise.set_value(x.result);
						dispatch::arm_manual(current.next_event);
						tasks.erase(it);
					}
				}
			});
	dispatch::link(event_fd, EPOLLIN, event);
	return 0;
}

static int init_int = init_dispatched_dns();
