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

#include "util.h"

struct request {
	std::string host;
	std::string port;
	int id;
	int timeout;
};

static int ids;
static std::list<request> reqs;
static std::vector<dns_response> ready;
static int event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
static std::mutex req_mutex, resp_mutex;
static std::condition_variable task_sleeper;

int enqueue_request(const std::string& host, const std::string& port,
		int timeout) {
	request new_req { host, port, ids++, timeout };
	std::lock_guard<std::mutex> lg(req_mutex);
	reqs.push_front(new_req);
	task_sleeper.notify_one();
	return new_req.id;
}

int get_dns_eventfd() {
	return event_fd;
}

std::vector<dns_response> get_ready_requests() {
	std::lock_guard<std::mutex> lg(resp_mutex);
	std::vector<dns_response> new_vec(ready);
	ready.clear();
	return new_vec;
}

request get_request() {
	std::unique_lock<std::mutex> read_lock(req_mutex);
	while (reqs.empty()) {
		task_sleeper.wait(read_lock);
	}
	request rq = reqs.front();
	reqs.pop_front();
	return rq;
}

void return_request_to_queue(request req) {
	std::lock_guard<std::mutex> lg(req_mutex);
	reqs.push_back(req);
	task_sleeper.notify_one();
}

void start_dns_resolver() {
	std::unique_lock<std::mutex> read_lock(req_mutex, std::defer_lock),
			write_lock(resp_mutex, std::defer_lock);
	constexpr addrinfo hint { 0, AF_INET, SOCK_STREAM, 0, 0, 0, 0, nullptr };
	addrinfo * addr;
	long long ll = 1;
	for (;;) {
		request req = get_request();

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
			util::log() << "Unable to resolve " << req.host << ":" << req.port
					<< " . " << gai_strerror(result);
			break;
		default:
			util::log() << "Unknown DNS error : " << gai_strerror(result) << " "
					<< strerror(errno) << " on " << req.host << ":" << req.port;
			exit(0);
		}

		util::log() << "Resolved domain " << req.host;

		int sock = -1;

		if (result == 0) {
			for (addrinfo * cr = addr; cr != nullptr; cr = cr->ai_next) {
				sock = socket(cr->ai_family, cr->ai_socktype, cr->ai_protocol);
				int conn_srv = connect(sock, cr->ai_addr, cr->ai_addrlen);
				if (conn_srv == 0) {
					break;
				}
				close(sock);
				sock = -1;
			}

			freeaddrinfo(addr);
		}
		dns_response resp { id, sock };
		write_lock.lock();
		ready.push_back(resp);
		write_lock.unlock();
		write(event_fd, &ll, sizeof(ll));
	}
}
