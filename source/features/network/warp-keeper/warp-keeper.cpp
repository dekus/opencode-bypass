#include "features/network/warp-keeper/warp-keeper.hpp"

#include <windows.h>

#include <cstdlib>

#include "features/network/zen-proxy/zen-proxy.hpp"
#include "ouc-common/ouc-common.hpp"
#include "ouc-firewall/ouc-firewall.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/xorstr/xorstr.hpp"

static const int k_tick_seconds = 45;

static bool ensure_connected(const std::string &warp_bin) {
	bool connected = false;
	std::string mode, raw;
	ouc_warp_status(&warp_bin, &connected, &mode, &raw);

	if (!mode.empty() && mode.find(xorstr("warp")) == std::string::npos)
		ouc_run_warp({xorstr("mode"), xorstr("warp")}, &warp_bin);
	else if (mode.empty())
		ouc_run_warp({xorstr("mode"), xorstr("warp")}, &warp_bin);

	if (connected && ouc_get_warp_local_v6())
		return true;

	ouc_run_warp({xorstr("connect")}, &warp_bin);
	LI(Sleep)(4000);
	ouc_warp_status(&warp_bin, &connected, &mode, &raw);
	if (connected && ouc_get_warp_local_v6()) {
		ouc_append_log(g_keeper_log_file, xorstr("reconnected existing registration"));
		return true;
	}

	ouc_append_log(g_keeper_log_file, xorstr("tunnel dead — rotating registration"));
	ouc_run_warp({xorstr("disconnect")}, &warp_bin);
	ouc_run_warp({xorstr("registration"), xorstr("delete")}, &warp_bin);
	auto wr = ouc_run_warp({xorstr("registration"), xorstr("new")}, &warp_bin);
	if (wr.code != 0) {
		ouc_append_log(g_keeper_log_file, std::string(xorstr("registration new failed: ")) + wr.output);
		return false;
	}
	ouc_run_warp({xorstr("mode"), xorstr("warp")}, &warp_bin);
	ouc_run_warp({xorstr("connect")}, &warp_bin);
	LI(Sleep)(5000);
	ouc_warp_status(&warp_bin, &connected, &mode, &raw);
	bool ok = connected && ouc_get_warp_local_v6().has_value();
	ouc_append_log(g_keeper_log_file, ok ? xorstr("tunnel repaired") : xorstr("repair still waiting"));
	return ok;
}

int warp_keeper_main() {
	ouc_ensure_dirs();
	if (!ouc_acquire_mutex(xorstr("OpencodeUnlimitedKeeper")))
		return 0;

	auto warp_opt = ouc_find_warp_cli();
	if (!warp_opt) {
		ouc_append_log(g_keeper_log_file, xorstr("warp-cli.exe not found — exiting"));
		return 1;
	}

	ouc_append_log(g_keeper_log_file, xorstr("warp-keeper up"));

	uint16_t port = 0;
	std::string edge;
	auto proxy = zen_proxy_start((uint16_t)g_service_proxy_port, &port, &edge);
	if (proxy) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), xorstr("service proxy on 127.0.0.1:%u edge=%s"), (unsigned)port, edge.c_str());
		ouc_append_log(g_keeper_log_file, buf);
	} else
		ouc_append_log(g_keeper_log_file, xorstr("service proxy bind failed"));

	for (int i = 0; i < 6; ++i) {
		auto wr = ouc_run_warp({xorstr("status")}, &*warp_opt, 8.0);
		if (wr.code == 0)
			break;
		LI(Sleep)(8000);
	}

	for (;;) {
		try {
			if (!ouc_is_keeper_paused())
				ensure_connected(*warp_opt);
			if (ouc_is_admin() &&
				(std::getenv(xorstr("OUC_FIREWALL_KILLSWITCH")) || ouc_firewall_killswitch_installed())) {
				if (!ouc_firewall_killswitch_installed())
					ouc_firewall_install_killswitch();
			}
		} catch (...) {
			ouc_append_log(g_keeper_log_file, xorstr("keeper error"));
		}
		LI(Sleep)(k_tick_seconds * 1000);
	}
}
