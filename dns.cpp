#include "dns.h"

#include <netdb.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <thread>
#include <atomic>

#include "util.h"

struct request {
	std::string host;
	std::string port;
	int id;
	std::reference_wrapper<const dispatch::event_ref> next_event;
	std::shared_ptr<std::promise<int>> promise;
};

struct dns_data {
	std::atomic_int ids;
	std::list<request> reqs;
	std::mutex req_mutex;
	std::condition_variable task_sleeper;
	std::atomic_flag work;
	std::vector<std::thread> threads;

	dns_data(int thread_count) {
		ids.store(0);
		threads.resize(thread_count);
		work.test_and_set(std::memory_order_relaxed);
		for (auto && t : threads) {
			t = std::thread(&dns_data::start_dns_resolver, this);
		}
	}
	std::future<int> enqueue_request(const std::string& host,
			const std::string& port, const dispatch::event_ref & event) {
		int id = ids.fetch_add(1, std::memory_order_relaxed);
		request new_req { host, port, id, std::ref(event), std::make_shared<
				std::promise<int>>() };
		std::lock_guard<std::mutex> lg(req_mutex);
		reqs.push_front(new_req);
		task_sleeper.notify_one();
		return new_req.promise->get_future();
	}
	bool get_request(request & to) {
		std::unique_lock<std::mutex> read_lock(req_mutex);
		while (reqs.empty()) {
			if (!work.test_and_set(std::memory_order_relaxed)) {
				work.clear(std::memory_order_relaxed);
				return false;
			}
			task_sleeper.wait(read_lock);
		}
		to = reqs.front();
		reqs.pop_front();
		return true;
	}
	void return_request_to_queue(request req) {
		std::unique_lock<std::mutex> read_lock(req_mutex);
		reqs.push_back(req);
		task_sleeper.notify_one();
	}
	void start_dns_resolver() {
		std::unique_lock<std::mutex> read_lock(req_mutex, std::defer_lock);
		constexpr addrinfo hint { 0, AF_INET, SOCK_STREAM, 0, 0, 0, 0, nullptr };
		const dispatch::event_ref dummy;
		request req {"", "", 0, std::ref(dummy), std::shared_ptr<std::promise<int>>()};
		addrinfo * addr;
		for (;;) {
			bool success = get_request(req);
			if (!work.test_and_set(std::memory_order_relaxed)) {
				work.clear(std::memory_order_relaxed);
				return;
			}

			int id = req.id;

			int result = getaddrinfo(req.host.c_str(), req.port.c_str(), &hint,
					&addr);
			switch (result) {
			case 0:
				break;
			case EAI_AGAIN:
				return_request_to_queue(req);
				continue;
			case EAI_NONAME:
			case EAI_FAIL:
			case EAI_SERVICE:
			case EAI_NODATA:
				util::log() << "Unable to resolve " << req.host << ":"
						<< req.port << " . " << gai_strerror(result);
				break;
			default:
				util::log() << "Unknown DNS error : " << gai_strerror(result)
						<< " " << strerror(errno) << " on " << req.host << ":"
						<< req.port;
				exit(0);
			}

			util::log() << "Resolved domain " << req.host;

			int sock = -1;

			if (result == 0) {
				for (addrinfo * cr = addr; cr != nullptr; cr = cr->ai_next) {
					sock = socket(cr->ai_family, cr->ai_socktype,
							cr->ai_protocol);
					int conn_srv = connect(sock, cr->ai_addr, cr->ai_addrlen);
					if (conn_srv == 0) {
						break;
					}
					close(sock);
					sock = -1;
				}

				freeaddrinfo(addr);
			}
			req.promise->set_value(sock);
			dispatch::arm_manual(req.next_event.get());
		}
	}
	void stop_pool() {
		work.clear(std::memory_order_relaxed);
		task_sleeper.notify_all();
	}
	~dns_data() {
		stop_pool();
		for (auto && t : threads) {
			t.join();
		}
	}
};

dns_pool::dns_pool(int thread_count) {
	data = std::make_shared<dns_data>(thread_count);
}

std::future<int> dns_pool::connect_to_remote_server(const std::string& host,
		const std::string& port, const dispatch::event_ref& event) const {
	return data->enqueue_request(host, port, event);
}

void dns_pool::stop_pool() {
	data->stop_pool();
}
