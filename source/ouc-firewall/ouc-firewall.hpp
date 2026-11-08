#ifndef OUC_FIREWALL_HPP
#define OUC_FIREWALL_HPP

#include <string>
#include <utility>

extern const char *k_cli_rule_name;

bool ouc_firewall_killswitch_installed();
std::pair<bool, std::string> ouc_firewall_install_killswitch();
std::pair<bool, std::string> ouc_firewall_remove_killswitch();

int ouc_firewall_main(int argc, char **argv);

#endif
