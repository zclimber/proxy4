#include "loaders.h"

#include <asm-generic/errno-base.h>
#include <stddef.h>
#include <sys/socket.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "util.h"

constexpr int READ_BUFFER_SIZE = 1 << 12;

using intprom = std::promise<int>;

void finish(dispatch::fd_ref & sock, const dispatch::event_ref & next_action,
		util::logger & log) {
	dispatch::unlink_current(sock);
	dispatch::recycle_event_current();
	dispatch::arm_manual(next_action);
	log << " (ending " << sock.fd() << ") ";
}

void async_load_generic(std::string& buf, dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action,
		std::function<bool(const std::string &, int, util::logger &)> && positive_check,
		std::shared_ptr<intprom> prom) {

	auto log = util::log();

	log << "Generic s" << sock.fd() << " : ";
	char t[READ_BUFFER_SIZE];
	if (positive_check(buf, 0, log)) {
		finish(sock, next_action, log);
		prom->set_value(0);
		log << "WIN inst";
		return;
	}
	for (;;) {
		int res = recv(sock.fd(), t, sizeof(t), MSG_DONTWAIT);
		log << res << " ";
		if (res <= 0) {
			log << strerror(errno) << " ";
			if (res == 0) {
				log << "EOF";
				finish(sock, fail_action, log);
				prom->set_value(-1);
			} else if (res == -1 && errno == EAGAIN) {
				log << "WAIT";
			} else {
				log << "FAIL";
				finish(sock, fail_action, log);
				prom->set_value(-1);
			}
			errno = 0;
			return;
		} else {
			buf.append(t, t + res);
			if (positive_check(buf, res, log)) {
				finish(sock, next_action, log);
				prom->set_value(0);
				log << "WIN";
				return;
			}
		}
	}
}

std::future<int> async_load::headers(std::string& buf, dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action) {
	auto prom = std::make_shared<intprom>();
	auto internal =
			[] (const std::string & buf, int last_size, util::logger & log) -> bool {
				auto find = buf.find("\r\n\r\n",
						std::max<int>(0, int(buf.size()) - last_size - 3));
				log << "chk " << (find != buf.npos) << " ";
				return find != buf.npos;
			};
	dispatch::event_ref d(
			[&buf, &sock, &next_action, &fail_action, prom, internal]() {
				async_load_generic(buf, sock, next_action, fail_action, internal, prom);
			});

	dispatch::link(sock, EPOLLIN | EPOLLRDHUP | EPOLLHUP, d);
	dispatch::arm_manual(d);
	util::log() << "Headers " << d.id();
	return prom->get_future();
}

std::future<int> async_load::fixed(std::string& buf, dispatch::fd_ref & sock,
		int length, const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action) {
	auto prom = std::make_shared<intprom>();
	dispatch::event_ref d(
			[&buf, &sock, &next_action, &fail_action, prom, length]() mutable {
				int & xlen = length;
				async_load_generic(buf, sock, next_action, fail_action,
						[&xlen] (const std::string &, int last_size, util::logger & log) {
							xlen -= last_size;
							log << "l " << xlen << " ";
							return xlen <= 0;
						}, prom);
			});
	dispatch::link(sock, EPOLLIN | EPOLLRDHUP | EPOLLHUP, d);
	dispatch::arm_manual(d);
	return prom->get_future();
}

std::pair<int, int> get_new_chunk_size(std::string_view sw) {
	int rs = 0;
	rs = std::strtol(sw.data(), nullptr, 16);
	size_t newt = sw.find("\r\n");
	if (newt == sw.npos) {
		return {-1, -1};
	} else {
		return {rs, (int)newt + 2};
	}
}

void chunked_check(dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action, int & cpos, int & chunkl,
		bool & length_pending, std::string & buf,
		std::shared_ptr<intprom> prom) {
	char t[4096];
	auto log = util::log();
	log << "Chunked s" << sock.fd() << " : ";
	while (true) {
		if ((cpos >= (int) buf.size()) | length_pending) {
			int res = recv(sock.fd(), t, sizeof(t), MSG_DONTWAIT);
			length_pending = false;
			log << "L " << res << " ";
			if (res <= 0) {
				log << strerror(errno) << " ";
				if (res == 0) {
					log << "EOF";
//					finish(sock, fail_action);
//					prom->set_value(-1);
				} else if (res == -1 && errno == EAGAIN) {
					log << "WAIT";
				} else {
					log << "FAIL";
					finish(sock, fail_action, log);
					prom->set_value(-1);
				}
				errno = 0;
				return;
			} else {
				buf.append(t, t + res);
			}
		} else {
			if (chunkl > 0) {
				int mn = std::min(chunkl, (int) buf.length() - cpos);
				cpos += mn;
				chunkl -= mn;
				log << "C- " << mn << " ";
			} else if (chunkl == 0) {
				std::string_view sw(buf);
				sw.remove_prefix(cpos);
				auto gn = get_new_chunk_size(sw);
				if (gn.first < 0) {
					length_pending = true;
					continue;
				}
				log << "CHUNK " << gn.first << " ";
				cpos += gn.second;
				chunkl = gn.first > 0 ? gn.first + 2 : -1;
			} else {
				if (buf.find("\r\n\r\n", cpos - 3) != buf.npos) {
					finish(sock, next_action, log);
					prom->set_value(0);
					log << "WIN";
					return;
				} else {
					cpos = buf.length();
				}
			}
		}
	}

}

std::future<int> async_load::chunked(std::string& buf, dispatch::fd_ref & sock,
		const dispatch::event_ref & next_action,
		const dispatch::event_ref & fail_action) {
	auto prom = std::make_shared<intprom>();
	int cpos = buf.find("\r\n\r\n") + 4;
	int chunkl = 0;
	bool length_pending = false;
	dispatch::event_ref d(
			[ =, &buf, &sock, &next_action, &fail_action]() mutable {
				chunked_check(sock, next_action, fail_action, cpos, chunkl, length_pending, buf, prom);
			});
	dispatch::link(sock, EPOLLIN | EPOLLRDHUP | EPOLLHUP, d);
	dispatch::arm_manual(d);
	return prom->get_future();
}

std::future<int> async_load::upload(std::string& buf, dispatch::fd_ref& sock,
		const dispatch::event_ref& next_action,
		const dispatch::event_ref& fail_action) {
	auto prom = std::make_shared<std::promise<int>>();
	size_t offs = 0;
	auto log = util::log();
	dispatch::event_ref upl(
			[&, prom, offs]() mutable {
				log << "Upload s" << sock.fd() << " : ";
				while(offs < buf.size()) {
					int rs = send(sock.fd(), buf.c_str() + offs, buf.size() - offs, MSG_DONTWAIT | MSG_NOSIGNAL);
					log << rs << " ";
					if(rs > 0) {
						offs += rs;
					} else if (rs == -1 && errno == EAGAIN) {
						log << "WAIT";
						return;
					} else {
						log << "FAIL";
						finish(sock, fail_action, log);
						prom->set_value(-1);
						return;
					}
				}
				if(offs == buf.size()) {
					finish(sock, next_action, log);
					log << "WIN";
					prom->set_value(0);
					offs += 100;
				} else {
					log << "SECONDCALL";
				}
			});
	dispatch::link(sock, EPOLLOUT | EPOLLRDHUP | EPOLLHUP, upl);
	dispatch::arm_manual(upl);
	return prom->get_future();
}
