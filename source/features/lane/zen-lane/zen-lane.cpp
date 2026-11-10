#include "features/lane/zen-lane/zen-lane.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#include "features/network/zen-proxy/zen-proxy.hpp"
#include "ouc-common/ouc-common.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/xorstr/xorstr.hpp"

static const std::unordered_set<std::string> k_no_tunnel = {
	xorstr("service"), xorstr("upgrade"), xorstr("update"), xorstr("uninstall"), xorstr("debug")};

static bool needs_tunnel(const std::vector<std::string> &args) {
	static const char *k_info[] = {"--help", "-h", "--version", "-v", "--completions"};
	for (auto &a : args) {
		for (auto *f : k_info) {
			if (a == f || a.rfind(xorstr("--completions="), 0) == 0)
				return false;
		}
	}
	for (auto &a : args) {
		if (a.empty() || a[0] == '-')
			continue;
		return k_no_tunnel.count(a) == 0;
	}
	return true;
}

static std::optional<std::string> ensure_warp_up() {
	if (auto v6 = ouc_get_warp_local_v6())
		return v6;
	auto warp = ouc_find_warp_cli();
	if (!warp)
		return std::nullopt;
	ouc_run_warp({xorstr("mode"), xorstr("warp")}, &*warp);
	ouc_run_warp({xorstr("connect")}, &*warp);
	for (int i = 0; i < 10; ++i) {
		LI(Sleep)(1000);
		if (auto v6 = ouc_get_warp_local_v6())
			return v6;
	}
	return std::nullopt;
}

int zen_lane_main(int argc, char **argv) {
	bool desktop_mode = false;
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (!desktop_mode && a == xorstr("--ouc-desktop")) {
			desktop_mode = true;
			continue;
		}
		args.push_back(a);
	}

	std::string target;
	if (desktop_mode) {
		auto d = ouc_find_opencode_desktop();
		if (!d) {
			fprintf(stderr, "%s\n", xorstr("OpenCode.exe desktop app not found."));
			return 1;
		}
		target = *d;
	} else {
		auto c = ouc_find_opencode_cli();
		if (!c) {
			fprintf(stderr, "%s\n", xorstr("opencode CLI binary not found"));
			return 1;
		}
		target = *c;
	}

	if (!needs_tunnel(args) && !desktop_mode) {
		std::string cmd = "\"" + target + "\"";
		for (auto &a : args)
			cmd += " \"" + a + "\"";
		return (int)LI(system)(cmd.c_str());
	}

	if (!ensure_warp_up()) {
		fprintf(stderr, "%s\n", xorstr("[zen-lane] WARP IPv6 tunnel is not up (fail-closed)."));
		return 1;
	}

	uint16_t port = 0;
	std::string edge;
	auto proxy = zen_proxy_start(0, &port, &edge);
	if (!proxy)
		return 1;

	char proxy_url[64];
	std::snprintf(proxy_url, sizeof(proxy_url), xorstr("http://127.0.0.1:%u"), (unsigned)port);

	_putenv_s(xorstr("HTTPS_PROXY"), proxy_url);
	_putenv_s(xorstr("HTTP_PROXY"), proxy_url);
	_putenv_s(xorstr("https_proxy"), proxy_url);
	_putenv_s(xorstr("http_proxy"), proxy_url);
	_putenv_s(xorstr("NO_PROXY"), xorstr("127.0.0.1,localhost,::1"));
	_putenv_s(xorstr("no_proxy"), xorstr("127.0.0.1,localhost,::1"));

	std::string cmd_line = "\"" + target + "\"";
	if (desktop_mode)
		cmd_line += std::string(" ") + xorstr("--proxy-server=") + proxy_url;
	else if (needs_tunnel(args)) {
		bool has_standalone = false;
		for (auto &a : args) {
			if (a == xorstr("--standalone") || a.rfind(xorstr("--server"), 0) == 0)
				has_standalone = true;
		}
		if (!has_standalone)
			cmd_line += std::string(" ") + xorstr("--standalone");
	}
	for (auto &a : args)
		cmd_line += " \"" + a + "\"";

	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	std::vector<char> mut(cmd_line.begin(), cmd_line.end());
	mut.push_back('\0');
	if (!LI(CreateProcessA)(nullptr, mut.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
			&pi)) {
		proxy->shutdown();
		return 1;
	}
	LI(WaitForSingleObject)(pi.hProcess, INFINITE);
	DWORD ec = 1;
	LI(GetExitCodeProcess)(pi.hProcess, &ec);
	LI(CloseHandle)(pi.hProcess);
	LI(CloseHandle)(pi.hThread);
	proxy->shutdown();
	return (int)ec;
}
