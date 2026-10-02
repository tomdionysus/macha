// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "storage/local_store.hpp"
#include "metadata/metadata.hpp"

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>

namespace macha {

// A node-local, persistent read cache. Cached objects are deliberately outside
// DHT ownership and replica accounting; losing the cache never reduces cluster
// durability. The metadata snapshot is retained separately and is not charged
// against max_blocks.
class PersistentBlockCache {
    // Reads are on the playback path: never hold a cache-wide mutex while
    // LocalStore reads an object, or a hit waits behind insertion and eviction.
    mutable std::mutex state_mutex_;
    std::mutex writer_mutex_;
    mutable std::mutex metadata_mutex_;
    mutable std::optional<Hash256> cached_metadata_hash_;
    CacheConfig config_;
    std::array<uint8_t, 32> key_{};
    std::shared_ptr<LocalStore> store_;
    std::list<ObjectId> lru_;
    std::map<ObjectId, std::list<ObjectId>::iterator> lru_index_;
    std::atomic_size_t block_count_{};
    // Since process start; monotonic, never reset, so consumers diff two reads.
    std::atomic_uint64_t hits_{};
    std::atomic_uint64_t misses_{};
    std::atomic_uint64_t evictions_{};

    void open_locked();
    void rebuild_lru_locked();
    void mark_used(const std::shared_ptr<LocalStore>&, const ObjectId&);
    void trim_to_limit(const std::shared_ptr<LocalStore>&, size_t);
    static std::filesystem::path metadata_path(const CacheConfig&);

  public:
    PersistentBlockCache(CacheConfig, std::array<uint8_t, 32>);
    void reconfigure(CacheConfig);
    bool enabled() const;
    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<Bytes> get(const ObjectId&);
    bool has(const ObjectId&) const;
    bool remove(const ObjectId&);
    size_t blocks() const;
    // Cumulative since start; entries is instantaneous. Sustained zero hits
    // with high evictions is a defect.
    struct Stats {
        uint64_t hits{};
        uint64_t misses{};
        uint64_t evictions{};
        uint64_t entries{};
    };
    Stats stats() const;
    void remember_metadata(const MetadataRecord&);
    std::optional<MetadataRecord> metadata() const;
};
} // namespace macha
