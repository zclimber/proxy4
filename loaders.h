#ifndef LOADERS_H_
#define LOADERS_H_

#include <future>
#include <string>

#include "dispatch.h"

namespace async_load {

// all of these assume sock is added to dispatch

std::future<int> headers(std::string& buf, dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action);
std::future<int> fixed(std::string& buf, dispatch::fd_ref & sock, int length,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action);
std::future<int> chunked(std::string& buf, dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action);
std::future<int> upload(std::string& buf, dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action);

}
;

#endif /* LOADERS_H_ */
