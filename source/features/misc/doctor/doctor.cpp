#include "features/misc/doctor/doctor.hpp"

#include <cstdio>
#include <fstream>

#include "ouc-common/ouc-common.hpp"
#include "ouc-firewall/ouc-firewall.hpp"
#include "vendor/nlohmann/json.hpp"
#include "vendor/xorstr/xorstr.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

static void print_ok(const char *msg) {
	printf("  [OK] %s\n", msg);
}

static void print_warn(const char *msg) {
	printf("  [!]  %s\n", msg);
}

static void print_bad(const char *msg) {
	printf("  [X]  %s\n", msg);
}

static bool task_exists(const char *name) {
	auto res = ouc_run_hidden({xorstr("schtasks"), xorstr("/Query"), xorstr("/TN"), name}, 6.0);
	return res.exit_code == 0;
}

int doctor_main() {
	printf("\n  =============================\n");
	printf("  Opencode Unlimited — doctor\n");
	printf("  =============================\n\n");

	if (ouc_is_mutex_held(xorstr("OpencodeUnlimitedKeeper")))
		print_ok(xorstr("warp-keeper process: running"));
	else if (task_exists(k_task_keeper))
		print_bad(xorstr("warp-keeper down (task installed) — run scheduled task"));
	else
		print_bad(xorstr("warp-keeper not running & scheduled task missing — re-run install"));

	auto warp_bin = ouc_find_warp_cli();
	if (!warp_bin)
		print_bad(xorstr("warp-cli.exe not found — install Cloudflare WARP"));
	else {
		bool connected = false;
		std::string mode, raw;
		ouc_warp_status(&*warp_bin, &connected, &mode, &raw);
		if (connected) {
			char buf[256];
			std::snprintf(buf, sizeof(buf), xorstr("warp tunnel: connected%s%s"),
				mode.empty() ? "" : " (mode: ", mode.empty() ? "" : mode.c_str());
			if (!mode.empty())
				std::strcat(buf, ")");
			print_ok(buf);
		} else {
			char buf[256];
			std::snprintf(buf, sizeof(buf), xorstr("warp not connected (%s)"), raw.empty() ? "unknown" : raw.c_str());
			print_bad(buf);
		}
	}

	auto local = ouc_get_warp_local_v6();
	if (local) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), xorstr("WARP adapter IPv6: %s"), local->c_str());
		print_ok(buf);
	} else
		print_bad(xorstr("CloudflareWARP adapter IPv6 missing"));

	auto egress = ouc_get_egress_v6(8.0);
	if (!egress.empty()) {
		auto prefix = ouc_normalize_v6_64(egress);
		if (ouc_is_cloudflare_v6(egress)) {
			char buf[160];
			std::snprintf(buf, sizeof(buf), xorstr("egress is cloudflare (warp): %s/64"), prefix.c_str());
			print_ok(buf);
		} else {
			char buf[160];
			std::snprintf(buf, sizeof(buf), xorstr("egress is NOT warp (%s)"), egress.c_str());
			print_bad(buf);
		}
	} else
		print_warn(xorstr("no IPv6 egress check response"));

	if (ouc_is_mutex_held(xorstr("OpencodeUnlimitedGuard")))
		print_ok(xorstr("zen-guard process: running"));
	else if (task_exists(k_task_guard))
		print_bad(xorstr("zen-guard down (task installed)"));
	else
		print_bad(xorstr("zen-guard not running & scheduled task missing"));

	if (std::filesystem::is_regular_file(g_guard_state_file)) {
		try {
			auto data = nlohmann::json::parse(ouc_read_file_text(g_guard_state_file));
			int hops = data.value(xorstr("hops"), 0);
			int bl = 0;
			if (data.contains(xorstr("blacklist")) && data[xorstr("blacklist")].is_array()) {
				for (auto &x : data[xorstr("blacklist")])
					if (x.is_string() && !x.get<std::string>().empty())
						++bl;
			}
			printf("       rotations so far: %d · blacklisted /64s: %d\n", hops, bl);
		} catch (...) {
			printf("       guard state unreadable\n");
		}
	}

	if (ouc_firewall_killswitch_installed())
		print_ok(xorstr("firewall kill-switch installed"));
	else
		print_warn(xorstr("firewall kill-switch not installed"));

	if (auto oc = ouc_find_opencode_cli()) {
		char buf[512];
		std::snprintf(buf, sizeof(buf), xorstr("real opencode CLI: %s"), oc->c_str());
		print_ok(buf);
	} else
		print_bad(xorstr("real opencode CLI not found"));

	bool proxy_up = false;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s != INVALID_SOCKET) {
		sockaddr_in addr {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)g_service_proxy_port);
		inet_pton(AF_INET, xorstr("127.0.0.1"), &addr.sin_addr);
		if (connect(s, (sockaddr *)&addr, sizeof(addr)) == 0)
			proxy_up = true;
		closesocket(s);
	}

	printf("\n  all [OK] = ready.\n\n");
	return 0;
}
