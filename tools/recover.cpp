// SPDX-License-Identifier: GPL-3.0-or-later
//
// Explains how to reset a lost root password.
//
//   macha-recover [endpoint]
//
// Recovery keys are not issued: the recovery-key code in src/users.{hpp,cpp}
// is unused (see create_genesis_root()).
#include <iostream>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::cout
        << "macha-recover: cluster recovery keys are not enabled.\n\n"
           "Nothing was issued to present, and no node accepts one -- there is no\n"
           "HTTP route for it, deliberately.\n\n"
           "To reset the root password, on any node, with that node stopped:\n\n"
           "  macha-users <state_path> <cluster.key> passwd root\n\n"
           "It replicates to the rest of the cluster when the node starts. An\n"
           "account holding manage_users can also do it through the API without\n"
           "stopping anything.\n\n"
           "Why there is no key: the only party who could present one is the\n"
           "operator, who already has root on a node -- where the command above\n"
           "does the same job without a secret that had to survive months in a\n"
           "drawer. A key would have meant a standing unauthenticated path to the\n"
           "most privileged account in the cluster, bought with a capability that\n"
           "already exists behind strictly more access.\n";
    return 0;
}
