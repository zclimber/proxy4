/*
 * http.cpp
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#include "http.h"

#include <stddef.h>
#include <cctype>
#include <sstream>
#include <utility>

bool header_parser::set_string(const std::string& s) {
	std::istringstream ss;
	ss.str(s);
	// select request string
	char nxt;
	std::getline(ss, request_str, '\r').get(nxt);
	if (nxt != '\n') {
		return false;
	}
	// select all headers
	for (std::string header;
			getline(ss, header, '\r').get(nxt) && header.size();) {
		if (nxt != '\n') {
			return false;
		}
		size_t colon = header.find(':');
		size_t nxt = colon + 1;
		while (isspace(header[nxt])) {
			nxt++;
		}
		headers_map.insert(
				{ header.substr(0, colon), header.substr(nxt, header.size()) });
	}
	excess_str = ss.str().substr(ss.tellg(), s.size());
	return true;
}

std::string header_parser::assemble_head() {
	std::ostringstream ss;
	ss << request_str << "\r\n";
	for (auto x : headers_map) {
		ss << x.first << ": " << x.second << "\r\n";
	}
	ss << "\r\n";
	return ss.str();
}

std::string& header_parser::request() {
	return request_str;
}

std::unordered_map<std::string, std::string>& header_parser::headers() {
	return headers_map;
}

std::string& header_parser::excess() {
	return excess_str;
}

