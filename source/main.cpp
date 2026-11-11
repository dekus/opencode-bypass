#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "features/lane/zen-lane/zen-lane.hpp"
#include "features/misc/doctor/doctor.hpp"
#include "features/misc/zen-hop/zen-hop.hpp"
#include "features/network/warp-keeper/warp-keeper.hpp"
#include "features/network/zen-guard/zen-guard.hpp"
#include "features/ui/opencode_bypass/opencode_bypass.hpp"
#include "ouc-common/ouc-common.hpp"
#include "ouc-firewall/ouc-firewall.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/xorstr/xorstr.hpp"

static void attach_parent_console() {
	if (LI(AttachConsole)(ATTACH_PARENT_PROCESS)) {
		freopen(xorstr("CONOUT$"), xorstr("w"), stdout);
		freopen(xorstr("CONOUT$"), xorstr("w"), stderr);
	}
}

static int run_console_mode(int argc, char **argv) {
	if (argc >= 2 && _stricmp(argv[1], xorstr("--worker")) == 0 && argc >= 3) {
		if (_stricmp(argv[2], xorstr("keeper")) == 0)
			return warp_keeper_main();
		if (_stricmp(argv[2], xorstr("guard")) == 0)
			return zen_guard_main();
	}
	if (argc >= 2 && _stricmp(argv[1], xorstr("--firewall")) == 0) {
		attach_parent_console();
		return ouc_firewall_main(argc - 1, argv + 1);
	}
	if (argc >= 2 && _stricmp(argv[1], xorstr("--lane")) == 0) {
		attach_parent_console();
		return zen_lane_main(argc - 1, argv + 1);
	}
	if (argc >= 2 && _stricmp(argv[1], xorstr("--hop")) == 0) {
		attach_parent_console();
		return zen_hop_main();
	}
	if (argc >= 2 && _stricmp(argv[1], xorstr("--doctor")) == 0) {
		attach_parent_console();
		return doctor_main();
	}
	return -1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR cmd_line, int) {
	ouc_init_paths();

	int argc = 0;
	LPWSTR *wide_argv = LI(CommandLineToArgvW)(LI(GetCommandLineW)(), &argc);
	if (!wide_argv)
		return 1;

	std::vector<std::string> argv;
	argv.reserve((size_t)argc);
	for (int i = 0; i < argc; ++i) {
		int n = LI(WideCharToMultiByte)(CP_UTF8, 0, wide_argv[i], -1, nullptr, 0, nullptr, nullptr);
		std::string s((size_t)n, '\0');
		LI(WideCharToMultiByte)(CP_UTF8, 0, wide_argv[i], -1, s.data(), n, nullptr, nullptr);
		if (!s.empty() && s.back() == '\0')
			s.pop_back();
		argv.push_back(std::move(s));
	}
	LI(LocalFree)(wide_argv);

	std::vector<char *> cargv;
	for (auto &a : argv)
		cargv.push_back(a.data());
	int argc_c = (int)cargv.size();

	int console_rc = run_console_mode(argc_c, cargv.data());
	if (console_rc >= 0)
		return console_rc;

	return opencode_bypass_main(instance);
}
