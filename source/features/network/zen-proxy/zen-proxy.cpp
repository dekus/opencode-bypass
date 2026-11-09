#include "features/network/zen-proxy/zen-proxy.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_set>

#include "ouc-common/ouc-common.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/xorstr/xorstr.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace {

const std::unordered_set<std::string> k_pinned = {"opencode.ai", "www.opencode.ai", "api.opencode.ai"};

struct warp_state {
	double m_ttl = 1.5;
	std::mutex m_lock;
	double m_ts = 0;
	std::string m_v6;
	std::string m_last_prefix;
	std::vector<std::string> m_pool;
	std::string m_edge;
	double m_reroll_mtime = 0;
};

static void to_lower(std::string &s) {
	for (auto &c : s)
		c = (char)tolower((unsigned char)c);
}

static void apply_edge_controls(warp_state *st) {
	auto override_file = g_lane_dir / xorstr("edge_override.txt");
	auto reroll_file = g_lane_dir / xorstr("edge_reroll");
	char *pin_env = std::getenv(xorstr("OUC_PIN_EDGE"));

	if (!pin_env && std::filesystem::is_regular_file(override_file)) {
		auto val = ouc_read_file_text(override_file);
		while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' '))
			val.pop_back();
		to_lower(val);
		if (!val.empty() && val != xorstr("auto") && val.find(':') != std::string::npos) {
			st->m_edge = val;
			ouc_write_file_text(g_lane_dir / xorstr("last_edge.txt"), val + "\n");
			return;
		}
	}

	if (std::filesystem::is_regular_file(reroll_file)) {
		try {
			auto mtime = std::filesystem::last_write_time(reroll_file);
			double mt = (double)std::chrono::duration<double>(mtime.time_since_epoch()).count();
			if (mt > st->m_reroll_mtime) {
				st->m_reroll_mtime = mt;
				auto fresh = ouc_fetch_opencode_aaaa_pool();
				if (!fresh.empty())
					st->m_pool = fresh;
				std::vector<std::string> choices;
				for (auto &c : st->m_pool) {
					if (c != st->m_edge)
						choices.push_back(c);
				}
				if (choices.empty())
					choices = st->m_pool;
				if (!choices.empty()) {
					std::mt19937 rng((unsigned)LI(GetTickCount)());
					st->m_edge = choices[rng() % choices.size()];
					ouc_write_file_text(g_lane_dir / xorstr("last_edge.txt"), st->m_edge + "\n");
				}
			}
		} catch (...) {
		}
	}
}

static std::pair<std::string, std::string> warp_get(warp_state *st, bool wait_rotating) {
	const double deadline = (double)LI(GetTickCount64)() / 1000.0 + (wait_rotating ? 18.0 : 0.0);
	for (;;) {
		{
			std::lock_guard<std::mutex> lk(st->m_lock);
			double now = (double)LI(GetTickCount64)() / 1000.0;
			if (now - st->m_ts < st->m_ttl && !st->m_v6.empty())
				return {st->m_v6, st->m_edge};
		}

		auto v6opt = ouc_get_warp_local_v6();
		std::string v6 = v6opt.value_or("");
		std::string edge;
		{
			std::lock_guard<std::mutex> lk(st->m_lock);
			st->m_ts = (double)LI(GetTickCount64)() / 1000.0;
			st->m_v6 = v6;
			if (!v6.empty()) {
				auto prefix = ouc_normalize_v6_64(v6);
				if (!prefix.empty() && !st->m_last_prefix.empty() && prefix != st->m_last_prefix) {
					if (!std::getenv(xorstr("OUC_PIN_EDGE"))) {
						auto fresh = ouc_fetch_opencode_aaaa_pool();
						if (!fresh.empty())
							st->m_pool = fresh;
						if (!st->m_pool.empty()) {
							std::mt19937 rng((unsigned)LI(GetTickCount)());
							st->m_edge = st->m_pool[rng() % st->m_pool.size()];
							ouc_write_file_text(g_lane_dir / xorstr("last_edge.txt"), st->m_edge + "\n");
						}
					}
				}
				if (!prefix.empty())
					st->m_last_prefix = prefix;
			}
			apply_edge_controls(st);
			edge = st->m_edge;
		}

		if (!v6.empty())
			return {v6, edge};

		if (!wait_rotating || !ouc_is_keeper_paused() || (double)LI(GetTickCount64)() / 1000.0 >= deadline)
			return {"", edge};

		LI(Sleep)(750);
	}
}

static SOCKET open_pinned_v6(const std::string &dest_ip, int port, const std::string &local_v6) {
	SOCKET s = LI(socket)(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;

	sockaddr_in6 bind_addr {};
	bind_addr.sin6_family = AF_INET6;
	LI(InetPtonA)(AF_INET6, local_v6.c_str(), &bind_addr.sin6_addr);

	if (LI(bind)(s, (sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
		LI(closesocket)(s);
		return INVALID_SOCKET;
	}

	sockaddr_in6 peer {};
	peer.sin6_family = AF_INET6;
	peer.sin6_port = LI(htons)((u_short)port);
	LI(InetPtonA)(AF_INET6, dest_ip.c_str(), &peer.sin6_addr);

	if (LI(connect)(s, (sockaddr *)&peer, sizeof(peer)) != 0) {
		LI(closesocket)(s);
		return INVALID_SOCKET;
	}
	return s;
}

static SOCKET open_upstream(const std::string &host, int port, warp_state *st) {
	std::string clean = host;
	if (!clean.empty() && clean.front() == '[')
		clean = clean.substr(1);
	if (!clean.empty() && clean.back() == ']')
		clean.pop_back();
	to_lower(clean);

	if (clean == xorstr("127.0.0.1") || clean == xorstr("localhost") || clean == xorstr("::1"))
		return INVALID_SOCKET;

	if (k_pinned.count(clean) || clean.size() > 12 && clean.substr(clean.size() - 12) == xorstr(".opencode.ai")) {
		auto [local_v6, pinned_edge] = warp_get(st, true);
		if (local_v6.empty())
			return INVALID_SOCKET;
		return open_pinned_v6(pinned_edge, port, local_v6);
	}

	auto [local_v6, _] = warp_get(st, false);
	if (!local_v6.empty()) {
		addrinfo hints {};
		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *res = nullptr;
		if (LI(getaddrinfo)(clean.c_str(), nullptr, &hints, &res) == 0 && res) {
			char buf[INET6_ADDRSTRLEN] = {};
			auto *in = (sockaddr_in6 *)res->ai_addr;
			LI(InetNtopA)(AF_INET6, &in->sin6_addr, buf, sizeof(buf));
			LI(freeaddrinfo)(res);
			return open_pinned_v6(buf, port, local_v6);
		}
		if (res)
			LI(freeaddrinfo)(res);
	}

	auto strict_flag = g_lane_dir / xorstr("strict_proxy.flag");
	if (std::getenv(xorstr("OUC_STRICT_PROXY")) || std::filesystem::is_regular_file(strict_flag))
		return INVALID_SOCKET;

	addrinfo hints {};
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *res = nullptr;
	if (LI(getaddrinfo)(clean.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
		return INVALID_SOCKET;

	SOCKET s = LI(socket)(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s == INVALID_SOCKET) {
		LI(freeaddrinfo)(res);
		return INVALID_SOCKET;
	}
	if (LI(connect)(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
		LI(closesocket)(s);
		LI(freeaddrinfo)(res);
		return INVALID_SOCKET;
	}
	LI(freeaddrinfo)(res);
	return s;
}

static void relay(SOCKET a, SOCKET b) {
	char buf[65536];
	fd_set fds;
	for (;;) {
		FD_ZERO(&fds);
		FD_SET(a, &fds);
		FD_SET(b, &fds);
		int hi = (int)((a > b) ? a : b);
		int r = LI(select)(hi + 1, &fds, nullptr, nullptr, nullptr);
		if (r <= 0)
			break;
		for (SOCKET src : {a, b}) {
			if (!FD_ISSET(src, &fds))
				continue;
			SOCKET dst = src == a ? b : a;
			int got = LI(recv)(src, buf, sizeof(buf), 0);
			if (got <= 0)
				return;
			int sent = 0;
			while (sent < got) {
				int n = LI(send)(dst, buf + sent, got - sent, 0);
				if (n <= 0)
					return;
				sent += n;
			}
		}
	}
}

static void send_http_err(SOCKET client, int code, const char *reason) {
	char msg[512];
	std::snprintf(msg, sizeof(msg), "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", code, reason);
	LI(send)(client, msg, (int)strlen(msg), 0);
}

static void handle_client(SOCKET client, warp_state *st) {
	struct client_close {
		SOCKET m_s;
		~client_close() {
			LI(closesocket)(m_s);
		}
	} close_guard {client};

	char line[8192];
	int got = LI(recv)(client, line, sizeof(line) - 1, 0);
	if (got <= 0)
		return;

	line[got] = '\0';
	std::istringstream iss(line);
	std::string method, target, proto;
	iss >> method >> target >> proto;
	if (method.empty())
		return;

	to_lower(method);
	if (method == xorstr("connect")) {
		std::string host;
		int port = 443;
		auto colon = target.rfind(':');
		if (colon == std::string::npos) {
			host = target;
		} else {
			host = target.substr(0, colon);
			port = std::atoi(target.substr(colon + 1).c_str());
		}
		auto up = open_upstream(host, port, st);
		if (up == INVALID_SOCKET) {
			send_http_err(client, 503, "Service Unavailable");
			return;
		}
		const char *ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
		LI(send)(client, ok, (int)strlen(ok), 0);
		relay(client, up);
		LI(closesocket)(up);
		return;
	}

	std::string host;
	int port = 80;
	if (target.find(xorstr("://")) != std::string::npos) {
		auto scheme_end = target.find("://");
		auto path_start = target.find('/', scheme_end + 3);
		std::string authority = path_start == std::string::npos ? target.substr(scheme_end + 3)
									  : target.substr(scheme_end + 3, path_start - scheme_end - 3);
		auto c = authority.find(':');
		if (c == std::string::npos) {
			host = authority;
		} else {
			host = authority.substr(0, c);
			port = std::atoi(authority.substr(c + 1).c_str());
		}
	}

	if (host.empty()) {
		send_http_err(client, 400, "Bad Request");
		return;
	}

	auto up = open_upstream(host, port, st);
	if (up == INVALID_SOCKET) {
		send_http_err(client, 503, "Service Unavailable");
		return;
	}
	LI(send)(up, line, got, 0);
	relay(client, up);
	LI(closesocket)(up);
}

} // namespace

struct zen_proxy_server_impl : zen_proxy_server {
	std::shared_ptr<warp_state> m_warp;
};

void zen_proxy_server::shutdown() {
	m_stop = true;
	if (m_listen != ~(uintptr_t)0) {
		LI(closesocket)((SOCKET)m_listen);
		m_listen = ~(uintptr_t)0;
	}
	if (m_accept_thread.joinable())
		m_accept_thread.join();
	if (m_watch_thread.joinable())
		m_watch_thread.join();
}

std::unique_ptr<zen_proxy_server> zen_proxy_start(uint16_t bind_port, uint16_t *out_port, std::string *out_edge) {
	ouc_ensure_dirs();
	auto pool = ouc_fetch_opencode_aaaa_pool();
	char *pin = std::getenv(xorstr("OUC_PIN_EDGE"));
	std::string chosen;
	if (pin && *pin)
		chosen = pin;
	else if (!pool.empty()) {
		std::mt19937 rng((unsigned)LI(GetTickCount)());
		chosen = pool[rng() % pool.size()];
	}
	ouc_write_file_text(g_lane_dir / xorstr("last_edge.txt"), chosen + "\n");

	auto st = std::make_shared<warp_state>();
	st->m_pool = pool;
	st->m_edge = chosen;

	auto srv = std::make_unique<zen_proxy_server_impl>();
	srv->m_warp = st;
	srv->m_edge = chosen;
	SOCKET listen_sock = LI(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock == INVALID_SOCKET)
		return nullptr;
	srv->m_listen = (uintptr_t)listen_sock;

	int yes = 1;
	LI(setsockopt)(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = LI(htons)(bind_port);
	LI(InetPtonA)(AF_INET, xorstr("127.0.0.1"), &addr.sin_addr);
	if (LI(bind)(listen_sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
		LI(closesocket)(listen_sock);
		return nullptr;
	}
	if (LI(listen)(listen_sock, SOMAXCONN) != 0) {
		LI(closesocket)(listen_sock);
		return nullptr;
	}

	sockaddr_in bound {};
	int len = sizeof(bound);
	LI(getsockname)(listen_sock, (sockaddr *)&bound, &len);
	srv->m_port = LI(ntohs)(bound.sin_port);
	*out_port = srv->m_port;
	*out_edge = chosen;

	srv->m_accept_thread = std::thread([srv = srv.get(), listen_sock, st]() {
		while (!srv->m_stop) {
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(listen_sock, &fds);
			timeval tv {1, 0};
			int r = LI(select)(0, &fds, nullptr, nullptr, &tv);
			if (r <= 0)
				continue;
			sockaddr_in peer {};
			int plen = sizeof(peer);
			SOCKET client = LI(accept)(listen_sock, (sockaddr *)&peer, &plen);
			if (client == INVALID_SOCKET)
				continue;
			std::thread(handle_client, client, st.get()).detach();
		}
	});

	srv->m_watch_thread = std::thread([st]() {
		while (true) {
			LI(Sleep)(1000);
			std::lock_guard<std::mutex> lk(st->m_lock);
			apply_edge_controls(st.get());
		}
	});

	return srv;
}
