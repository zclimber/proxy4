/*
 * http.h
 *
 *  Created on: Feb 9, 2018
 *      Author: mk2
 */

#ifndef HTTP_H_
#define HTTP_H_

#include <unordered_map>
#include <string>

class header_parser {
	std::string request_str, excess_str;
	std::unordered_map<std::string, std::string> headers_map;
public:
	bool set_string(const std::string & s);
	std::string assemble_head();
	std::string & request();
	std::unordered_map<std::string, std::string> & headers();
	std::string & excess();
};
#endif /* HTTP_H_ */

