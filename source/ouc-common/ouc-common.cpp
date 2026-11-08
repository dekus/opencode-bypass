#include "ouc-common/ouc-common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <ShlObj.h>

#include "vendor/lazy-importer/li.hpp"
#include "vendor/nlohmann/json.hpp"
#include "vendor/xorstr/xorstr.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;
using json = nlohmann::json;

std::filesystem::path g_dest_dir;
std::filesystem::path g_scripts_dir;
std::filesystem::path g_bin_dir;
std::filesystem::path g_guard_dir;
std::filesystem::path g_keeper_dir;
std::filesystem::path g_lane_dir;
std::filesystem::path g_pause_file;
std::filesystem::path g_guard_state_file;
std::filesystem::path g_guard_log_file;
std::filesystem::path g_keeper_log_file;
std::filesystem::path g_opencode_log_file;
int g_service_proxy_port = 41987;

static bool g_wsock_up = false;

static void ensure_wsock() {
	if (g_wsock_up)
		return;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
		g_wsock_up = true;
}

void ouc_init_paths() {
	const char *port_key = xorstr("OUC_SERVICE_PROXY_PORT");
	char *port_env = std::getenv(port_key);
	if (port_env) {
		int p = std::atoi(port_env);
		if (p > 0 && p < 65536)
			g_service_proxy_port = p;
	}

	char local_appdata[MAX_PATH];
	if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_appdata) != S_OK)
		local_appdata[0] = '\0';

	g_dest_dir = fs::path(local_appdata) / k_brand;
	g_scripts_dir = g_dest_dir / fs::path(xorstr("scripts"));
	g_bin_dir = g_dest_dir / fs::path(xorstr("bin"));
	g_guard_dir = g_dest_dir / fs::path(xorstr("guard"));
	g_keeper_dir = g_dest_dir / fs::path(xorstr("keeper"));
	g_lane_dir = g_dest_dir / fs::path(xorstr("lane"));
	g_pause_file = g_dest_dir / fs::path(xorstr("keeper.pause"));
	g_guard_state_file = g_guard_dir / fs::path(xorstr("state.json"));
	g_guard_log_file = g_guard_dir / fs::path(xorstr("guard.log"));
	g_keeper_log_file = g_keeper_dir / fs::path(xorstr("keeper.log"));

	char user_profile[MAX_PATH];
	if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, user_profile) == S_OK)
		g_opencode_log_file = fs::path(user_profile) / fs::path(xorstr(".local/share/opencode/log/opencode.log"));
	else
		g_opencode_log_file = fs::path();

	ensure_wsock();
}

void ouc_ensure_dirs() {
	for (const auto &d : {g_dest_dir, g_scripts_dir, g_bin_dir, g_guard_dir, g_keeper_dir, g_lane_dir})
		fs::create_directories(d);
}

static std::string quote_arg(const std::string &a) {
	if (a.find(' ') == std::string::npos && a.find('"') == std::string::npos)
		return a;
	std::string out = "\"";
	for (char c : a) {
		if (c == '"')
			out += "\\\"";
		else
			out.push_back(c);
	}
	out += '"';
	return out;
}

ouc_proc_result ouc_run_hidden(const std::vector<std::string> &args, double timeout_sec) {
	ouc_proc_result res;
	if (args.empty())
		return res;

	std::string cmd_line;
	for (std::size_t i = 0; i < args.size(); ++i) {
		if (i)
			cmd_line.push_back(' ');
		cmd_line += quote_arg(args[i]);
	}

	SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
	HANDLE rd_out = nullptr;
	HANDLE wr_out = nullptr;
	if (!LI(CreatePipe)(&rd_out, &wr_out, &sa, 0)) {
		res.exit_code = -1;
		return res;
	}
	LI(SetHandleInformation)(rd_out, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr_out;
	si.hStdError = wr_out;
	si.hStdInput = LI(GetStdHandle)(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi = {};
	std::vector<char> cmd_mut(cmd_line.begin(), cmd_line.end());
	cmd_mut.push_back('\0');

	DWORD flags = CREATE_NO_WINDOW;
	if (!LI(CreateProcessA)(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi)) {
		LI(CloseHandle)(wr_out);
		LI(CloseHandle)(rd_out);
		res.exit_code = -1;
		return res;
	}
	LI(CloseHandle)(wr_out);

	std::string accum;
	char buf[4096];
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(timeout_sec * 1000.0));

	for (;;) {
		DWORD avail = 0;
		if (!LI(PeekNamedPipe)(rd_out, nullptr, 0, nullptr, &avail, nullptr))
			break;

		if (avail > 0) {
			DWORD got = 0;
			DWORD to_read = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
			if (LI(ReadFile)(rd_out, buf, to_read, &got, nullptr) && got)
				accum.append(buf, got);
		}

		DWORD wait = LI(WaitForSingleObject)(pi.hProcess, avail ? 0 : 50);
		if (wait == WAIT_OBJECT_0)
			break;

		if (std::chrono::steady_clock::now() >= deadline) {
			LI(TerminateProcess)(pi.hProcess, 1);
			break;
		}
	}

	while (true) {
		DWORD got = 0;
		if (!LI(ReadFile)(rd_out, buf, sizeof(buf), &got, nullptr) || !got)
			break;
		accum.append(buf, got);
	}

	DWORD ec = 1;
	LI(GetExitCodeProcess)(pi.hProcess, &ec);
	LI(CloseHandle)(pi.hProcess);
	LI(CloseHandle)(pi.hThread);
	LI(CloseHandle)(rd_out);

	res.exit_code = (int)ec;
	res.output = std::move(accum);
	return res;
}

static bool is_file_path(const fs::path &p) {
	std::error_code ec;
	return fs::is_regular_file(p, ec);
}

std::optional<std::string> ouc_find_warp_cli() {
	char *env_bin = std::getenv(xorstr("OUC_WARP_BIN"));
	if (env_bin && *env_bin) {
		fs::path p(env_bin);
		if (is_file_path(p))
			return p.string();
	}

	const fs::path candidates[] = {
		fs::path(xorstr("C:/Program Files/Cloudflare/Cloudflare WARP/warp-cli.exe")),
		fs::path(xorstr("C:/Program Files (x86)/Cloudflare/Cloudflare WARP/warp-cli.exe")),
	};
	char local_appdata[MAX_PATH];
	if (LI(SHGetFolderPathA)(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_appdata) == S_OK) {
		fs::path la(local_appdata);
		fs::path extra = la / xorstr("Cloudflare/Cloudflare WARP/warp-cli.exe");
		for (const auto &c : candidates) {
			if (is_file_path(c))
				return c.string();
		}
		if (is_file_path(extra))
			return extra.string();
	}
	for (const auto &c : candidates) {
		if (is_file_path(c))
			return c.string();
	}
	return std::nullopt;
}

std::optional<std::string> ouc_find_opencode_cli() {
	char *env_bin = std::getenv(xorstr("OUC_REAL_BIN"));
	if (env_bin && *env_bin) {
		fs::path p(env_bin);
		if (is_file_path(p))
			return p.string();
	}

	char local_appdata[MAX_PATH];
	char user_profile[MAX_PATH];
	LI(SHGetFolderPathA)(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_appdata);
	LI(SHGetFolderPathA)(nullptr, CSIDL_PROFILE, nullptr, 0, user_profile);

	const fs::path paths[] = {
		fs::path(local_appdata) / xorstr("Programs/ai.opencode.desktop/resources/opencode-cli.exe"),
		fs::path(user_profile) / xorstr(".opencode/bin/opencode.exe"),
		fs::path(local_appdata) / xorstr("opencode/opencode.exe"),
		fs::path(user_profile) / xorstr(".bun/bin/opencode.exe"),
	};
	for (const auto &p : paths) {
		if (is_file_path(p))
			return p.string();
	}
	return std::nullopt;
}

std::optional<std::string> ouc_find_opencode_desktop() {
	char local_appdata[MAX_PATH];
	if (LI(SHGetFolderPathA)(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_appdata) != S_OK)
		return std::nullopt;
	fs::path desktop = fs::path(local_appdata) / xorstr("Programs/ai.opencode.desktop/OpenCode.exe");
	if (is_file_path(desktop))
		return desktop.string();
	return std::nullopt;
}

ouc_warp_run ouc_run_warp(const std::vector<std::string> &args, const std::string *warp_bin, double timeout_sec) {
	ouc_warp_run out;
	auto exe = warp_bin ? *warp_bin : ouc_find_warp_cli().value_or("");
	if (exe.empty()) {
		out.output = xorstr("warp-cli.exe not found");
		return out;
	}

	std::vector<std::string> attempts[2];
	attempts[0] = {exe, xorstr("--accept-tos")};
	attempts[0].insert(attempts[0].end(), args.begin(), args.end());
	attempts[1] = {exe};
	attempts[1].insert(attempts[1].end(), args.begin(), args.end());

	for (auto &cmd : attempts) {
		auto res = ouc_run_hidden(cmd, timeout_sec);
		out.code = res.exit_code;
		out.output = res.output;
		if (res.exit_code == 0)
			break;
	}
	return out;
}

std::string ouc_warp_mode(const std::string *warp_bin) {
	auto wr = ouc_run_warp({xorstr("-j"), xorstr("settings"), xorstr("list")}, warp_bin, 6.0);
	if (wr.code == 0 && !wr.output.empty() && wr.output[0] == '{') {
		try {
			auto data = json::parse(wr.output);
			if (data.contains(xorstr("settings")) && data[xorstr("settings")].contains(xorstr("operation_mode")))
				return data[xorstr("settings")][xorstr("operation_mode")].get<std::string>();
			if (data.contains(xorstr("operation_mode")))
				return data[xorstr("operation_mode")].get<std::string>();
		} catch (...) {
		}
	}
	return "";
}

bool ouc_warp_status(const std::string *warp_bin, bool *connected, std::string *mode, std::string *raw) {
	*connected = false;
	mode->clear();
	raw->clear();

	auto wr = ouc_run_warp({xorstr("-j"), xorstr("status")}, warp_bin, 8.0);
	if (wr.code == 0 && !wr.output.empty() && wr.output[0] == '{') {
		try {
			auto data = json::parse(wr.output);
			std::string status = data.value(xorstr("status"), "");
			*raw = status;
			*connected = _stricmp(status.c_str(), xorstr("connected")) == 0;
			if (data.contains(xorstr("reason"))) {
				auto &reason = data[xorstr("reason")];
				if (reason.is_object() && reason.contains(xorstr("SettingsChanged"))) {
					auto &sc = reason[xorstr("SettingsChanged")];
					if (sc.is_object() && sc.contains(xorstr("current"))) {
						auto &cur = sc[xorstr("current")];
						if (cur.is_object() && cur.contains(xorstr("operation_mode"))) {
							auto &om = cur[xorstr("operation_mode")];
							if (om.is_string())
								*mode = om.get<std::string>();
						}
					}
				}
			}
			if (mode->empty())
				*mode = ouc_warp_mode(warp_bin);

			return true;
		} catch (...) {
		}
	}

	auto wr2 = ouc_run_warp({xorstr("status")}, warp_bin, 8.0);
	std::string first;
	if (!wr2.output.empty()) {
		auto pos = wr2.output.find('\n');
		first = pos == std::string::npos ? wr2.output : wr2.output.substr(0, pos);
	}
	*raw = first;
	*connected = wr2.code == 0 && first.find(xorstr("Connected")) != std::string::npos &&
		first.find(xorstr("Disconnected")) == std::string::npos;
	return true;
}

bool ouc_is_cloudflare_v6(const std::string &addr) {
	std::string clean = addr;
	auto pct = clean.find('%');
	if (pct != std::string::npos)
		clean = clean.substr(0, pct);

	IN6_ADDR in6 {};
	if (LI(InetPtonA)(AF_INET6, clean.c_str(), &in6) != 1)
		return false;

	bool is_ll = IN6_IS_ADDR_LINKLOCAL(&in6);
	bool is_loop = IN6_IS_ADDR_LOOPBACK(&in6);
	bool is_mc = IN6_IS_ADDR_MULTICAST(&in6);
	if (is_ll || is_loop || is_mc)
		return false;

	std::string low = clean;
	for (auto &c : low)
		c = (char)tolower((unsigned char)c);

	return low.rfind(xorstr("2606:"), 0) == 0 || low.rfind(xorstr("2a09:"), 0) == 0;
}

std::optional<std::string> ouc_get_warp_local_v6() {
	auto res = ouc_run_hidden(
		{xorstr("netsh"), xorstr("interface"), xorstr("ipv6"), xorstr("show"), xorstr("addresses"),
			xorstr("CloudflareWARP")},
		5.0);
	if (res.exit_code != 0)
		return std::nullopt;

	static const std::regex k_v6_re(R"(\b([0-9a-fA-F:]+:[0-9a-fA-F:]+)\b)");
	std::istringstream iss(res.output);
	std::string line;
	while (std::getline(iss, line)) {
		std::smatch m;
		if (!std::regex_search(line, m, k_v6_re))
			continue;
		std::string cand = m[1].str();
		auto pct = cand.find('%');
		if (pct != std::string::npos)
			cand = cand.substr(0, pct);

		IN6_ADDR in6 {};
		if (LI(InetPtonA)(AF_INET6, cand.c_str(), &in6) != 1)
			continue;
		if (IN6_IS_ADDR_LINKLOCAL(&in6) || IN6_IS_ADDR_LOOPBACK(&in6) || IN6_IS_ADDR_MULTICAST(&in6))
			continue;

		if (ouc_is_cloudflare_v6(cand) || !IN6_IS_ADDR_LINKLOCAL(&in6))
			return cand;
	}
	return std::nullopt;
}

std::string ouc_get_egress_v6(double timeout_sec) {
	auto curl_res = ouc_run_hidden(
		{xorstr("curl.exe"), xorstr("-6"), xorstr("-s"), xorstr("-m"),
			std::to_string((int)timeout_sec), xorstr("https://api64.ipify.org")},
		timeout_sec + 2.0);
	std::string ip = curl_res.output;
	while (!ip.empty() && (ip.back() == '\r' || ip.back() == '\n' || ip.back() == ' '))
		ip.pop_back();
	if (!ip.empty() && ip.find(':') != std::string::npos) {
		IN6_ADDR in6 {};
		if (LI(InetPtonA)(AF_INET6, ip.c_str(), &in6) == 1)
			return ip;
	}

	return "";
}

std::string ouc_normalize_v6_64(const std::string &addr) {
	std::string clean = addr;
	auto pct = clean.find('%');
	if (pct != std::string::npos)
		clean = clean.substr(0, pct);
	if (clean.empty() || clean.find(':') == std::string::npos)
		return "";

	IN6_ADDR in6 {};
	if (LI(InetPtonA)(AF_INET6, clean.c_str(), &in6) != 1)
		return "";

	in6.u.Word[4] = 0;
	in6.u.Word[5] = 0;
	in6.u.Word[6] = 0;
	in6.u.Word[7] = 0;

	char buf[INET6_ADDRSTRLEN] = {};
	if (!LI(InetNtopA)(AF_INET6, &in6, buf, sizeof(buf)))
		return "";
	return buf;
}

bool ouc_is_keeper_paused() {
	if (!is_file_path(g_pause_file))
		return false;
	try {
		auto text = ouc_read_file_text(g_pause_file);
		auto data = json::parse(text);
		double until = data.value(xorstr("until"), 0.0);
		auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
		if (now < until)
			return true;
		fs::remove(g_pause_file);
	} catch (...) {
	}
	return false;
}

void ouc_keeper_pause_begin(const std::string &reason, int seconds) {
	ouc_ensure_dirs();
	json payload;
	auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	payload[xorstr("until")] = now + (double)seconds;
	payload[xorstr("reason")] = reason;
	payload[xorstr("pid")] = (int)LI(GetCurrentProcessId)();
	ouc_write_file_text(g_pause_file, payload.dump());
}

void ouc_keeper_pause_end() {
	std::error_code ec;
	fs::remove(g_pause_file, ec);
}

void *ouc_acquire_mutex(const char *name) {
	HANDLE h = LI(CreateMutexA)(nullptr, FALSE, (std::string(xorstr("Local\\")) + name).c_str());
	if (!h)
		return nullptr;
	if (LI(GetLastError)() == ERROR_ALREADY_EXISTS) {
		LI(CloseHandle)(h);
		return nullptr;
	}
	return h;
}

bool ouc_is_mutex_held(const char *name) {
	HANDLE h = LI(OpenMutexA)(SYNCHRONIZE, FALSE, (std::string(xorstr("Local\\")) + name).c_str());
	if (!h)
		return false;
	LI(CloseHandle)(h);
	return true;
}

bool ouc_is_admin() {
	BOOL admin = FALSE;
	PSID grp = nullptr;
	SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
	if (LI(AllocateAndInitializeSid)(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
		LI(CheckTokenMembership)(nullptr, grp, &admin);
		LI(FreeSid)(grp);
	}
	return admin == TRUE;
}

void ouc_append_log(const fs::path &path, const std::string &message, std::size_t max_bytes) {
	fs::create_directories(path.parent_path());
	try {
		if (is_file_path(path) && fs::file_size(path) > max_bytes) {
			auto bak = path;
			bak += xorstr(".1");
			fs::remove(bak);
			fs::rename(path, bak);
		}
	} catch (...) {
	}

	auto t = std::time(nullptr);
	std::tm tm {};
	localtime_s(&tm, &t);
	char stamp[32];
	std::snprintf(stamp, sizeof(stamp), xorstr("%02d-%02d %02d:%02d:%02d"), tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		tm.tm_min, tm.tm_sec);

	std::ofstream fh(path, std::ios::app);
	if (fh)
		fh << stamp << ' ' << message << '\n';
}

static bool fetch_doh(const char *url, std::vector<std::string> &out) {
	auto res = ouc_run_hidden(
		{xorstr("curl.exe"), xorstr("-s"), xorstr("-m"), xorstr("4"), xorstr("-H"), xorstr("Accept: application/dns-json"),
			url},
		5.0);
	std::string body = res.output;

	try {
		auto data = json::parse(body);
		if (!data.contains(xorstr("Answer")))
			return false;
		for (auto &ans : data[xorstr("Answer")]) {
			if (ans.value(xorstr("type"), 0) != 28)
				continue;
			std::string ip = ans.value(xorstr("data"), "");
			IN6_ADDR in6 {};
			if (LI(InetPtonA)(AF_INET6, ip.c_str(), &in6) == 1)
				out.push_back(ip);
		}
	} catch (...) {
		return false;
	}
	return true;
}

std::vector<std::string> ouc_fetch_opencode_aaaa_pool() {
	std::vector<std::string> pool;
	const char *doh_urls[] = {
		xorstr("https://1.1.1.1/dns-query?name=opencode.ai&type=AAAA"),
		xorstr("https://1.0.0.1/dns-query?name=opencode.ai&type=AAAA"),
		xorstr("https://dns.google/resolve?name=opencode.ai&type=AAAA"),
	};
	for (auto *u : doh_urls)
		fetch_doh(u, pool);

	struct host_ctx {
		const char *name;
	} hosts[] = {{xorstr("opencode.ai")}, {xorstr("api.opencode.ai")}};
	for (auto &h : hosts) {
		addrinfo hints {};
		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *res = nullptr;
		if (LI(getaddrinfo)(h.name, xorstr("443"), &hints, &res) == 0) {
			for (auto *p = res; p; p = p->ai_next) {
				if (p->ai_family != AF_INET6)
					continue;
				char buf[INET6_ADDRSTRLEN] = {};
				auto *in = (sockaddr_in6 *)p->ai_addr;
				LI(InetNtopA)(AF_INET6, &in->sin6_addr, buf, sizeof(buf));
				pool.push_back(buf);
			}
			LI(freeaddrinfo)(res);
		}
	}

	std::sort(pool.begin(), pool.end());
	pool.erase(std::unique(pool.begin(), pool.end()), pool.end());

	if (pool.empty()) {
		const char *fallback[] = {
			xorstr("2606:4700::6812:a42"),
			xorstr("2606:4700::6812:b42"),
			xorstr("2606:4700:10::ac42:ad95"),
			xorstr("2606:4700:10::ac42:ad96"),
			xorstr("2606:4700:10::6814:2011"),
		};
		for (auto *f : fallback)
			pool.emplace_back(f);
	}
	return pool;
}

std::string ouc_hop_once(const std::string &warp_bin, int max_reg_tries) {
	ouc_run_warp({xorstr("disconnect")}, &warp_bin);
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	ouc_run_warp({xorstr("registration"), xorstr("delete")}, &warp_bin);
	std::this_thread::sleep_for(std::chrono::milliseconds(800));

	bool registered = false;
	for (int attempt = 0; attempt < max_reg_tries; ++attempt) {
		auto wr = ouc_run_warp({xorstr("registration"), xorstr("new")}, &warp_bin);
		if (wr.code == 0) {
			registered = true;
			break;
		}
		if (attempt + 1 < max_reg_tries)
			std::this_thread::sleep_for(std::chrono::seconds(6));
	}
	if (!registered)
		return "";

	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	ouc_run_warp({xorstr("mode"), xorstr("warp")}, &warp_bin);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	ouc_run_warp({xorstr("connect")}, &warp_bin);

	for (int idx = 0; idx < 12; ++idx) {
		std::this_thread::sleep_for(std::chrono::seconds(3));
		bool connected = false;
		std::string mode, raw;
		ouc_warp_status(&warp_bin, &connected, &mode, &raw);
		if (!connected) {
			if (idx == 2 || idx == 5)
				ouc_run_warp({xorstr("connect")}, &warp_bin);
			continue;
		}
		auto v6 = ouc_get_egress_v6(5.0);
		if (!v6.empty())
			return v6;
		auto local = ouc_get_warp_local_v6();
		if (local && ouc_is_cloudflare_v6(*local))
			return *local;
	}
	return "";
}

std::string ouc_exe_dir() {
	char path[MAX_PATH];
	if (!LI(GetModuleFileNameA)(nullptr, path, MAX_PATH))
		return "";
	fs::path p(path);
	return p.parent_path().string();
}

std::string ouc_read_file_text(const fs::path &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return "";
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool ouc_write_file_text(const fs::path &path, const std::string &text) {
	fs::create_directories(path.parent_path());
	std::ofstream f(path, std::ios::binary);
	if (!f)
		return false;
	f << text;
	return true;
}
