// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "local_store.hpp"

#include <memory>
#include <mutex>
#include <set>

namespace macha {

class StoragePool {
    struct Backend;
    std::filesystem::path state_path_;
    NodeId node_id_;
    std::array<uint8_t, 32> key_{};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Backend>> backends_;
    size_t rebalance_offset_{};

    std::vector<std::shared_ptr<Backend>> snapshot() const;
    std::filesystem::path identity_path(const std::filesystem::path&) const;
    bool activate(const std::shared_ptr<Backend>&);
    void deactivate(const std::shared_ptr<Backend>&, const std::string&) const;
    std::vector<std::shared_ptr<Backend>> ranked(const ObjectId&) const;

  public:
    StoragePool(std::filesystem::path state_path, NodeId, std::vector<StorageBackendConfig>,
                std::array<uint8_t, 32> key);
    void reconfigure(const std::vector<StorageBackendConfig>&);
    void refresh();

    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<Bytes> get(const ObjectId&) const;
    bool has(const ObjectId&) const;
    bool remove(const ObjectId&);
    std::vector<ObjectId> list() const;
    bool older_than(const ObjectId&, std::chrono::seconds) const;

    // Moves authoritative local objects toward their deterministic backend as
    // disks are added, removed or returned. budget_bytes == 0 means unlimited.
    uint64_t rebalance_once(uint64_t budget_bytes = 0);

    uint64_t used() const;
    uint64_t limit() const;
    size_t online_backends() const;
};
} // namespace macha
