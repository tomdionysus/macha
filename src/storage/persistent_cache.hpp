// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "storage/local_store.hpp"
#include "metadata/metadata.hpp"

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include "contract/thread_safety.hpp"

namespace macha {

// A node-local, persistent read cache. Cached objects are deliberately outside
// DHT ownership and replica accounting; losing the cache never reduces cluster
// durability. The metadata snapshot is retained separately and is not charged
// against max_blocks.
class PersistentBlockCache {
    // Reads are on the playback path: never hold a cache-wide mutex while
    // LocalStore reads an object, or a hit waits behind insertion and eviction.
    // Held by reconfigure() across opening the cache store (LocalStore
    // construction and its directory walk) and destroying the old one.
    mutable IoMutex state_mutex_ MACHA_ACQUIRED_AFTER(writer_mutex_, metadata_mutex_);
    // Held across LocalStore puts and removes; serialises writers so two
    // cannot both take the last slot. Guards no state of its own.
    IoMutex writer_mutex_;
    // Held across the metadata file's write, read and logging.
    mutable IoMutex metadata_mutex_ MACHA_ACQUIRED_AFTER(writer_mutex_);
    mutable std::optional<Hash256> cached_metadata_hash_ MACHA_GUARDED_BY(metadata_mutex_);
    CacheConfig config_ MACHA_GUARDED_BY(state_mutex_);
    const std::array<uint8_t, 32> key_{};
    std::shared_ptr<LocalStore> store_ MACHA_GUARDED_BY(state_mutex_);
    std::list<ObjectId> lru_ MACHA_GUARDED_BY(state_mutex_);
    std::map<ObjectId, std::list<ObjectId>::iterator> lru_index_ MACHA_GUARDED_BY(state_mutex_);
    std::atomic_size_t block_count_{};
    // Since process start; monotonic, never reset, so consumers diff two reads.
    std::atomic_uint64_t hits_{};
    std::atomic_uint64_t misses_{};
    std::atomic_uint64_t evictions_{};

    void open_locked() MACHA_REQUIRES(state_mutex_);
    void rebuild_lru_locked() MACHA_REQUIRES(state_mutex_);
    void mark_used(const std::shared_ptr<LocalStore>&, const ObjectId&)
        MACHA_EXCLUDES(state_mutex_);
    void trim_to_limit(const std::shared_ptr<LocalStore>&, size_t) MACHA_EXCLUDES(state_mutex_);
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
