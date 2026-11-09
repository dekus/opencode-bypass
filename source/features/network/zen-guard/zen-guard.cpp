#include "features/network/zen-guard/zen-guard.hpp"

#include <windows.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>

#include "ouc-common/ouc-common.hpp"
#include "vendor/nlohmann/json.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/xorstr/xorstr.hpp"

using json = nlohmann::json;

static const int k_poll_seconds = 10;
static const int k_cooldown_seconds = 75;
static const int k_max_hop_tries = 8;
static const int k_blacklist_max = 200;
static const std::size_t k_max_read = 50000000;

static json default_state(std::size_t offset = 0) {
	return json {{xorstr("blacklist"), json::array()}, {xorstr("offset"), offset}, {xorstr("last_hop"), 0},
		{xorstr("hops"), 0}, {xorstr("current"), ""}};
}

static json load_state() {
	if (!std::filesystem::is_regular_file(g_guard_state_file)) {
		std::size_t offset = 0;
		if (std::filesystem::is_regular_file(g_opencode_log_file))
			offset = std::filesystem::file_size(g_opencode_log_file);
		auto st = default_state(offset);
		ouc_write_file_text(g_guard_state_file, st.dump());
		return st;
	}
	try {
		auto data = json::parse(ouc_read_file_text(g_guard_state_file));
		auto st = default_state();
		for (auto it = data.begin(); it != data.end(); ++it)
			st[it.key()] = it.value();
		return st;
	} catch (...) {
	}
	return default_state();
}

static void save_state(const json &st) {
	ouc_write_file_text(g_guard_state_file, st.dump());
}

static void blacklist_add(json &st, const std::string &addr) {
	auto prefix = ouc_normalize_v6_64(addr);
	if (prefix.empty())
		return;
	json bl = json::array();
	for (auto &x : st[xorstr("blacklist")]) {
		if (!x.is_string())
			continue;
		if (ouc_normalize_v6_64(x.get<std::string>()) != prefix)
			bl.push_back(x);
	}
	bl.push_back(prefix);
	while ((int)bl.size() > k_blacklist_max)
		bl.erase(bl.begin());
	st[xorstr("blacklist")] = bl;
}

static bool is_burnt(const json &st, const std::string &addr) {
	auto prefix = ouc_normalize_v6_64(addr);
	if (prefix.empty())
		return false;
	for (auto &x : st[xorstr("blacklist")]) {
		if (!x.is_string())
			continue;
		if (ouc_normalize_v6_64(x.get<std::string>()) == prefix)
			return true;
	}
	return false;
}

static bool read_new_log(json &st) {
	if (!std::filesystem::is_regular_file(g_opencode_log_file))
		return false;

	std::size_t size = 0;
	try {
		size = std::filesystem::file_size(g_opencode_log_file);
	} catch (...) {
		return false;
	}

	std::size_t offset = st.value(xorstr("offset"), 0);
	if (size < offset)
		offset = 0;

	bool wall = false;
	std::size_t delta = size - offset;
	if (delta > 0 && delta < k_max_read) {
		std::ifstream fh(g_opencode_log_file, std::ios::binary);
		fh.seekg((std::streamoff)offset);
		std::string chunk((std::istreambuf_iterator<char>(fh)), std::istreambuf_iterator<char>());
		if (chunk.find(xorstr("QuotaExceeded")) != std::string::npos ||
			chunk.find(xorstr("Free usage limit reached")) != std::string::npos)
			wall = true;
	}

	st[xorstr("offset")] = size;
	save_state(st);
	return wall;
}

static void rotate(json &st, const std::string &warp_bin) {
	auto now = (int)std::time(nullptr);
	std::string old_v6 = ouc_get_egress_v6(8.0);
	if (old_v6.empty()) {
		auto loc = ouc_get_warp_local_v6();
		if (loc)
			old_v6 = *loc;
	}
	ouc_append_log(g_guard_log_file, std::string(xorstr("wall detected — rotating (was: ")) + (old_v6.empty() ? "?" : old_v6) + ")");
	blacklist_add(st, old_v6);
	save_state(st);

	std::string new_v6;
	ouc_keeper_pause_begin(xorstr("zen-guard"), 300);
	for (int attempt = 1; attempt <= k_max_hop_tries; ++attempt) {
		auto cand = ouc_hop_once(warp_bin);
		if (cand.empty()) {
			ouc_append_log(g_guard_log_file, std::string(xorstr("try ")) + std::to_string(attempt) + xorstr(": tunnel repair failed"));
			continue;
		}
		if (is_burnt(st, cand)) {
			ouc_append_log(g_guard_log_file, std::string(xorstr("try ")) + std::to_string(attempt) + xorstr(": blacklisted /64"));
			continue;
		}
		new_v6 = cand;
		break;
	}
	if (new_v6.empty()) {
		ouc_append_log(g_guard_log_file, xorstr("everything blacklisted — recycling"));
		new_v6 = ouc_hop_once(warp_bin);
	}
	ouc_keeper_pause_end();

	st[xorstr("last_hop")] = now;
	if (!new_v6.empty()) {
		st[xorstr("current")] = new_v6;
		st[xorstr("hops")] = st.value(xorstr("hops"), 0) + 1;
		save_state(st);
		ouc_append_log(g_guard_log_file, std::string(xorstr("rotated #")) + std::to_string(st[xorstr("hops")].get<int>()) + ": ok");
	} else {
		save_state(st);
		ouc_append_log(g_guard_log_file, xorstr("rotation failed"));
	}
}

int zen_guard_main() {
	ouc_ensure_dirs();
	void *mtx = ouc_acquire_mutex(xorstr("OpencodeUnlimitedGuard"));
	if (!mtx)
		return 0;

	auto warp_opt = ouc_find_warp_cli();
	if (!warp_opt) {
		ouc_append_log(g_guard_log_file, xorstr("warp-cli.exe not found — exiting"));
		return 1;
	}

	auto state = load_state();
	ouc_append_log(g_guard_log_file, xorstr("zen-guard up"));

	for (;;) {
		LI(Sleep)(k_poll_seconds * 1000);
		try {
			if (!read_new_log(state))
				continue;
			auto now = (int)std::time(nullptr);
			int last_hop = state.value(xorstr("last_hop"), 0);
			int age = now - last_hop;
			if (age >= k_cooldown_seconds)
				rotate(state, *warp_opt);
			else
				ouc_append_log(g_guard_log_file, xorstr("wall seen but in cooldown"));
		} catch (...) {
			ouc_append_log(g_guard_log_file, xorstr("guard error"));
		}
	}
}
