/*
 * dispatch.h
 *
 *  Created on: Mar 2, 2018
 *      Author: mk2
 */

#ifndef DISPATCH_H_
#define DISPATCH_H_

#include <functional>
#include "util.h"

#include <sys/epoll.h>

namespace dispatch {

class event_ref {
	int event_id;
public:
	event_ref();
	event_ref(const std::function<void()> & event);

	event_ref(const event_ref &) = delete;
	event_ref(event_ref &&);
	event_ref & operator =(event_ref &&);

	int id() const;

	void recycle();

	~event_ref();

	friend util::logger & operator << (util::logger&, const event_ref &);
};


util::logger & operator << (util::logger&, const event_ref &);

class fd_ref {
	int fd_id;
public:
	fd_ref();
	fd_ref(int id, int epoll_mode);

	fd_ref(const fd_ref &) = delete;
	fd_ref(fd_ref &&);
	fd_ref & operator =(fd_ref &&);

	int fd() const;

	void recycle();

	~fd_ref();

	friend util::logger & operator << (util::logger&, const fd_ref &);
};

util::logger & operator << (util::logger&, const fd_ref &);

void link(const fd_ref &, int epoll_target, const event_ref &);
void unlink(const fd_ref &, const event_ref &);
void unlink_current(const fd_ref &);

void recycle_event(const event_ref &);
void recycle_event_current();
void recycle_fd(const fd_ref &);

void arm_manual(const event_ref &);

void run_dispatcher_in_current_thread();
void create_dispatcher_thread();
void cleanup();

}

#endif /* DISPATCH_H_ */
