#include "features/ui/opencode_bypass/opencode_bypass_support.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <ShlObj.h>

#include "ouc-common/ouc-common.hpp"
#include "vendor/lazy-importer/li.hpp"
#include "vendor/nlohmann/json.hpp"
#include "vendor/xorstr/xorstr.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string opencode_bypass_self_exe() {
	wchar_t buf[MAX_PATH];
	DWORD n = LI(GetModuleFileNameW)(nullptr, buf, MAX_PATH);
	if (!n || n >= MAX_PATH)
		return {};
	int need = LI(WideCharToMultiByte)(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
	if (need <= 1)
		return {};
	std::string out((size_t)need - 1, '\0');
	LI(WideCharToMultiByte)(CP_UTF8, 0, buf, -1, out.data(), need, nullptr, nullptr);
	return out;
}

bool opencode_bypass_port_open(int port) {
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return false;
	sockaddr_in a {};
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	bool ok = connect(s, (sockaddr *)&a, sizeof(a)) == 0;
	closesocket(s);
	return ok;
}

bool opencode_bypass_task_exists(const char *task_name) {
	const std::vector<std::string> args = {"schtasks", "/Query", "/TN", task_name};
	auto r = ouc_run_hidden(args, 5.0);
	return r.exit_code == 0;
}

bool opencode_bypass_user_path_has_bin() {
	HKEY key = nullptr;
	if (LI(RegOpenKeyExW)(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) != ERROR_SUCCESS)
		return false;
	wchar_t buf[32768];
	DWORD sz = sizeof(buf);
	DWORD type = 0;
	bool ok = false;
	if (LI(RegQueryValueExW)(key, L"Path", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_EXPAND_SZ) {
		std::wstring wpath(buf);
		std::string target = g_bin_dir.string();
		for (auto &c : target)
			if (c == '/')
				c = '\\';
		while (!target.empty() && (target.back() == '\\' || target.back() == '/'))
			target.pop_back();
		std::wstring wtarget(target.begin(), target.end());
		std::transform(wtarget.begin(), wtarget.end(), wtarget.begin(), ::towlower);
		size_t pos = wpath.find(L';');
		if (pos != std::wstring::npos) {
			std::wstring first = wpath.substr(0, pos);
			std::transform(first.begin(), first.end(), first.begin(), ::towlower);
			while (!first.empty() && (first.back() == L'\\' || first.back() == L'/'))
				first.pop_back();
			ok = (first == wtarget);
		}
	}
	LI(RegCloseKey)(key);
	return ok;
}

bool opencode_bypass_user_path_set(bool add) {
	HKEY key = nullptr;
	if (LI(RegOpenKeyExW)(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
		return false;

	wchar_t buf[32768];
	DWORD sz = sizeof(buf);
	DWORD type = REG_EXPAND_SZ;
	std::wstring cur;
	if (LI(RegQueryValueExW)(key, L"Path", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
		cur.assign(buf);

	std::string target = g_bin_dir.string();
	for (auto &c : target)
		if (c == '/')
			c = '\\';
	while (!target.empty() && (target.back() == '\\' || target.back() == '/'))
		target.pop_back();
	std::wstring wtarget(target.begin(), target.end());
	std::wstring lower_target = wtarget;
	std::transform(lower_target.begin(), lower_target.end(), lower_target.begin(), ::towlower);

	std::vector<std::wstring> parts;
	std::wstringstream ss(cur);
	std::wstring item;
	while (std::getline(ss, item, L';')) {
		if (item.empty())
			continue;
		std::wstring t = item;
		while (!t.empty() && (t.back() == L'\\' || t.back() == L'/'))
			t.pop_back();
		std::wstring lt = t;
		std::transform(lt.begin(), lt.end(), lt.begin(), ::towlower);
		if (lt == lower_target)
			continue;
		parts.push_back(t);
	}

	std::wstring rebuilt;
	if (add) {
		rebuilt = wtarget;
		for (const auto &p : parts) {
			rebuilt += L";";
			rebuilt += p;
		}
	} else {
		for (size_t i = 0; i < parts.size(); ++i) {
			if (i)
				rebuilt += L";";
			rebuilt += parts[i];
		}
	}

	LI(RegSetValueExW)(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE *)rebuilt.c_str(),
		(DWORD)((rebuilt.size() + 1) * sizeof(wchar_t)));
	LI(RegCloseKey)(key);
	return true;
}

void opencode_bypass_deploy_shims() {
	ouc_ensure_dirs();
	const std::string exe = opencode_bypass_self_exe();
	if (exe.empty())
		return;

	auto write_shim = [](const fs::path &path, const std::string &body) { ouc_write_file_text(path, body); };

	const std::string qexe = "\"" + exe + "\"";
	write_shim(g_bin_dir / xorstr("opencode.cmd"), std::string(xorstr("@echo off\r\n")) + qexe + xorstr(" --lane %*\r\nexit /b %ERRORLEVEL%\r\n"));
	write_shim(g_bin_dir / xorstr("opencode-desktop.cmd"),
		std::string(xorstr("@echo off\r\nstart \"\" ")) + qexe + xorstr(" --lane --ouc-desktop %*\r\n"));
	write_shim(g_bin_dir / xorstr("opencode-bypass-hop.cmd"), std::string(xorstr("@echo off\r\n")) + qexe + xorstr(" --hop %*\r\nexit /b %ERRORLEVEL%\r\n"));
	write_shim(g_bin_dir / xorstr("opencode-bypass-doctor.cmd"),
		std::string(xorstr("@echo off\r\n")) + qexe + xorstr(" --doctor %*\r\nexit /b %ERRORLEVEL%\r\n"));
	write_shim(g_bin_dir / xorstr("opencode-bypass-keeper.cmd"),
		std::string(xorstr("@echo off\r\n")) + qexe + xorstr(" --worker keeper\r\nexit /b %ERRORLEVEL%\r\n"));
	write_shim(g_bin_dir / xorstr("opencode-bypass-guard.cmd"),
		std::string(xorstr("@echo off\r\n")) + qexe + xorstr(" --worker guard\r\nexit /b %ERRORLEVEL%\r\n"));
}

std::vector<std::string> opencode_bypass_tail_lines(const fs::path &path, int count) {
	std::vector<std::string> lines;
	if (!fs::is_regular_file(path))
		return lines;
	try {
		auto sz = fs::file_size(path);
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return lines;
		const auto read_bytes = (std::min)(sz, (uintmax_t)131072);
		if (sz > read_bytes)
			in.seekg((std::streamoff)(sz - read_bytes));
		std::string text((size_t)read_bytes, '\0');
		in.read(text.data(), (std::streamsize)read_bytes);
		text.resize((size_t)in.gcount());
		std::stringstream ss(text);
		std::string line;
		while (std::getline(ss, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			lines.push_back(std::move(line));
		}
		if ((int)lines.size() > count)
			lines.erase(lines.begin(), lines.end() - count);
	} catch (...) {
	}
	return lines;
}

opencode_bypass_db_stats opencode_bypass_read_db_stats() {
	opencode_bypass_db_stats s;
	char profile[MAX_PATH];
	if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, profile) != S_OK)
		return s;
	fs::path db = fs::path(profile) / fs::path(".local/share/opencode/opencode.db");
	if (!fs::is_regular_file(db))
		return s;
	auto ps = std::string(xorstr("-NoProfile -Command \"$db='")) + db.string() + xorstr("';")
		+ xorstr("Add-Type -AssemblyName System.Data;$c=New-Object System.Data.SQLite.SQLiteConnection")
		+ xorstr("('Data Source='+$db);") + xorstr("$c.Open();") + xorstr("$p=$c.ExecuteScalar('SELECT count(*) FROM project');")
		+ xorstr("$s=$c.ExecuteScalar('SELECT count(*) FROM session_v2');")
		+ xorstr("$m=$c.ExecuteScalar('SELECT count(*) FROM session_message');")
		+ xorstr("Write-Output ($p.ToString()+','+$s.ToString()+','+$m.ToString())\"");
	const std::vector<std::string> cmd = {"powershell.exe", ps};
	auto r = ouc_run_hidden(cmd, 8.0);
	if (r.exit_code != 0)
		return s;
	int p = 0, se = 0, m = 0;
	if (std::sscanf(r.output.c_str(), "%d,%d,%d", &p, &se, &m) == 3) {
		s.projects = p;
		s.sessions = se;
		s.messages = m;
	}
	return s;
}

int opencode_bypass_kill_worker_processes(const char *worker_name) {
	const std::string exe = opencode_bypass_self_exe();
	if (exe.empty())
		return 0;
	std::string esc = exe;
	for (size_t i = 0; i < esc.size(); ++i)
		if (esc[i] == '\\')
			esc.insert(i++, "\\");
	std::string flag = std::string(xorstr("--worker ")) + worker_name;
	auto ps =
		std::string(xorstr("-NoProfile -Command \"$exe='")) + esc + xorstr("';$killed=0;Get-CimInstance Win32_Process ")
		+ xorstr("| Where-Object { $_.ExecutablePath -and ($_.ExecutablePath -ieq $exe) -and ($_.CommandLine -match '")
		+ flag + xorstr("') } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue;")
		+ xorstr("$killed++ }; Write-Output $killed\"");
	const std::vector<std::string> kill_cmd = {"powershell.exe", ps};
	auto r = ouc_run_hidden(kill_cmd, 12.0);
	if (r.exit_code != 0)
		return 0;
	return std::atoi(r.output.c_str());
}

bool opencode_bypass_spawn_worker(const char *worker_name) {
	const std::string exe = opencode_bypass_self_exe();
	if (exe.empty())
		return false;
	std::wstring wexe(exe.begin(), exe.end());
	std::wstring args = L"--worker ";
	args += std::wstring(worker_name, worker_name + std::strlen(worker_name));

	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	sei.lpFile = wexe.c_str();
	sei.lpParameters = args.c_str();
	sei.nShow = SW_HIDE;
	if (!LI(ShellExecuteExW)(&sei))
		return false;
	if (sei.hProcess)
		LI(CloseHandle)(sei.hProcess);
	return true;
}

bool opencode_bypass_desktop_service_proxy_wired() {
	char profile[MAX_PATH];
	if (LI(SHGetFolderPathA)(nullptr, CSIDL_PROFILE, nullptr, 0, profile) != S_OK)
		return false;
	fs::path cfg = fs::path(profile) / xorstr(".config/opencode/service.json");
	if (!fs::is_regular_file(cfg))
		return false;
	try {
		auto text = ouc_read_file_text(cfg);
		auto j = json::parse(text);
		if (!j.contains(xorstr("env")) || !j[xorstr("env")].is_object())
			return false;
		std::string needle = std::string(xorstr("127.0.0.1:")) + std::to_string(g_service_proxy_port);
		auto &env = j[xorstr("env")];
		if (env.contains(xorstr("HTTPS_PROXY")))
			return env[xorstr("HTTPS_PROXY")].get<std::string>().find(needle) != std::string::npos;
	} catch (...) {
	}
	return false;
}

bool opencode_bypass_scripts_installed() {
	return fs::is_regular_file(g_bin_dir / xorstr("opencode.cmd"));
}

void opencode_bypass_sleep_ms(int ms) {
	if (ms <= 0)
		return;
	LI(Sleep)((DWORD)ms);
}

bool opencode_bypass_set_autostart(bool enable, std::string *detail) {
	if (!enable) {
		ouc_run_hidden(std::vector<std::string>{"schtasks", "/Delete", "/TN", k_task_keeper, "/F"}, 8.0);
		ouc_run_hidden(std::vector<std::string>{"schtasks", "/Delete", "/TN", k_task_guard, "/F"}, 8.0);
		if (detail)
			*detail = xorstr("Removed logon Scheduled Tasks.");
		return true;
	}

	opencode_bypass_deploy_shims();
	const std::string exe = opencode_bypass_self_exe();
	if (exe.empty()) {
		if (detail)
			*detail = xorstr("Could not resolve executable path.");
		return false;
	}

	std::string esc = exe;
	for (size_t i = 0; i < esc.size(); ++i)
		if (esc[i] == '\'')
			esc.insert(i++, "\'\'");

	auto reg_one = [&](const char *tn, const char *worker_flag) -> bool {
		std::string arg = std::string(xorstr("--worker ")) + worker_flag;
		auto ps = std::string(xorstr("-NoProfile -Command "))
			+ xorstr("$exe='") + esc + xorstr("';$arg='") + arg + xorstr("';")
			+ xorstr("$a=New-ScheduledTaskAction -Execute $exe -Argument $arg;")
			+ xorstr("$t=New-ScheduledTaskTrigger -AtLogOn -User \"$env:USERDOMAIN\\$env:USERNAME\";")
			+ xorstr("$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries ")
			+ xorstr("-StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -RestartCount 3 ")
			+ xorstr("-RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew;")
			+ xorstr("Unregister-ScheduledTask -TaskName '") + tn + xorstr("' -Confirm:$false -ErrorAction SilentlyContinue;")
			+ xorstr("Register-ScheduledTask -TaskName '") + tn + xorstr("' -Action $a -Trigger $t -Settings $s | Out-Null");
		const std::vector<std::string> reg_cmd = {"powershell.exe", ps};
		auto r = ouc_run_hidden(reg_cmd, 14.0);
		return r.exit_code == 0;
	};

	if (!reg_one(k_task_keeper, xorstr("keeper")) || !reg_one(k_task_guard, xorstr("guard"))) {
		if (detail)
			*detail = xorstr("Task registration failed.");
		return false;
	}
	if (detail)
		*detail = xorstr("Registered logon auto-start tasks for Keeper & Guard.");
	return true;
}

bool opencode_bypass_copy_clipboard(const std::string &text) {
	if (text.empty())
		return false;
	if (!LI(OpenClipboard)(nullptr))
		return false;
	LI(EmptyClipboard)();
	HGLOBAL h = LI(GlobalAlloc)(GMEM_MOVEABLE, text.size() + 1);
	if (!h) {
		LI(CloseClipboard)();
		return false;
	}
	void *p = LI(GlobalLock)(h);
	if (!p) {
		LI(GlobalFree)(h);
		LI(CloseClipboard)();
		return false;
	}
	memcpy(p, text.c_str(), text.size() + 1);
	LI(GlobalUnlock)(h);
	bool ok = LI(SetClipboardData)(CF_TEXT, h) != nullptr;
	LI(CloseClipboard)();
	return ok;
}

bool opencode_bypass_open_path(const fs::path &path) {
	std::wstring w(path.wstring());
	HINSTANCE rc = LI(ShellExecuteW)(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return (INT_PTR)rc > 32;
}

bool opencode_bypass_launch_detached(const std::vector<std::string> &args) {
	if (args.empty())
		return false;
	std::wstring cmd;
	for (size_t i = 0; i < args.size(); ++i) {
		if (i)
			cmd += L' ';
		std::wstring a(args[i].begin(), args[i].end());
		if (a.find(L' ') != std::wstring::npos)
			cmd += L'"' + a + L'"';
		else
			cmd += a;
	}
	STARTUPINFOW si = {sizeof(si)};
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');
	if (!LI(CreateProcessW)(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
			nullptr, &si, &pi))
		return false;
	LI(CloseHandle)(pi.hThread);
	LI(CloseHandle)(pi.hProcess);
	return true;
}
