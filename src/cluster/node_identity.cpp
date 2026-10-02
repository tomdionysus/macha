// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/node_identity.hpp"

#include "durable_file.hpp"
#include "storage/local_store.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace macha {
namespace {

NodeId load_v18_node_id(const std::filesystem::path& state) {
    static constexpr std::string_view expected = "macha-state-layout-v18";
    const auto marker = state / "storage-layout";
    if (std::filesystem::exists(marker)) {
        std::ifstream input(marker);
        std::string value;
        std::getline(input, value);
        if (!input && value.empty())
            throw std::runtime_error("cannot read storage layout marker");
        if (value != expected)
            throw std::runtime_error(
                "incompatible Macha storage layout; 0.18 requires a fresh namespace");
    } else {
        // No migration path: an older namespace/backend layout is refused, not
        // reinterpreted. .macha.lock (from StorageLock) is the only entry a
        // fresh state directory may hold.
        for (const auto& entry : std::filesystem::directory_iterator(state)) {
            if (entry.path().filename() == ".macha.lock")
                continue;
            throw std::runtime_error(
                "existing unversioned Macha state detected; 0.18 requires a fresh namespace");
        }
        durable_replace_file(marker, std::string(expected) + "\n");
    }
    return load_or_create_node_id(state);
}

} // namespace

NodeIdentity::NodeIdentity(const std::filesystem::path& state_path, ClusterKeys cluster_keys)
    : keys(std::move(cluster_keys)), id(load_v18_node_id(state_path)),
      durability_epoch(random_node_id()) {}

} // namespace macha
