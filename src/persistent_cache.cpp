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
    std::lock_guard lock(mutex_);
    open_locked();
}

void PersistentBlockCache::open_locked() {
    store_.reset();
    if (config_.path.empty() || !config_.max_blocks)
        return;
    try {
        store_ = std::make_unique<LocalStore>(config_.path, std::numeric_limits<uint64_t>::max(), key_);
        trim_locked();
    } catch (const std::exception& error) {
        store_.reset();
        Log::warn("persistent cache disabled: " + std::string(error.what()));
    }
}

void PersistentBlockCache::reconfigure(CacheConfig config) {
    std::lock_guard lock(mutex_);
    bool reopen = config.path != config_.path;
    config_ = std::move(config);
    if (config_.path.empty() || !config_.max_blocks) {
        store_.reset();
        return;
    }
    if (reopen || !store_)
        open_locked();
    else
        trim_locked();
}

bool PersistentBlockCache::enabled() const {
    std::lock_guard lock(mutex_);
    return static_cast<bool>(store_) && config_.max_blocks;
}

void PersistentBlockCache::trim_locked(const ObjectId* incoming) {
    if (!store_ || !config_.max_blocks)
        return;
    auto ids = store_->list();
    size_t target = config_.max_blocks;
    if (incoming && !store_->has(*incoming) && target)
        --target;
    while (ids.size() > target) {
        auto victim = std::min_element(ids.begin(), ids.end(), [&](const auto& a, const auto& b) {
            return store_->last_write(a) < store_->last_write(b);
        });
        if (victim == ids.end())
            break;
        (void)store_->remove(*victim);
        ids.erase(victim);
    }
}

bool PersistentBlockCache::put(const ObjectId& id, std::span<const uint8_t> data) {
    std::lock_guard lock(mutex_);
    if (!store_ || !config_.max_blocks)
        return false;
    try {
        if (store_->has(id)) {
            store_->touch(id);
            return true;
        }
        trim_locked(&id);
        bool ok = store_->put(id, data);
        if (ok)
            store_->touch(id);
        return ok;
    } catch (const std::exception& error) {
        Log::debug("persistent cache write: " + std::string(error.what()));
        return false;
    }
}

std::optional<Bytes> PersistentBlockCache::get(const ObjectId& id) {
    std::lock_guard lock(mutex_);
    if (!store_)
        return {};
    try {
        auto data = store_->get(id);
        if (data)
            store_->touch(id);
        return data;
    } catch (const std::exception& error) {
        Log::debug("persistent cache read: " + std::string(error.what()));
        (void)store_->remove(id);
        return {};
    }
}

bool PersistentBlockCache::has(const ObjectId& id) const {
    std::lock_guard lock(mutex_);
    return store_ && store_->has(id);
}

bool PersistentBlockCache::remove(const ObjectId& id) {
    std::lock_guard lock(mutex_);
    return store_ && store_->remove(id);
}

size_t PersistentBlockCache::blocks() const {
    std::lock_guard lock(mutex_);
    return store_ ? store_->list().size() : 0;
}

std::filesystem::path PersistentBlockCache::metadata_path_locked() const {
    return config_.path / "metadata" / "current.meta";
}

void PersistentBlockCache::remember_metadata(const MetadataRecord& record) {
    std::lock_guard lock(mutex_);
    if (!store_ || !config_.prefer_metadata)
        return;
    try {
        auto plain = encode_metadata_record(record);
        auto sealed = aes_gcm_seal(key_, plain, cache_meta_magic);
        Writer writer;
        writer.raw(cache_meta_magic);
        writer.fixed(sealed.nonce);
        writer.fixed(sealed.tag);
        writer.bytes(sealed.ciphertext);
        atomic_write(metadata_path_locked(), writer.data());
    } catch (const std::exception& error) {
        Log::debug("persistent metadata cache write: " + std::string(error.what()));
    }
}

std::optional<MetadataRecord> PersistentBlockCache::metadata() const {
    std::lock_guard lock(mutex_);
    if (!store_ || !config_.prefer_metadata)
        return {};
    try {
        auto path = metadata_path_locked();
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
