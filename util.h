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

namespace util {

struct newline {
};

extern newline newl;

class logger {
	std::ostringstream ss;
public:
	void flush() {
		if (ss.rdbuf()->str().length() > 0) {
			auto tm = std::chrono::system_clock::to_time_t(
					std::chrono::system_clock::now());
			std::clog << std::put_time(std::localtime(&tm), "%T") << " "
					<< ss.rdbuf()->str() << "\n";
			ss.rdbuf()->str(std::string());
		}
	}
	logger() = default;
	logger(logger &&) = default;
	logger(const logger &) = delete;
	logger & operator <<(const newline &) {
		flush();
		return *this;
	}
	template<class T>
	logger & operator <<(T t) {
		ss << t;
		return *this;
	}
	~logger() {
		flush();
	}
};

logger log();

void name_fd(int fd, std::string fd_name);
const std::string & get_name(int fd);

}

#endif /* UTIL_H_ */
