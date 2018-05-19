#ifndef DNS_DISPATCHED_H_
#define DNS_DISPATCHED_H_

#include <future>
#include <string>

#include "dispatch.h"

std::future<int> connect_to_remote_server(const std::string& host,
		const std::string& port, const dispatch::event_ref & event);

std::thread init_dispatched_dns();

#endif /* DNS_DISPATCHED_H_ */
