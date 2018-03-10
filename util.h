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

class logger {
	class pr_logger {
		std::ostream & os;
	public:
		pr_logger() :
				os(std::clog) {
		}
		pr_logger(std::ostream & os) :
				os(os) {
		}
		template<class T>
		const pr_logger & operator <<(T t) const {
			os << t;
			return *this;
		}
	};
	pr_logger prl;
public:
	logger() :
			prl(std::clog) {
	}
	logger(std::ostream & os) :
			prl(os) {
	}
	const pr_logger & cnt() const {
		return prl;
	}
	template<class T>
	const pr_logger & operator <<(T t) const {
		auto tm = std::chrono::system_clock::to_time_t(
				std::chrono::system_clock::now());
		prl << std::put_time(std::localtime(&tm), "%T") << " ";
		return prl << t;
	}
};

void name_fd(int fd, std::string fd_name);
const std::string & get_name(int fd);

int read(int fd, char * data, size_t length);
int send(int fd, const char * data, size_t length, int flags);

extern logger log;

}

#endif /* UTIL_H_ */
