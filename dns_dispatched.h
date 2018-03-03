#ifndef DNS_DISPATCHED_H_
#define DNS_DISPATCHED_H_

#include "dns.h"
#include "dispatch.h"

#include <future>

std::future<int> connect_to_remote_server(const std::string & host,
		const std::string & port, int next_event);

#endif /* DNS_DISPATCHED_H_ */
