// SPDX-License-Identifier: GPL-3.0-or-later
//
// Explains how to reset a lost root password.
//
//   macha-recover [endpoint]
//
// Cluster recovery keys are not issued in this deployment model, so this tool
// has nothing to present and no route to present it to. It exists so that an
// operator who reaches for it in an emergency -- because older notes and the
// shipped config example name it -- is told what to do instead of being handed
// "command not found".
//
// The machinery for recovery keys is still in src/users.{hpp,cpp} and still
// tested; nothing calls it. See create_genesis_root() for why.
#include <iostream>

int main(int argc, char** argv) {
    // Recovery keys are not issued in this deployment model, so there is
    // nothing for this tool to present. It stays, and exits successfully,
    // because it is named in older notes and in the shipped config example:
    // an operator reaching for it in an emergency must be told what to do
    // instead, not handed "command not found" and left guessing.
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
