#include "ouc-firewall/ouc-firewall.hpp"

#include <cstring>

#include "ouc-common/ouc-common.hpp"
#include "vendor/xorstr/xorstr.hpp"

const char *k_cli_rule_name = "OUC_LEAKGUARD_CLI";

static const char *k_public_ranges =
	"0.0.0.0-9.255.255.255,"
	"11.0.0.0-126.255.255.255,"
	"128.0.0.0-172.15.255.255,"
	"172.32.0.0-192.167.255.255,"
	"192.169.0.0-255.255.255.255,"
	"2000::/3";

bool ouc_firewall_killswitch_installed() {
	auto res = ouc_run_hidden(
		{xorstr("netsh"), xorstr("advfirewall"), xorstr("firewall"), xorstr("show"), xorstr("rule"),
			std::string(xorstr("name=")) + k_cli_rule_name},
		6.0);
	return res.exit_code == 0 && res.output.find(k_cli_rule_name) != std::string::npos;
}

std::pair<bool, std::string> ouc_firewall_install_killswitch() {
	if (!ouc_is_admin())
		return {false, xorstr("administrator privileges required for firewall kill-switch")};

	auto cli = ouc_find_opencode_cli();
	if (!cli || cli->size() < 4 || _stricmp(cli->c_str() + cli->size() - 4, xorstr(".exe")) != 0)
		return {false, xorstr("opencode-cli.exe not found (skipping program firewall rule")};

	ouc_firewall_remove_killswitch();
	auto res = ouc_run_hidden(
		{xorstr("netsh"), xorstr("advfirewall"), xorstr("firewall"), xorstr("add"), xorstr("rule"),
			std::string(xorstr("name=")) + k_cli_rule_name, xorstr("dir=out"), xorstr("action=block"),
			std::string(xorstr("program=")) + *cli, std::string(xorstr("remoteip=")) + k_public_ranges, xorstr("profile=any"),
			xorstr("enable=yes"),
			xorstr("description=Opencode Unlimited: force opencode-cli through localhost zen-proxy/WARP")},
		10.0);
	if (res.exit_code == 0)
		return {true, std::string(xorstr("installed rule ")) + k_cli_rule_name + xorstr(" for ") + *cli};
	return {false, res.output.empty() ? xorstr("netsh failed") : res.output};
}

std::pair<bool, std::string> ouc_firewall_remove_killswitch() {
	if (!ouc_is_admin())
		return {false, xorstr("administrator privileges required to remove firewall rules")};
	ouc_run_hidden(
		{xorstr("netsh"), xorstr("advfirewall"), xorstr("firewall"), xorstr("delete"), xorstr("rule"),
			std::string(xorstr("name=")) + k_cli_rule_name},
		8.0);
	return {true, std::string(xorstr("removed rule ")) + k_cli_rule_name};
}

int ouc_firewall_main(int argc, char **argv) {
	const char *action = argc > 1 ? argv[1] : xorstr("status");
	if (_stricmp(action, xorstr("install")) == 0) {
		auto [ok, msg] = ouc_firewall_install_killswitch();
		printf("%s\n", msg.c_str());
		return ok ? 0 : 1;
	}
	if (_stricmp(action, xorstr("remove")) == 0) {
		auto [ok, msg] = ouc_firewall_remove_killswitch();
		printf("%s\n", msg.c_str());
		return ok ? 0 : 1;
	}
	if (_stricmp(action, xorstr("status")) == 0) {
		bool inst = ouc_firewall_killswitch_installed();
		printf("%s\n", inst ? xorstr("installed") : xorstr("not installed"));
		return inst ? 0 : 1;
	}
	fprintf(stderr, "%s\n", xorstr("Usage: opencode-unlimited.exe --firewall [install|remove|status]"));
	return 2;
}
