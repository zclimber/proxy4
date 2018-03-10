#ifndef DNS_DISPATCHED_H_
#define DNS_DISPATCHED_H_

#include "dns.h"
#include "dispatch.h"

#include <future>

std::future<int> connect_to_remote_server(const std::string& host,
		const std::string& port, const dispatch::event_ref & event);

#endif /* DNS_DISPATCHED_H_ */
