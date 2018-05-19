/*
 * util.cpp
 *
 *  Created on: Feb 13, 2018
 *      Author: mk2
 */

#include "util.h"

#include <sys/socket.h>
#include <cstring>
#include <unistd.h>
#include <unordered_map>

namespace util {

newline newl;

logger log() {
	return logger();
}

void logger::flush() {
	std::string str = ss.str();
	if (str.length() > 0) {
		std::ostringstream().swap(ss);
		auto tm = std::chrono::system_clock::to_time_t(
				std::chrono::system_clock::now());
		std::clog << std::put_time(std::localtime(&tm), "%T") << " " << str
				<< "\n";
	}
}

logger & logger::operator <<(const newline &) {
	flush();
	return *this;
}

logger::~logger() {
	flush();
}

std::unordered_map<int, std::string> names;

void name_fd(int fd, std::string fd_name) {
	names[fd] = fd_name;
}

const std::string& get_name(int fd) {
	if (names.count(fd) == 0) {
		names[fd] = std::to_string(fd);
	}
	return names[fd];
}

std::string error() {
	std::string res(strerror(errno));
	errno = 0;
	return res;
}

}
