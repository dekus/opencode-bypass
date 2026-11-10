#include "features/misc/zen-hop/zen-hop.hpp"

#include <cstdio>

#include "ouc-common/ouc-common.hpp"
#include "vendor/xorstr/xorstr.hpp"

int zen_hop_main() {
	auto warp_opt = ouc_find_warp_cli();
	if (!warp_opt) {
		fprintf(stderr, "%s\n", xorstr("warp-cli.exe not found"));
		return 1;
	}
	const std::string &warp_bin = *warp_opt;

	auto old_v6 = ouc_get_egress_v6(8.0);
	if (old_v6.empty()) {
		auto loc = ouc_get_warp_local_v6();
		if (loc)
			old_v6 = *loc;
	}

	printf("%s %s\n", xorstr("old /64:"), old_v6.empty() ? "?" : old_v6.c_str());
	printf("%s\n", xorstr("-- rotating (keeper paused) --"));

	ouc_keeper_pause_begin(xorstr("zen-hop"), 180);
	std::string new_v6 = ouc_hop_once(warp_bin, 5);
	ouc_keeper_pause_end();

	if (new_v6.empty()) {
		printf("%s\n", xorstr("-- registration/tunnel failed — keeper will keep retrying --"));
		return 1;
	}

	printf("%s %s\n", xorstr("new /64:"), new_v6.c_str());
	auto old_prefix = ouc_normalize_v6_64(old_v6);
	auto new_prefix = ouc_normalize_v6_64(new_v6);
	if (!new_prefix.empty() && new_prefix != old_prefix) {
		printf("%s\n", xorstr("== FRESH /64 — re-send your message =="));
		return 0;
	}
	if (new_v6 != old_v6) {
		printf("%s\n", xorstr("== FRESH /64 — re-send your message =="));
		return 0;
	}

	printf("%s\n", xorstr("== cloudflare handed back the same block — run me again =="));
	return 1;
}
