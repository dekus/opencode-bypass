#ifndef ZEN_PROXY_HPP
#define ZEN_PROXY_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct zen_proxy_server {
	std::atomic<bool> m_stop {false};
	std::thread m_accept_thread;
	std::thread m_watch_thread;
	uintptr_t m_listen = ~(uintptr_t)0;
	uint16_t m_port = 0;
	std::string m_edge;

	void shutdown();
};

std::unique_ptr<zen_proxy_server> zen_proxy_start(uint16_t bind_port, uint16_t *out_port, std::string *out_edge);

#endif
