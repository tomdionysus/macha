// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "local_store.hpp"
#include "metadata.hpp"

#include <memory>
#include <mutex>

namespace macha {

// A node-local, persistent read cache. Cached objects are deliberately outside
// DHT ownership and replica accounting; losing the cache never reduces cluster
// durability. The metadata snapshot is retained separately and is not charged
// against max_blocks.
class PersistentBlockCache {
    mutable std::mutex mutex_;
    CacheConfig config_;
    std::array<uint8_t, 32> key_{};
    std::unique_ptr<LocalStore> store_;

    void open_locked();
    void trim_locked(const ObjectId* incoming = nullptr);
    std::filesystem::path metadata_path_locked() const;

  public:
    PersistentBlockCache(CacheConfig, std::array<uint8_t, 32>);
    void reconfigure(CacheConfig);
    bool enabled() const;
    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<Bytes> get(const ObjectId&);
    bool has(const ObjectId&) const;
    bool remove(const ObjectId&);
    size_t blocks() const;
    void remember_metadata(const MetadataRecord&);
    std::optional<MetadataRecord> metadata() const;
};
} // namespace macha
