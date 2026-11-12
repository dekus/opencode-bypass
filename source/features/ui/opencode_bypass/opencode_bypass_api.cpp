#include "features/ui/opencode_bypass/opencode_bypass_api.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>

#include <windows.h>
#include <shellapi.h>

#include "ouc-common/ouc-common.hpp"
#include "ouc-firewall/ouc-firewall.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/nlohmann/json.hpp"
#include "vendor/xorstr/xorstr.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::mutex g_egress_mu;
static std::string g_cached_egress;
static bool g_egress_checking = false;
static double g_last_egress_ts = 0.0;
static std::string g_last_local_v6;

static std::mutex g_pool_mu;
static std::vector<std::string> g_edge_pool;

static std::atomic<bool> g_action_busy{false};

static const char *k_service_env_keys[] = {xorstr("HTTPS_PROXY"), xorstr("HTTP_PROXY"), xorstr("https_proxy"),
	xorstr("http_proxy"), xorstr("NO_PROXY"), xorstr("no_proxy")};

static double now_sec() {
	using clock = std::chrono::steady_clock;
	static const auto t0 = clock::now();
	return std::chrono::duration<double>(clock::now() - t0).count();
}

static void refresh_egress_finish(const std::string &v6) {
	std::lock_guard<std::mutex> lk(g_egress_mu);
	g_cached_egress = v6;
	g_last_egress_ts = now_sec();
	g_egress_checking = false;
}

static void refresh_egress_sync(bool force) {
	{
		std::lock_guard<std::mutex> lk(g_egress_mu);
		if (g_egress_checking)
			return;
		if (!force && (now_sec() - g_last_egress_ts) < 25.0)
			return;
		g_egress_checking = true;
	}
	std::string v6;
	try {
		v6 = ouc_get_egress_v6(4.0);
	} catch (...) {
	}
	refresh_egress_finish(v6);
}

void opencode_bypass_api_boot() {
	auto pool = ouc_fetch_opencode_aaaa_pool();
	if (!pool.empty()) {
		std::lock_guard<std::mutex> lk(g_pool_mu);
		g_edge_pool = std::move(pool);
	}
}

static bool try_action_lock() {
	bool expected = false;
	return g_action_busy.compare_exchange_strong(expected, true);
}

static void action_unlock() {
	g_action_busy.store(false);
}

static json read_guard_json() {
	if (!fs::is_regular_file(g_guard_state_file))
		return json::object();
	try {
		return json::parse(ouc_read_file_text(g_guard_state_file));
	} catch (...) {
		return json::object();
	}
}

static void write_guard_json(const json &j) {
	ouc_write_file_text(g_guard_state_file, j.dump());
}

static std::string lane_edge_override_lower() {
	std::string ov = xorstr("auto");
	const fs::path k_edge_ov = g_lane_dir / fs::path("edge_override.txt");
	if (fs::is_regular_file(k_edge_ov)) {
		ov = ouc_read_file_text(k_edge_ov);
		while (!ov.empty() && (ov.back() == '\n' || ov.back() == '\r'))
			ov.pop_back();
		for (auto &c : ov)
			c = (char)std::tolower((unsigned char)c);
	}
	return ov;
}

static std::string trim_copy(std::string s) {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
		s.pop_back();
	size_t i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
		++i;
	return s.substr(i);
}

void opencode_bypass_snapshot_refresh(opencode_bypass_snapshot *snap, bool force_egress) {
	auto warp_opt = ouc_find_warp_cli();
	bool connected = false;
	std::string mode, status_text;
	if (warp_opt)
		ouc_warp_status(&*warp_opt, &connected, &mode, &status_text);

	std::string local_v6 = ouc_get_warp_local_v6().value_or("");
	if (local_v6 != g_last_local_v6) {
		g_last_local_v6 = local_v6;
		if (!local_v6.empty())
			refresh_egress_sync(true);
		else {
			std::lock_guard<std::mutex> lk(g_egress_mu);
			g_cached_egress.clear();
		}
	} else if (force_egress) {
		refresh_egress_sync(true);
	}

	std::string egress_v6;
	bool checking = false;
	{
		std::lock_guard<std::mutex> lk(g_egress_mu);
		egress_v6 = g_cached_egress;
		checking = g_egress_checking;
	}
	(void)force_egress;

	snap->warp_installed = warp_opt.has_value();
	snap->warp_bin = warp_opt.value_or("");
	snap->warp_connected = connected;
	snap->warp_mode = mode;
	snap->warp_status_text =
		status_text.empty() ? (connected ? xorstr("Connected") : xorstr("Disconnected")) : status_text;
	snap->adapter_v6 = local_v6;
	snap->egress_v6 = egress_v6;
	snap->egress_prefix = egress_v6.empty() ? "" : ouc_normalize_v6_64(egress_v6);
	snap->is_cf = !egress_v6.empty() && ouc_is_cloudflare_v6(egress_v6);
	snap->checking_egress = checking;
	snap->rotating = ouc_is_keeper_paused();

	snap->keeper_running = ouc_is_mutex_held(xorstr("OpencodeUnlimitedKeeper"));
	snap->guard_running = ouc_is_mutex_held(xorstr("OpencodeUnlimitedGuard"));
	snap->keeper_task = opencode_bypass_task_exists(k_task_keeper);
	snap->guard_task = opencode_bypass_task_exists(k_task_guard);
	snap->proxy_listening = opencode_bypass_port_open(g_service_proxy_port);
	snap->proxy_port = g_service_proxy_port;
	snap->firewall_installed = ouc_firewall_killswitch_installed();
	snap->is_admin = ouc_is_admin();

	snap->pinned_edge.clear();
	const fs::path k_last_edge = g_lane_dir / fs::path("last_edge.txt");
	const fs::path k_edge_ov = g_lane_dir / fs::path("edge_override.txt");
	if (fs::is_regular_file(k_last_edge))
		snap->pinned_edge = trim_copy(ouc_read_file_text(k_last_edge));

	snap->edge_override = xorstr("auto");
	if (fs::is_regular_file(k_edge_ov))
		snap->edge_override = trim_copy(ouc_read_file_text(k_edge_ov));
	if (snap->edge_override.empty())
		snap->edge_override = xorstr("auto");

	{
		std::lock_guard<std::mutex> lk(g_pool_mu);
		snap->edge_pool = g_edge_pool;
	}

	snap->strict_mode = fs::is_regular_file(g_lane_dir / fs::path("strict_proxy.flag"));
	snap->cli_path = ouc_find_opencode_cli().value_or("");
	snap->desktop_path = ouc_find_opencode_desktop().value_or("");
	snap->path_wired = opencode_bypass_user_path_has_bin();
	snap->desktop_service_wired = opencode_bypass_desktop_service_proxy_wired();
	snap->scripts_installed = opencode_bypass_scripts_installed();

	auto st = read_guard_json();
	snap->hops = st.value("hops", 0);
	snap->last_hop = st.value("last_hop", (int64_t)0);
	snap->blacklist.clear();
	if (st.contains("blacklist") && st["blacklist"].is_array()) {
		for (auto it = st["blacklist"].rbegin(); it != st["blacklist"].rend(); ++it) {
			if (it->is_string() && !it->get<std::string>().empty())
				snap->blacklist.push_back(it->get<std::string>());
		}
	}
	snap->blacklist_count = (int)snap->blacklist.size();
	static DWORD last_db_ms = 0;
	DWORD tms = GetTickCount();
	if (last_db_ms != 0 && tms - last_db_ms > 30000) {
		snap->db = opencode_bypass_read_db_stats();
		last_db_ms = tms;
	} else if (last_db_ms == 0) {
		last_db_ms = tms;
	}
	snap->log_size_bytes = fs::is_regular_file(g_opencode_log_file) ? (int64_t)fs::file_size(g_opencode_log_file) : 0;
	snap->log_guard = opencode_bypass_tail_lines(g_guard_log_file, 75);
	snap->log_keeper = opencode_bypass_tail_lines(g_keeper_log_file, 75);
	snap->log_opencode = opencode_bypass_tail_lines(g_opencode_log_file, 55);
}

opencode_bypass_action_result opencode_bypass_action_hop() {
	opencode_bypass_action_result r;
	if (!try_action_lock()) {
		r.message = xorstr("Another rotation or setup action is already running.");
		return r;
	}
	auto warp = ouc_find_warp_cli();
	if (!warp) {
		action_unlock();
		r.message = xorstr("warp-cli.exe not found.");
		return r;
	}
	std::string old_v6;
	{
		std::lock_guard<std::mutex> lk(g_egress_mu);
		old_v6 = g_cached_egress.empty() ? ouc_get_warp_local_v6().value_or("") : g_cached_egress;
	}
	ouc_keeper_pause_begin(xorstr("bypass-ui-hop"), 180);
	auto new_v6 = ouc_hop_once(*warp, 5);
	ouc_keeper_pause_end();
	if (new_v6.empty()) {
		action_unlock();
		r.message = xorstr("Rotation failed — keeper will retry automatically.");
		return r;
	}
	{
		std::lock_guard<std::mutex> lk(g_egress_mu);
		g_cached_egress = new_v6;
		g_last_egress_ts = now_sec();
	}
	ouc_ensure_dirs();
	if (lane_edge_override_lower() == xorstr("auto"))
		ouc_write_file_text(g_lane_dir / xorstr("edge_reroll"), std::to_string((long long)time(nullptr)));
	auto gst = read_guard_json();
	gst[xorstr("hops")] = gst.value(xorstr("hops"), 0) + 1;
	gst[xorstr("last_hop")] = (int64_t)time(nullptr);
	gst[xorstr("current")] = new_v6;
	write_guard_json(gst);
	std::string old_p = old_v6.empty() ? "?" : ouc_normalize_v6_64(old_v6);
	std::string new_p = ouc_normalize_v6_64(new_v6);
	if (new_p.empty())
		new_p = new_v6;
	r.ok = true;
	r.message = std::string(xorstr("Rotated /64: ")) + old_p + xorstr(" -> ") + new_p;
	action_unlock();
	return r;
}

opencode_bypass_action_result opencode_bypass_action_warp_connect() {
	opencode_bypass_action_result r;
	auto warp = ouc_find_warp_cli();
	if (!warp) {
		r.message = xorstr("warp-cli.exe not found.");
		return r;
	}
	ouc_run_warp({xorstr("registration"), xorstr("new")}, &*warp);
	ouc_run_warp({xorstr("mode"), xorstr("warp")}, &*warp);
	auto run = ouc_run_warp({xorstr("connect")}, &*warp);
	for (int i = 0; i < 10; ++i) {
		opencode_bypass_sleep_ms(1000);
		if (ouc_get_warp_local_v6())
			break;
	}
	refresh_egress_sync(true);
	if (ouc_get_warp_local_v6()) {
		r.ok = true;
		r.message = xorstr("WARP connected in TUN mode.");
	} else {
		r.ok = run.code == 0;
		r.message = run.output.empty() ? xorstr("WARP connect triggered; waiting for IPv6.") : run.output;
	}
	return r;
}

opencode_bypass_action_result opencode_bypass_action_warp_disconnect() {
	opencode_bypass_action_result r;
	auto warp = ouc_find_warp_cli();
	if (!warp) {
		r.message = xorstr("warp-cli.exe not found.");
		return r;
	}
	auto run = ouc_run_warp({xorstr("disconnect")}, &*warp);
	{
		std::lock_guard<std::mutex> lk(g_egress_mu);
		g_cached_egress.clear();
	}
	r.ok = run.code == 0;
	r.message = r.ok ? xorstr("WARP disconnected.") : run.output;
	return r;
}

opencode_bypass_action_result opencode_bypass_action_toggle_worker(const char *name, bool enable) {
	opencode_bypass_action_result r;
	if (_stricmp(name, xorstr("keeper")) != 0 && _stricmp(name, xorstr("guard")) != 0) {
		r.message = xorstr("Unknown worker.");
		return r;
	}
	const char *task = _stricmp(name, xorstr("keeper")) == 0 ? k_task_keeper : k_task_guard;
	if (!enable) {
		int killed = opencode_bypass_kill_worker_processes(name);
		r.ok = true;
		r.message = std::string(xorstr("Stopped ")) + name + xorstr(" (") + std::to_string(killed) + xorstr(").");
		return r;
	}
	opencode_bypass_deploy_shims();
	if (opencode_bypass_task_exists(task)) {
		const std::vector<std::string> run_task = {"schtasks", "/Run", "/TN", task};
		auto res = ouc_run_hidden(run_task, 8.0);
		if (res.exit_code == 0) {
			opencode_bypass_sleep_ms(1000);
			r.ok = true;
			r.message = std::string(xorstr("Started ")) + name + xorstr(" via Scheduled Task.");
			return r;
		}
	}
	if (opencode_bypass_spawn_worker(name)) {
		opencode_bypass_sleep_ms(1000);
		r.ok = true;
		r.message = std::string(xorstr("Started bundled ")) + name + xorstr(" daemon.");
		return r;
	}
	r.message = std::string(xorstr("Could not launch ")) + name + ".";
	return r;
}

opencode_bypass_action_result opencode_bypass_action_toggle_desktop_service_proxy(bool enable) {
	opencode_bypass_action_result r;
	auto oc = ouc_find_opencode_cli();
	if (!oc) {
		r.message = xorstr("opencode-cli.exe not found.");
		return r;
	}
	std::string proxy_url = std::string(xorstr("http://127.0.0.1:")) + std::to_string(g_service_proxy_port);
	if (enable) {
		if (!opencode_bypass_port_open(g_service_proxy_port)) {
			opencode_bypass_action_toggle_worker(xorstr("keeper"), true);
			for (int i = 0; i < 10; ++i) {
				if (opencode_bypass_port_open(g_service_proxy_port))
					break;
				opencode_bypass_sleep_ms(400);
			}
		}
		ouc_run_hidden({*oc, xorstr("service"), xorstr("set"), xorstr("env"), xorstr("HTTPS_PROXY"), proxy_url});
		ouc_run_hidden({*oc, xorstr("service"), xorstr("set"), xorstr("env"), xorstr("HTTP_PROXY"), proxy_url});
		ouc_run_hidden({*oc, xorstr("service"), xorstr("set"), xorstr("env"), xorstr("https_proxy"), proxy_url});
		ouc_run_hidden({*oc, xorstr("service"), xorstr("set"), xorstr("env"), xorstr("http_proxy"), proxy_url});
		ouc_run_hidden(
			{*oc, xorstr("service"), xorstr("set"), xorstr("env"), xorstr("NO_PROXY"), xorstr("127.0.0.1,localhost,::1")});
		ouc_run_hidden(
			{*oc, xorstr("service"), xorstr("set"), xorstr("env"), xorstr("no_proxy"), xorstr("127.0.0.1,localhost,::1")});
	} else {
		for (const char *k : k_service_env_keys)
			ouc_run_hidden({*oc, xorstr("service"), xorstr("unset"), xorstr("env"), k});
	}
	auto st = ouc_run_hidden({*oc, xorstr("service"), xorstr("status")}, 6.0);
	if (st.output.find(xorstr("http://127.0.0.1")) != std::string::npos)
		opencode_bypass_launch_detached({*oc, xorstr("service"), xorstr("restart")});
	r.ok = true;
	r.message = enable ? xorstr("OpenCode Desktop service wired to local proxy (restarted if needed).")
			   : xorstr("OpenCode Desktop service restored to direct egress.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_toggle_autostart(bool enable) {
	opencode_bypass_action_result r;
	std::string detail;
	r.ok = opencode_bypass_set_autostart(enable, &detail);
	r.message = detail;
	return r;
}

opencode_bypass_action_result opencode_bypass_action_toggle_firewall(bool enable) {
	opencode_bypass_action_result r;
	if (ouc_is_admin()) {
		auto pr = enable ? ouc_firewall_install_killswitch() : ouc_firewall_remove_killswitch();
		r.ok = pr.first;
		r.message = pr.second;
		return r;
	}
	std::string exe = opencode_bypass_self_exe();
	std::string params = enable ? xorstr("--firewall install") : xorstr("--firewall remove");
	std::wstring wexe(exe.begin(), exe.end());
	std::wstring wparams(params.begin(), params.end());
	auto rc = (INT_PTR)LI(ShellExecuteW)(nullptr, L"runas", wexe.c_str(), wparams.c_str(), nullptr, SW_HIDE);
	if (rc <= 32) {
		r.message = xorstr("UAC prompt was declined or failed.");
		return r;
	}
	for (int i = 0; i < 12; ++i) {
		opencode_bypass_sleep_ms(350);
		if (ouc_firewall_killswitch_installed() == enable)
			break;
	}
	r.ok = ouc_firewall_killswitch_installed() == enable;
	r.message = r.ok ? (enable ? xorstr("Firewall kill-switch installed.") : xorstr("Firewall kill-switch removed."))
		       : xorstr("Firewall update sent; refresh status in a moment.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_toggle_strict_mode(bool enable) {
	opencode_bypass_action_result r;
	ouc_ensure_dirs();
	fs::path flag = g_lane_dir / xorstr("strict_proxy.flag");
	if (enable) {
		ouc_write_file_text(flag, xorstr("1\n"));
		r.message = xorstr("Strict WARP-only proxy mode enabled.");
	} else {
		std::error_code ec;
		fs::remove(flag, ec);
		r.message = xorstr("Strict WARP-only mode disabled.");
	}
	r.ok = true;
	return r;
}

opencode_bypass_action_result opencode_bypass_action_reroll_edge() {
	opencode_bypass_action_result r;
	ouc_ensure_dirs();
	ouc_write_file_text(g_lane_dir / xorstr("edge_override.txt"), xorstr("auto\n"));
	ouc_write_file_text(g_lane_dir / xorstr("edge_reroll"), std::to_string((long long)time(nullptr)));
	opencode_bypass_sleep_ms(1700);
	std::string edge;
	if (fs::is_regular_file(g_lane_dir / xorstr("last_edge.txt")))
		edge = trim_copy(ouc_read_file_text(g_lane_dir / xorstr("last_edge.txt")));
	r.ok = true;
	r.message = std::string(xorstr("Re-rolled Cloudflare edge: ")) + (edge.empty() ? xorstr("pending next request") : edge);
	return r;
}

opencode_bypass_action_result opencode_bypass_action_set_edge_override(const char *edge) {
	opencode_bypass_action_result r;
	ouc_ensure_dirs();
	std::string clean = edge && edge[0] ? trim_copy(edge) : xorstr("auto");
	if (clean.empty())
		clean = xorstr("auto");
	ouc_write_file_text(g_lane_dir / xorstr("edge_override.txt"), clean + "\n");
	opencode_bypass_sleep_ms(1600);
	r.ok = true;
	r.message = std::string(xorstr("Edge pinning set to: ")) + clean;
	return r;
}

opencode_bypass_action_result opencode_bypass_action_refresh_edge_pool() {
	opencode_bypass_action_result r;
	auto pool = ouc_fetch_opencode_aaaa_pool();
	{
		std::lock_guard<std::mutex> lk(g_pool_mu);
		g_edge_pool = pool;
	}
	r.ok = true;
	r.pool = pool;
	r.message = std::string(xorstr("Found ")) + std::to_string(pool.size()) + xorstr(" live opencode.ai IPv6 edges.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_clear_blacklist() {
	opencode_bypass_action_result r;
	if (!fs::is_regular_file(g_guard_state_file)) {
		r.ok = true;
		r.message = xorstr("Blacklist already empty.");
		return r;
	}
	auto st = read_guard_json();
	st[xorstr("blacklist")] = json::array();
	write_guard_json(st);
	r.ok = true;
	r.message = xorstr("Cleared /64 blacklist.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_launch_desktop() {
	opencode_bypass_action_result r;
	auto desktop = ouc_find_opencode_desktop();
	if (!desktop) {
		r.message = xorstr("OpenCode.exe Desktop app not found.");
		return r;
	}
	if (!opencode_bypass_port_open(g_service_proxy_port))
		opencode_bypass_action_toggle_worker(xorstr("keeper"), true);
	if (!opencode_bypass_desktop_service_proxy_wired())
		opencode_bypass_action_toggle_desktop_service_proxy(true);
	std::string proxy_url = std::string(xorstr("http://127.0.0.1:")) + std::to_string(g_service_proxy_port);
	std::wstring wdesk(desktop->begin(), desktop->end());
	std::wstring warg = L"--proxy-server=" + std::wstring(proxy_url.begin(), proxy_url.end());
	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	sei.lpFile = wdesk.c_str();
	sei.lpParameters = warg.c_str();
	sei.nShow = SW_SHOWNORMAL;
	if (!LI(ShellExecuteExW)(&sei)) {
		r.message = xorstr("Failed to launch Desktop.");
		return r;
	}
	if (sei.hProcess)
		LI(CloseHandle)(sei.hProcess);
	r.ok = true;
	r.message = xorstr("Launched OpenCode Desktop through WARP IPv6 lane.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_launch_cli() {
	opencode_bypass_action_result r;
	opencode_bypass_deploy_shims();
	fs::path shim = g_bin_dir / xorstr("opencode.cmd");
	if (!fs::is_regular_file(shim)) {
		r.message = xorstr("opencode CLI shim not ready; run Install / Repair first.");
		return r;
	}
	char wt_buf[MAX_PATH] = {};
	DWORD wt_len = LI(SearchPathA)(nullptr, xorstr("wt.exe"), nullptr, MAX_PATH, wt_buf, nullptr);
	if (wt_len > 0 && wt_len < MAX_PATH) {
		opencode_bypass_launch_detached({wt_buf, xorstr("cmd.exe"), xorstr("/k"), shim.string()});
	} else {
		opencode_bypass_launch_detached(
			{xorstr("cmd.exe"), xorstr("/c"), xorstr("start"), xorstr("OpenCode Bypass CLI"), xorstr("cmd.exe"),
				xorstr("/k"), shim.string()});
	}
	r.ok = true;
	r.message = xorstr("Opened OpenCode CLI terminal via Zen Lane.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_install_or_repair() {
	opencode_bypass_action_result r;
	if (!try_action_lock()) {
		r.message = xorstr("Another setup action is in progress.");
		return r;
	}
	if (!ouc_find_warp_cli()) {
		action_unlock();
		r.message = xorstr("Cloudflare WARP (warp-cli.exe) not found.");
		return r;
	}
	if (!ouc_find_opencode_cli()) {
		action_unlock();
		r.message = xorstr("OpenCode CLI not found.");
		return r;
	}
	opencode_bypass_deploy_shims();
	opencode_bypass_user_path_set(true);
	opencode_bypass_action_toggle_autostart(true);
	opencode_bypass_action_toggle_worker(xorstr("keeper"), true);
	opencode_bypass_action_toggle_worker(xorstr("guard"), true);
	opencode_bypass_action_warp_connect();
	opencode_bypass_action_toggle_desktop_service_proxy(true);
	refresh_egress_sync(true);
	r.ok = true;
	r.message = xorstr("Installed/repaired shims, PATH, tasks, WARP TUN, and Desktop service.");
	action_unlock();
	return r;
}

opencode_bypass_action_result opencode_bypass_action_uninstall_all() {
	opencode_bypass_action_result r;
	if (!try_action_lock()) {
		r.message = xorstr("Another setup action is in progress.");
		return r;
	}
	opencode_bypass_action_toggle_autostart(false);
	opencode_bypass_action_toggle_worker(xorstr("guard"), false);
	opencode_bypass_action_toggle_worker(xorstr("keeper"), false);
	opencode_bypass_action_toggle_desktop_service_proxy(false);
	if (ouc_firewall_killswitch_installed())
		opencode_bypass_action_toggle_firewall(false);
	opencode_bypass_user_path_set(false);
	if (auto warp = ouc_find_warp_cli())
		ouc_run_warp({xorstr("disconnect")}, &*warp);
	std::error_code ec;
	fs::remove_all(g_dest_dir, ec);
	{
		std::lock_guard<std::mutex> lk(g_egress_mu);
		g_cached_egress.clear();
	}
	r.ok = true;
	r.message = xorstr("Uninstalled background tasks, shims, proxy overrides, and state.");
	action_unlock();
	return r;
}

opencode_bypass_action_result opencode_bypass_action_open_logs_folder() {
	opencode_bypass_action_result r;
	ouc_ensure_dirs();
	r.ok = opencode_bypass_open_path(g_dest_dir);
	r.message = r.ok ? std::string(xorstr("Opened ")) + g_dest_dir.string() : xorstr("Could not open logs folder.");
	return r;
}

opencode_bypass_action_result opencode_bypass_action_copy_text(const char *text) {
	opencode_bypass_action_result r;
	if (!text || !text[0]) {
		r.message = xorstr("Nothing to copy.");
		return r;
	}
	r.ok = opencode_bypass_copy_clipboard(text);
	r.message = r.ok ? std::string(xorstr("Copied: ")) + text : xorstr("Clipboard copy failed.");
	return r;
}
