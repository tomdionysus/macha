// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline administration of a node's replica of the cluster user table. Its
// only required job is bootstrap: a cluster with no accounts has no way to
// create the first one over an API that requires an admin to call it.
//
//   macha-users <state_path> <cluster.key> list
//   macha-users <state_path> <cluster.key> create <username> [--roles a,b]
//   macha-users <state_path> <cluster.key> passwd <username>
//   macha-users <state_path> <cluster.key> delete <username>
//
// The password is read from the terminal with echo off, never from argv --
// argv is visible to every process on the box and lands in shell history.
//
// The node must be stopped. A running daemon holds the whole table in memory
// and rewrites this file on its next mutation, so an edit made underneath it
// would be silently discarded; worse, it would then be replicated away by the
// daemon's own copy. Create the first admin during a restart window, on one
// node: it replicates to the others when the node starts.
//
// Rejected alternative: letting the first POST /api/v1/users through
// unauthenticated while the table is empty. Convenient, and a race with anyone
// who can reach the port in the minutes after an upgrade.
#include "codec.hpp"
#include "crypto.hpp"
#include "durable_file.hpp"
#include "users.hpp"

#include <termios.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace macha;

namespace {
constexpr std::array<uint8_t, 8> magic{'M', 'A', 'C', 'H', 'U', 'S', 'R', '1'};

std::array<uint8_t, 32> seal_key(const ClusterKeys& keys) {
    static constexpr std::string_view label = "macha/users/v1";
    return hkdf_sha256(keys.master, {},
                       std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(label.data()),
                                                label.size()));
}

std::vector<UserRecord> load(const std::filesystem::path& path,
                             const std::array<uint8_t, 32>& key) {
    if (!std::filesystem::exists(path))
        return {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open " + path.string());
    Bytes sealed((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Reader reader(sealed);
    const auto nonce = reader.fixed<12>();
    const auto tag = reader.fixed<16>();
    const auto ciphertext = reader.bytes();
    reader.finish();
    return decode_users(aes_gcm_open(key, nonce, tag, ciphertext, magic));
}

std::string read_password(const char* prompt) {
    std::cerr << prompt << std::flush;
    termios original{};
    const bool is_tty = tcgetattr(STDIN_FILENO, &original) == 0;
    if (is_tty) {
        termios quiet = original;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    std::string password;
    std::getline(std::cin, password);
    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << "\n";
    }
    return password;
}

std::string prompt_new_password() {
    const auto password = read_password("New password: ");
    if (password.empty())
        throw std::runtime_error("password must not be empty");
    if (read_password("Repeat password: ") != password)
        throw std::runtime_error("passwords do not match");
    return password;
}

std::vector<std::string> split_roles(std::string_view value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        auto role = std::string(value.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!role.empty()) {
            if (!known_role(role))
                throw std::runtime_error("unknown role '" + role + "'");
            out.push_back(std::move(role));
        }
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return out;
}

int usage() {
    std::cerr << "usage: macha-users <state_path> <cluster.key> <command> [args]\n"
                 "  list\n"
                 "  create <username> [--roles manage_users,manager,importer,media_viewer]\n"
                 "  passwd <username>\n"
                 "  delete <username>\n"
                 "  init                 create root and anonymous on a cluster that\n"
                 "                       has no accounts yet (e.g. after upgrading)\n\n"
                 "The node must be stopped: a running daemon rewrites this file\n"
                 "from its own in-memory copy and would discard the change.\n";
    return 2;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4)
        return usage();
    try {
        const std::filesystem::path state_path = argv[1];
        const auto keys = load_cluster_keys(argv[2]);
        const std::string command = argv[3];
        const auto key = seal_key(keys);
        const auto path = state_path / "users" / "users.bin";

        auto users = load(path, key);
        auto live = [&](std::string_view username) {
            const auto normalized = normalize_username(username);
            for (auto& user : users)
                if (!user.tombstone && user.username == normalized)
                    return &user;
            return static_cast<UserRecord*>(nullptr);
        };

        if (command == "init") {
            // The same call a node founding a new cluster makes, so an
            // upgraded cluster ends up with exactly the accounts a fresh one
            // would have rather than an approximation assembled by hand.
            // Refuses if the table holds anything at all, tombstones included.
            UserStore store_view(4096, path, key);
            auto created = create_initial_accounts(store_view, keys, state_path, NodeId{});
            if (!created)
                throw std::runtime_error(
                    "this node already has accounts; use `create` to add one, or `passwd` "
                    "to reset a password");
            std::cout << "Created the " << created->root.username << " and "
                      << created->anonymous.username << " accounts.\n\n"
                      << "  username: " << created->root.username << "\n"
                      << "  password: " << created->password << "\n\n"
                      << "Also written to " << created->path.string() << " (mode 0600).\n"
                      << "Start this node and the accounts replicate to the rest of the\n"
                      << "cluster. Sign in, change the password, and delete that file.\n";
            return 0;
        }

        if (command == "list") {
            for (const auto& user : users) {
                if (user.tombstone)
                    continue;
                std::cout << user.username << "  " << user.id << "  roles=";
                for (size_t i = 0; i < user.roles.size(); ++i)
                    std::cout << (i ? "," : "") << user.roles[i];
                std::cout << "  generation=" << user.credential_generation << "\n";
            }
            return 0;
        }

        if (argc < 5)
            return usage();
        const std::string username = argv[4];

        if (command == "create") {
            auto roles = all_roles();
            for (int i = 5; i + 1 < argc; i += 2)
                if (std::string(argv[i]) == "--roles")
                    roles = split_roles(argv[i + 1]);
            if (live(username))
                throw std::runtime_error("user already exists");

            // Built through UserStore so the record is produced by exactly the
            // code the daemon uses -- KDF parameters, role expansion and the
            // sealed file layout cannot drift between the two.
            UserStore store_view(4096, path, key);
            auto created = store_view.create(username, prompt_new_password(), roles, NodeId{});
            if (!created)
                throw std::runtime_error("could not create user");
            std::cout << "created " << created->username << " (" << created->id << ")\n";
            return 0;
        }

        if (command == "passwd") {
            auto* user = live(username);
            if (!user)
                throw std::runtime_error("no such user");
            // Same rule as the API and the store: anonymous has no password.
            // Said here too so the operator gets the reason rather than a bare
            // refusal from two layers down.
            if (user->username == anonymous_username)
                throw std::runtime_error(
                    "the 'anonymous' account has no password and cannot be given one; "
                    "what an unauthenticated visitor may do is its roles, and whether "
                    "one may connect at all is session.allow_anonymous");
            UserStore store_view(4096, path, key);
            if (!store_view.update(user->id, prompt_new_password(), std::nullopt, NodeId{}))
                throw std::runtime_error("could not change password");
            std::cout << "password changed; existing sessions for " << user->username
                      << " are now invalid\n";
            return 0;
        }

        if (command == "delete") {
            auto* user = live(username);
            if (!user)
                throw std::runtime_error("no such user");
            UserStore store_view(4096, path, key);
            if (!store_view.remove(user->id, NodeId{}))
                throw std::runtime_error("could not delete user");
            std::cout << "deleted " << username << "\n";
            return 0;
        }

        return usage();
    } catch (const std::exception& error) {
        std::cerr << "macha-users: " << error.what() << "\n";
        return 1;
    }
}
