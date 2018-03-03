/*
 * dispatch.h
 *
 *  Created on: Mar 2, 2018
 *      Author: mk2
 */

#ifndef DISPATCH_H_
#define DISPATCH_H_

#include <sys/epoll.h>
#include <functional>

namespace dispatch{

int add_event(std::function<void()> event);
bool add_fd(int fd, int epoll_mode);

void link(int fd, int epoll_target, int event_id);
void unlink(int fd, int event_id);
void unlink_current(int fd);

void recycle_event(int event_id);
void recycle_event_current();
void recycle_fd(int fd);

void arm_manual(int event_id);

void run_dispatcher_in_current_thread();
void create_dispatcher_thread();

}

#endif /* DISPATCH_H_ */
