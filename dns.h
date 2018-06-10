/*
 * dns.h
 *
 *  Created on: Mar 2, 2018
 *      Author: mk2
 */

#ifndef DNS_H_
#define DNS_H_

#include <string>
#include <vector>
#include <memory>
#include <future>

#include "dispatch.h"

struct dns_data;

class dns_pool {
	std::shared_ptr<dns_data> data;
public:
	dns_pool(int thread_count);
	std::future<int> connect_to_remote_server(const std::string& host,
				const std::string& port, const dispatch::event_ref & event) const;
	void stop_pool();
	void stop_wait();
};

#endif /* DNS_H_ */
