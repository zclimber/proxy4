#include "dns.h"
#include "util.h"

#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <list>
#include <condition_variable>
#include <thread>
#include <cstring>

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

using util::log;

int enqueue_request(const std::string& host, const std::string& port,
		int timeout) {
	request new_req { host, port, ids++, timeout };
	std::lock_guard<std::mutex> lg(req_mutex);
	reqs.push_back(new_req);
	task_sleeper.notify_all();
	return new_req.id;
}

int get_dns_eventfd() {
	return event_fd;
}

std::vector<dns_response> get_ready_requests() {
	std::lock_guard<std::mutex> lg(resp_mutex);
	std::vector<dns_response> new_vec = std::move(ready);
	return new_vec;
}

void loop() {
	std::unique_lock<std::mutex> read_lock(req_mutex, std::defer_lock),
			write_lock(resp_mutex, std::defer_lock);
	auto it = reqs.begin();
	constexpr addrinfo hint { 0, AF_INET, SOCK_STREAM, 0, 0, 0, 0, nullptr };
	addrinfo * addr;
	long long ll = 1;
	for (;;) {
		read_lock.lock();
		if (reqs.empty()) {
			task_sleeper.wait(read_lock);
		}
		if (it == reqs.end()) {
			it = reqs.begin();
		}
		read_lock.unlock();

		int id = it->id;

		int result = getaddrinfo(it->host.c_str(), it->port.c_str(), &hint,
				&addr);
		switch (result) {
		case 0:
			break;
		case EAI_AGAIN:
			continue;
		case EAI_NONAME:
		case EAI_FAIL:
		case EAI_SERVICE:
			log << "Unable to resolve " << it->host << ":" << it->port << " . "
					<< gai_strerror(result) << "\n";
			break;
		default:
			log << "Unknown DNS error : " << gai_strerror(result) << " "
					<< strerror(errno) << " on " << it->host << ":" << it->port
					<< "\n";
			exit(0);
		}

		log << "Resolved domain " << it->host << "\n";

		int sock = -1;

		read_lock.lock();
		it = reqs.erase(it);
		read_lock.unlock();

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

static std::thread dns_thread(loop);
