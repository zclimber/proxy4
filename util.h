/*
 * util.h
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#ifndef UTIL_H_
#define UTIL_H_

#include <iostream>
#include <type_traits>

namespace util {

class logger {
	std::ostream & os;
public:
	logger() :
			os(std::clog) {
	}
	logger(std::ostream & os) :
			os(os) {
	}
	template<class T>
	const logger & operator <<(T t) const {
		os << t;
		return *this;
	}
};

void name_fd(int fd, std::string fd_name);
const std::string & get_name(int fd);

int read(int fd, char * data, size_t length);
int send(int fd, const char * data, size_t length, int flags);

extern logger log;

}

#endif /* UTIL_H_ */
