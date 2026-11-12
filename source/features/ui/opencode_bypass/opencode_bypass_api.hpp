#ifndef OPENCODE_BYPASS_API_HPP
#define OPENCODE_BYPASS_API_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "features/ui/opencode_bypass/opencode_bypass_support.hpp"

struct opencode_bypass_snapshot {
	bool warp_installed = false;
	std::string warp_bin;
	bool warp_connected = false;
	std::string warp_mode;
	std::string warp_status_text;
	std::string adapter_v6;
	std::string egress_v6;
	std::string egress_prefix;
	bool is_cf = false;
	bool checking_egress = false;
	bool rotating = false;

	bool keeper_running = false;
	bool guard_running = false;
	bool keeper_task = false;
	bool guard_task = false;
	bool proxy_listening = false;
	int proxy_port = 0;
	bool firewall_installed = false;
	bool is_admin = false;

	std::string pinned_edge;
	std::string edge_override;
	std::vector<std::string> edge_pool;
	bool strict_mode = false;
	std::string cli_path;
	std::string desktop_path;
	bool path_wired = false;
	bool desktop_service_wired = false;
	bool scripts_installed = false;

	int hops = 0;
	int blacklist_count = 0;
	std::vector<std::string> blacklist;
	int64_t last_hop = 0;
	opencode_bypass_db_stats db;
	int64_t log_size_bytes = 0;

	std::vector<std::string> log_guard;
	std::vector<std::string> log_keeper;
	std::vector<std::string> log_opencode;
};

void opencode_bypass_api_boot();
void opencode_bypass_snapshot_refresh(opencode_bypass_snapshot *snap, bool force_egress = false);

struct opencode_bypass_action_result {
	bool ok = false;
	std::string message;
	std::vector<std::string> pool;
};

opencode_bypass_action_result opencode_bypass_action_hop();
opencode_bypass_action_result opencode_bypass_action_warp_connect();
opencode_bypass_action_result opencode_bypass_action_warp_disconnect();
opencode_bypass_action_result opencode_bypass_action_toggle_worker(const char *name, bool enable);
opencode_bypass_action_result opencode_bypass_action_toggle_desktop_service_proxy(bool enable);
opencode_bypass_action_result opencode_bypass_action_toggle_autostart(bool enable);
opencode_bypass_action_result opencode_bypass_action_toggle_firewall(bool enable);
opencode_bypass_action_result opencode_bypass_action_toggle_strict_mode(bool enable);
opencode_bypass_action_result opencode_bypass_action_reroll_edge();
opencode_bypass_action_result opencode_bypass_action_set_edge_override(const char *edge);
opencode_bypass_action_result opencode_bypass_action_refresh_edge_pool();
opencode_bypass_action_result opencode_bypass_action_clear_blacklist();
opencode_bypass_action_result opencode_bypass_action_launch_desktop();
opencode_bypass_action_result opencode_bypass_action_launch_cli();
opencode_bypass_action_result opencode_bypass_action_install_or_repair();
opencode_bypass_action_result opencode_bypass_action_uninstall_all();
opencode_bypass_action_result opencode_bypass_action_open_logs_folder();
opencode_bypass_action_result opencode_bypass_action_copy_text(const char *text);

#endif
