/*
 * relay.h
 *
 *  Created on: Mar 10, 2018
 *      Author: mk2
 */

#ifndef RELAY_H_
#define RELAY_H_

#include <functional>
#include <string>
#include <memory>

#include "dispatch.h"

class relay: std::enable_shared_from_this<relay> {
	enum state {
		READ_WRITE, WRITE_REST, FINISHED
	};
	dispatch::fd_ref & in_fd, &out_fd;
	state st;
	std::string buf;
	std::function<void()> finisher;

public:

	relay(dispatch::fd_ref & in_fd, dispatch::fd_ref & out_fd);
	void loop_once();

	relay & set_buffer(std::string && data);
	relay & set_buffer(const std::string & data);
	relay & set_finisher(std::function<void()> on_finish);

	~relay();
};

void make_relay(dispatch::fd_ref& in_fd, dispatch::fd_ref& out_fd,
		std::string data, std::function<void()> on_finish);

#endif /* RELAY_H_ */
