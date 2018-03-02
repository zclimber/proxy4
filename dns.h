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

struct dns_response{
	int identifier;
	int result;
};

int enqueue_request(const std::string & host, const std::string & port, int timeout);

int get_dns_eventfd();

std::vector<dns_response> get_ready_requests();


#endif /* DNS_H_ */
