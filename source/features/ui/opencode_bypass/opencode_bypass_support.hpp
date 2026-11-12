#ifndef OPENCODE_BYPASS_SUPPORT_HPP
#define OPENCODE_BYPASS_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct opencode_bypass_db_stats {
	int projects = 0;
	int sessions = 0;
	int messages = 0;
};

std::string opencode_bypass_self_exe();
bool opencode_bypass_port_open(int port);
bool opencode_bypass_task_exists(const char *task_name);
bool opencode_bypass_user_path_has_bin();
bool opencode_bypass_user_path_set(bool add);
void opencode_bypass_deploy_shims();
std::vector<std::string> opencode_bypass_tail_lines(const std::filesystem::path &path, int count);
opencode_bypass_db_stats opencode_bypass_read_db_stats();
int opencode_bypass_kill_worker_processes(const char *worker_name);
bool opencode_bypass_spawn_worker(const char *worker_name);
bool opencode_bypass_desktop_service_proxy_wired();
bool opencode_bypass_scripts_installed();
bool opencode_bypass_set_autostart(bool enable, std::string *detail);
void opencode_bypass_sleep_ms(int ms);
bool opencode_bypass_copy_clipboard(const std::string &text);
bool opencode_bypass_open_path(const std::filesystem::path &path);
bool opencode_bypass_launch_detached(const std::vector<std::string> &args);

#endif
