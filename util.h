/*
 * util.h
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#ifndef UTIL_H_
#define UTIL_H_

#include <stddef.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>

#include <memory>

namespace util {

struct newline {
};

extern newline newl;

class logger {
	std::unique_ptr<std::ostringstream> uss = std::make_unique<std::ostringstream>();
//	std::ostringstream ss;
public:
	void flush();
	logger() = default;
	logger(logger &&) = default;
	logger(const logger &) = delete;
	logger & operator <<(const newline &);
	template<class T>
	logger & operator <<(T t) {
		(*uss.get()) << t;
		return *this;
	}
	~logger();
};

std::string error();

logger log();

void name_fd(int fd, std::string fd_name);
const std::string & get_name(int fd);

}

#endif /* UTIL_H_ */
