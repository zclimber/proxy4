/*
 * util.cpp
 *
 *  Created on: Feb 13, 2018
 *      Author: mk2
 */

#include "util.h"

#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace util {

logger log;

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

int read(int fd, char* data, size_t length) {
	int result = ::read(fd, data, length);
	log << "Read " << result << " from " << get_name(fd) << "\n";
	return result;
}

int send(int fd, const char* data, size_t length, int flags) {
	int result = ::send(fd, data, length, flags);
	log << "Write " << result << " to " << get_name(fd) << "\n";
	return result;
}

}
