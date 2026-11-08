#ifndef OUC_COMMON_HPP
#define OUC_COMMON_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

constexpr const char *k_brand = "opencode-unlimited";
constexpr const char *k_task_keeper = "OpencodeUnlimited-Keeper";
constexpr const char *k_task_guard = "OpencodeUnlimited-Guard";
constexpr const char *k_fw_rule_prefix = "OUC_LEAKGUARD_";

extern std::filesystem::path g_dest_dir;
extern std::filesystem::path g_scripts_dir;
extern std::filesystem::path g_bin_dir;
extern std::filesystem::path g_guard_dir;
extern std::filesystem::path g_keeper_dir;
extern std::filesystem::path g_lane_dir;
extern std::filesystem::path g_pause_file;
extern std::filesystem::path g_guard_state_file;
extern std::filesystem::path g_guard_log_file;
extern std::filesystem::path g_keeper_log_file;
extern std::filesystem::path g_opencode_log_file;
extern int g_service_proxy_port;

void ouc_init_paths();
void ouc_ensure_dirs();

struct ouc_proc_result {
	int exit_code = -1;
	std::string output;
};

ouc_proc_result ouc_run_hidden(const std::vector<std::string> &args, double timeout_sec = 15.0);

std::optional<std::string> ouc_find_warp_cli();
std::optional<std::string> ouc_find_opencode_cli();
std::optional<std::string> ouc_find_opencode_desktop();

struct ouc_warp_run {
	int code = 1;
	std::string output;
};

ouc_warp_run ouc_run_warp(const std::vector<std::string> &args, const std::string *warp_bin = nullptr, double timeout_sec = 15.0);
std::string ouc_warp_mode(const std::string *warp_bin = nullptr);
bool ouc_warp_status(const std::string *warp_bin, bool *connected, std::string *mode, std::string *raw);

bool ouc_is_cloudflare_v6(const std::string &addr);
std::optional<std::string> ouc_get_warp_local_v6();
std::string ouc_get_egress_v6(double timeout_sec = 8.0);
std::string ouc_normalize_v6_64(const std::string &addr);

bool ouc_is_keeper_paused();
void ouc_keeper_pause_begin(const std::string &reason, int seconds = 180);
void ouc_keeper_pause_end();

void *ouc_acquire_mutex(const char *name);
bool ouc_is_mutex_held(const char *name);
bool ouc_is_admin();

void ouc_append_log(const std::filesystem::path &path, const std::string &message, std::size_t max_bytes = 1048576);

std::vector<std::string> ouc_fetch_opencode_aaaa_pool();
std::string ouc_hop_once(const std::string &warp_bin, int max_reg_tries = 3);

std::string ouc_exe_dir();
std::string ouc_read_file_text(const std::filesystem::path &path);
bool ouc_write_file_text(const std::filesystem::path &path, const std::string &text);

#endif
