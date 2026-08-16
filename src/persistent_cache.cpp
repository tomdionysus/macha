// SPDX-License-Identifier: GPL-3.0-or-later
#include "persistent_cache.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> cache_meta_magic{'D', 'H', 'T', 'C', 'M', 'E', 'T', '1'};

void write_all(int fd, std::span<const uint8_t> bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(strerror(errno));
        }
        offset += static_cast<size_t>(n);
    }
}

void atomic_write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temp = path.string() + ".tmp." + std::to_string(getpid()) + "." + std::to_string(unix_ms());
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create cache metadata: " + std::string(strerror(errno)));
    try {
        write_all(fd, bytes);
        if (::fsync(fd))
            throw std::runtime_error("cannot sync cache metadata: " + std::string(strerror(errno)));
        if (::close(fd))
            throw std::runtime_error("cannot close cache metadata: " + std::string(strerror(errno)));
        fd = -1;
        if (::rename(temp.c_str(), path.c_str()))
            throw std::runtime_error("cannot install cache metadata: " + std::string(strerror(errno)));
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw;
    }
}
} // namespace

PersistentBlockCache::PersistentBlockCache(CacheConfig config, std::array<uint8_t, 32> key)
    : config_(std::move(config)), key_(key) {
    std::lock_guard lock(state_mutex_);
    open_locked();
}

void PersistentBlockCache::open_locked() {
    store_.reset();
    lru_.clear();
    lru_index_.clear();
    if (config_.path.empty() || !config_.max_blocks)
        return;
    try {
        store_ = std::make_shared<LocalStore>(config_.path,
                                              std::numeric_limits<uint64_t>::max(), key_);
        rebuild_lru_locked();
    } catch (const std::exception& error) {
        store_.reset();
        lru_.clear();
        lru_index_.clear();
        Log::warn("persistent cache disabled: " + std::string(error.what()));
    }
}

void PersistentBlockCache::reconfigure(CacheConfig config) {
    std::lock_guard writer(writer_mutex_);

    std::shared_ptr<LocalStore> store;
    size_t limit = 0;
    {
        std::lock_guard lock(state_mutex_);
        bool reopen = config.path != config_.path;
        config_ = std::move(config);
        if (config_.path.empty() || !config_.max_blocks) {
            store_.reset();
            lru_.clear();
            lru_index_.clear();
            return;
        }
        if (reopen || !store_)
            open_locked();
        store = store_;
        limit = config_.max_blocks;
    }

    trim_to_limit(store, limit);
}

bool PersistentBlockCache::enabled() const {
    std::lock_guard lock(state_mutex_);
    return static_cast<bool>(store_) && config_.max_blocks;
}

void PersistentBlockCache::rebuild_lru_locked() {
    lru_.clear();
    lru_index_.clear();
    if (!store_)
        return;

    // Reconcile the on-disk cache exactly once when it is opened.  Runtime
    // eviction is maintained incrementally from here; it must never recurse
    // over the complete cache tree on every inserted extent.
    for (const auto& id : store_->list()) {
        lru_.push_back(id);
        lru_index_[id] = std::prev(lru_.end());
    }
}

void PersistentBlockCache::mark_used(const std::shared_ptr<LocalStore>& store,
                                     const ObjectId& id) {
    std::lock_guard lock(state_mutex_);
    if (!store_ || store_ != store)
        return;
    auto found = lru_index_.find(id);
    if (found == lru_index_.end())
        return; // It may have been evicted after this reader opened the object.
    lru_.splice(lru_.end(), lru_, found->second);
    found->second = std::prev(lru_.end());
}

void PersistentBlockCache::trim_to_limit(const std::shared_ptr<LocalStore>& store,
                                         size_t limit) {
    if (!store)
        return;

    while (true) {
        std::optional<ObjectId> victim;
        {
            std::lock_guard lock(state_mutex_);
            if (!store_ || store_ != store || lru_.size() <= limit)
                return;
            victim = lru_.front();
            lru_index_.erase(*victim);
            lru_.pop_front();
        }

        // LocalStore serialises physical mutations internally.  This may fsync,
        // so deliberately do it without state_mutex_: foreground cache readers
        // only need a shared_ptr snapshot and continue independently.
        (void)store->remove(*victim);
    }
}

bool PersistentBlockCache::put(const ObjectId& id, std::span<const uint8_t> data) {
    // Cache mutation is serialised independently of reads.  This also makes the
    // in-memory block count a reservation mechanism: two writers cannot both
    // observe one remaining slot and exceed max_blocks.
    std::lock_guard writer(writer_mutex_);

    std::shared_ptr<LocalStore> store;
    size_t limit = 0;
    {
        std::lock_guard lock(state_mutex_);
        store = store_;
        limit = config_.max_blocks;
    }
    if (!store || !limit)
        return false;

    try {
        if (store->has(id)) {
            mark_used(store, id);
            return true;
        }

        // Make one slot before doing the expensive encrypted/fsynced put.  No
        // recursive directory walk is performed here; lru_ is authoritative
        // for this cache process after the one-time open reconciliation.
        while (true) {
            std::optional<ObjectId> victim;
            {
                std::lock_guard lock(state_mutex_);
                if (!store_ || store_ != store || !config_.max_blocks)
                    return false;
                limit = config_.max_blocks;
                if (lru_.size() < limit)
                    break;
                victim = lru_.front();
                lru_index_.erase(*victim);
                lru_.pop_front();
            }
            (void)store->remove(*victim);
        }

        const bool ok = store->put(id, data);
        if (!ok)
            return false;
        {
            std::lock_guard lock(state_mutex_);
            if (store_ != store)
                return true; // Reconfigured concurrently after the physical put.
            auto existing = lru_index_.find(id);
            if (existing != lru_index_.end()) {
                lru_.splice(lru_.end(), lru_, existing->second);
                existing->second = std::prev(lru_.end());
            } else {
                lru_.push_back(id);
                lru_index_[id] = std::prev(lru_.end());
            }
        }
        return true;
    } catch (const std::exception& error) {
        Log::debug("persistent cache write: " + std::string(error.what()));
        return false;
    }
}

std::optional<Bytes> PersistentBlockCache::get(const ObjectId& id) {
    std::shared_ptr<LocalStore> store;
    {
        std::lock_guard lock(state_mutex_);
        store = store_;
    }
    if (!store)
        return {};

    try {
        auto data = store->get(id);
        if (data)
            mark_used(store, id);
        return data;
    } catch (const std::exception& error) {
        Log::debug("persistent cache read: " + std::string(error.what()));
        // A corrupt cache entry is disposable, but only remove it from the
        // same cache instance that produced the failed read.  A live
        // reconfiguration may already have installed a different cache root.
        std::lock_guard writer(writer_mutex_);
        bool current = false;
        {
            std::lock_guard lock(state_mutex_);
            current = store_ == store;
        }
        if (current) {
            (void)store->remove(id);
            std::lock_guard lock(state_mutex_);
            if (store_ == store) {
                auto found = lru_index_.find(id);
                if (found != lru_index_.end()) {
                    lru_.erase(found->second);
                    lru_index_.erase(found);
                }
            }
        }
        return {};
    }
}

bool PersistentBlockCache::has(const ObjectId& id) const {
    std::shared_ptr<LocalStore> store;
    {
        std::lock_guard lock(state_mutex_);
        store = store_;
    }
    return store && store->has(id);
}

bool PersistentBlockCache::remove(const ObjectId& id) {
    std::lock_guard writer(writer_mutex_);
    std::shared_ptr<LocalStore> store;
    {
        std::lock_guard lock(state_mutex_);
        store = store_;
    }
    if (!store)
        return false;

    const bool removed = store->remove(id);
    {
        std::lock_guard lock(state_mutex_);
        if (store_ == store) {
            auto found = lru_index_.find(id);
            if (found != lru_index_.end()) {
                lru_.erase(found->second);
                lru_index_.erase(found);
            }
        }
    }
    return removed;
}

size_t PersistentBlockCache::blocks() const {
    std::lock_guard lock(state_mutex_);
    return lru_.size();
}

std::filesystem::path PersistentBlockCache::metadata_path(const CacheConfig& config) {
    return config.path / "metadata" / "current.meta";
}

void PersistentBlockCache::remember_metadata(const MetadataRecord& record) {
    std::lock_guard metadata_lock(metadata_mutex_);
    CacheConfig config;
    std::shared_ptr<LocalStore> store;
    {
        std::lock_guard lock(state_mutex_);
        config = config_;
        store = store_;
    }
    if (!store || !config.prefer_metadata)
        return;
    try {
        auto plain = encode_metadata_record(record);
        auto sealed = aes_gcm_seal(key_, plain, cache_meta_magic);
        Writer writer;
        writer.raw(cache_meta_magic);
        writer.fixed(sealed.nonce);
        writer.fixed(sealed.tag);
        writer.bytes(sealed.ciphertext);
        atomic_write(metadata_path(config), writer.data());
    } catch (const std::exception& error) {
        Log::debug("persistent metadata cache write: " + std::string(error.what()));
    }
}

std::optional<MetadataRecord> PersistentBlockCache::metadata() const {
    std::lock_guard metadata_lock(metadata_mutex_);
    CacheConfig config;
    std::shared_ptr<LocalStore> store;
    {
        std::lock_guard lock(state_mutex_);
        config = config_;
        store = store_;
    }
    if (!store || !config.prefer_metadata)
        return {};
    try {
        auto path = metadata_path(config);
        if (!std::filesystem::exists(path))
            return {};
        std::ifstream in(path, std::ios::binary);
        Bytes bytes(std::istreambuf_iterator<char>(in), {});
        Reader reader(bytes);
        auto magic = reader.raw(cache_meta_magic.size());
        if (!std::equal(magic.begin(), magic.end(), cache_meta_magic.begin()))
            throw std::runtime_error("bad cached metadata header");
        auto nonce = reader.fixed<12>();
        auto tag = reader.fixed<16>();
        auto ciphertext = reader.bytes();
        reader.finish();
        return decode_metadata_record(aes_gcm_open(key_, nonce, tag, ciphertext, cache_meta_magic));
    } catch (const std::exception& error) {
        Log::debug("persistent metadata cache read: " + std::string(error.what()));
        return {};
    }
}
} // namespace macha
