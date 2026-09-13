// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_store.hpp"
#include "codec.hpp"
#include "log.hpp"
#include "startup_progress.hpp"
#include "supervised.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> M{'D', 'H', 'T', 'O', 'B', 'J', '0', '1'};
constexpr std::array<uint8_t, 8> A{'M', 'A', 'C', 'H', 'A', 'A', 'C', '1'};
constexpr std::array<uint8_t, 8> P{'M', 'A', 'C', 'H', 'P', 'K', '0', '1'};
constexpr size_t accounting_slot_size = 128;
constexpr size_t accounting_body_size = accounting_slot_size - 32;
constexpr uint8_t accounting_none = 0;
constexpr uint8_t accounting_dirty = 3;
constexpr uint8_t pack_put = 1;
constexpr uint8_t pack_remove = 2;
constexpr uint8_t pack_touch = 3;
constexpr size_t pack_header_prefix_size = 93;
constexpr size_t pack_header_size = pack_header_prefix_size + 32;
constexpr uint64_t pack_payload_safety_limit = 128ULL * 1024 * 1024;

struct AccountingRecord {
    uint64_t sequence{};
    uint64_t used{};
    uint8_t operation{};
    ObjectId id{};
    uint64_t size{};
};

struct PackHeader {
    uint8_t type{};
    uint64_t touched_ms{};
    ObjectId id{};
    uint64_t plain_size{};
    uint64_t payload_size{};
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
};

void pwa_exact(int fd, std::span<const uint8_t> data, uint64_t offset) {
    size_t done = 0;
    while (done < data.size()) {
        auto n = ::pwrite(fd, data.data() + done, data.size() - done,
                          static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot write storage state: " + std::string(strerror(errno)));
        }
        done += static_cast<size_t>(n);
    }
}

std::optional<Bytes> pra_exact(int fd, size_t size, uint64_t offset) {
    Bytes out(size);
    size_t done = 0;
    while (done < out.size()) {
        auto n = ::pread(fd, out.data() + done, out.size() - done,
                         static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return {};
        }
        if (n == 0) return {};
        done += static_cast<size_t>(n);
    }
    return out;
}

Bytes encode_accounting(const AccountingRecord& record) {
    Writer body;
    body.raw(A);
    body.u64(record.sequence);
    body.u64(record.used);
    body.u8(record.operation);
    body.u64(record.size);
    body.fixed(record.id.bytes);
    while (body.data().size() < accounting_body_size) body.u8(0);
    if (body.data().size() != accounting_body_size)
        throw std::runtime_error("internal accounting record size error");
    auto hash = sha256(body.data());
    Writer out;
    out.raw(body.data());
    out.fixed(hash.bytes);
    return out.take();
}

std::optional<AccountingRecord> decode_accounting(std::span<const uint8_t> bytes) {
    if (bytes.size() != accounting_slot_size) return {};
    auto body = bytes.first(accounting_body_size);
    auto expected = sha256(body);
    if (!std::equal(expected.bytes.begin(), expected.bytes.end(),
                    bytes.begin() + accounting_body_size))
        return {};
    try {
        Reader reader(body);
        auto magic = reader.raw(A.size());
        if (!std::equal(magic.begin(), magic.end(), A.begin())) return {};
        AccountingRecord record;
        record.sequence = reader.u64();
        record.used = reader.u64();
        record.operation = reader.u8();
        record.size = reader.u64();
        record.id.bytes = reader.fixed<32>();
        if (record.operation != accounting_none && record.operation != accounting_dirty)
            return {};
        return record;
    } catch (...) {
        return {};
    }
}

Bytes encode_pack_header(const PackHeader& header) {
    Writer prefix;
    prefix.raw(P);
    prefix.u8(header.type);
    prefix.u64(header.touched_ms);
    prefix.fixed(header.id.bytes);
    prefix.u64(header.plain_size);
    prefix.u64(header.payload_size);
    prefix.fixed(header.nonce);
    prefix.fixed(header.tag);
    if (prefix.data().size() != pack_header_prefix_size)
        throw std::runtime_error("internal pack header size error");
    auto checksum = sha256(prefix.data());
    Writer out;
    out.raw(prefix.data());
    out.fixed(checksum.bytes);
    return out.take();
}

std::optional<PackHeader> decode_pack_header(std::span<const uint8_t> bytes) {
    if (bytes.size() != pack_header_size) return {};
    const auto prefix = bytes.first(pack_header_prefix_size);
    const auto checksum = sha256(prefix);
    if (!std::equal(checksum.bytes.begin(), checksum.bytes.end(),
                    bytes.begin() + pack_header_prefix_size))
        return {};
    try {
        Reader r(prefix);
        auto magic = r.raw(P.size());
        if (!std::equal(magic.begin(), magic.end(), P.begin())) return {};
        PackHeader h;
        h.type = r.u8();
        h.touched_ms = r.u64();
        h.id.bytes = r.fixed<32>();
        h.plain_size = r.u64();
        h.payload_size = r.u64();
        h.nonce = r.fixed<12>();
        h.tag = r.fixed<16>();
        r.finish();
        if (h.type < pack_put || h.type > pack_touch) return {};
        if (h.type == pack_put) {
            if (!h.payload_size || h.payload_size > pack_payload_safety_limit ||
                h.plain_size != h.payload_size)
                return {};
        } else if (h.payload_size || h.plain_size) {
            return {};
        }
        return h;
    } catch (...) {
        return {};
    }
}

// The first offset at or after `from` holding a header that decodes, or
// nothing if the rest of the file has none. Recovery asks this when it meets
// an undecodable header: a torn append leaves nothing decodable behind it,
// whereas mid-pack damage (bit rot, an external edit) leaves the records after
// it intact. Payloads are AES-GCM ciphertext, so a magic match followed by a
// valid SHA-256 inside one is not a realistic false positive. Reads the
// remainder of one pack in bounded chunks; hashes only where the magic matches.
std::optional<uint64_t> find_next_pack_header(int fd, uint64_t from, uint64_t file_size) {
    constexpr size_t chunk = 1024 * 1024;
    static_assert(chunk > 2 * pack_header_size);
    uint64_t position = from;
    while (position + pack_header_size <= file_size) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk, file_size - position));
        auto bytes = pra_exact(fd, want, position);
        if (!bytes)
            throw std::runtime_error("cannot read pack during recovery");
        const std::span<const uint8_t> view(*bytes);
        size_t search = 0;
        while (search + pack_header_size <= view.size()) {
            auto hit = std::search(view.begin() + static_cast<std::ptrdiff_t>(search), view.end(),
                                   P.begin(), P.end());
            if (hit == view.end())
                break;
            const size_t index = static_cast<size_t>(hit - view.begin());
            if (index + pack_header_size > view.size())
                break;
            if (decode_pack_header(view.subspan(index, pack_header_size)))
                return position + index;
            search = index + 1;
        }
        if (want < chunk)
            break;
        // Overlap the next chunk by a header so one straddling the boundary
        // is still seen whole.
        position += want - (pack_header_size - 1);
    }
    return {};
}

void wa(int fd, std::span<const uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
}

Bytes rf(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("cannot open object: " + std::string(strerror(errno)));
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        const auto error = std::string(strerror(errno));
        close(fd);
        throw std::runtime_error("cannot stat object: " + error);
    }
    Bytes out(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < out.size()) {
        auto n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            const auto error = std::string(strerror(errno));
            close(fd);
            throw std::runtime_error("cannot read object: " + error);
        }
        if (n == 0) {
            close(fd);
            throw std::runtime_error("short object read");
        }
        done += static_cast<size_t>(n);
    }
    if (close(fd) != 0)
        throw std::runtime_error("cannot close object: " + std::string(strerror(errno)));
    return out;
}

void syncdir(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        int rc;
        do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
        ::close(fd);
    }
}

std::string pack_name(uint64_t sequence) {
    std::ostringstream out;
    out << "pack-" << std::setw(20) << std::setfill('0') << sequence << ".pack";
    return out.str();
}

std::optional<uint64_t> pack_sequence(std::string_view name) {
    if (name.size() != 30 || !name.starts_with("pack-") || !name.ends_with(".pack"))
        return {};
    try {
        size_t used = 0;
        auto value = std::stoull(std::string(name.substr(5, 20)), &used, 10);
        if (used != 20 || !value) return {};
        return value;
    } catch (...) {
        return {};
    }
}

std::filesystem::file_time_type file_time_from_unix_ms(uint64_t value) {
    using namespace std::chrono;
    const auto target = system_clock::time_point(milliseconds(value));
    return std::filesystem::file_time_type::clock::now() + (target - system_clock::now());
}

} // namespace

StorageLock::StorageLock(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    auto path = root / ".macha.lock";
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0)
        throw std::runtime_error("cannot open storage lock: " + std::string(strerror(errno)));
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        auto error = std::string(strerror(errno));
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("storage path is already in use: " + error);
    }
}

StorageLock::~StorageLock() {
    if (fd_ >= 0) {
        (void)flock(fd_, LOCK_UN);
        close(fd_);
    }
}

LocalStore::LocalStore(std::filesystem::path root, LocalStoreOptions options,
                       std::array<uint8_t, 32> key, LocalStoreMode mode,
                       std::shared_ptr<DurabilityDomain> durability_domain)
    : root_(std::move(root)), objects_(root_ / "objects"), packs_(root_ / "packs"),
      accounting_path_(root_ / ".macha.accounting"), limit_(options.limit),
      reserve_free_(options.reserve_free), pack_threshold_(options.pack_threshold),
      pack_target_size_(options.pack_target_size), key_(key), mode_(mode),
      durability_domain_(std::move(durability_domain)) {
    if (!limit_) throw std::runtime_error("local store limit must be non-zero");
    if ((pack_threshold_ == 0) != (pack_target_size_ == 0))
        throw std::runtime_error("pack threshold and target size must both be zero or non-zero");
    if (pack_threshold_ && pack_target_size_ < pack_threshold_)
        throw std::runtime_error("pack target size must be >= pack threshold");

    std::filesystem::create_directories(objects_);
    std::filesystem::create_directories(packs_);
    {
        std::lock_guard lock(m_);
        rebuild_pack_index_locked(true);
    }

    if (mode_ == LocalStoreMode::ephemeral) {
        durability_domain_.reset();
        scan_thread_ = std::jthread([this](std::stop_token stop) {
            run_supervised("local-store-scan", [this, stop] {
                try {
                    scan(stop);
                } catch (const std::exception& error) {
                    scan_failed_.store(true, std::memory_order_release);
                    accounting_cv_.notify_all();
                    Log::warn("storage accounting scan failed path=" + root_.string() +
                              " error=" + error.what());
                    throw;
                } catch (...) {
                    scan_failed_.store(true, std::memory_order_release);
                    accounting_cv_.notify_all();
                    Log::warn("storage accounting scan failed path=" + root_.string() +
                              " error=<unknown>");
                    throw;
                }
            });
        });
        return;
    }

    if (!durability_domain_)
        durability_domain_ = std::make_shared<DurabilityDomain>(1, root_);
    accounting_fd_ = ::open(accounting_path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (accounting_fd_ < 0)
        throw std::runtime_error("cannot open accounting state: " + std::string(strerror(errno)));

    // Packed indexes are deliberately derived from the authoritative append-only
    // records at every start. A *clean* accounting checkpoint is trustworthy
    // with packs too: ensure_accounting_dirty() persists "dirty" before the
    // first mutation of a session, so a torn pack tail can only exist behind
    // a dirty checkpoint. Until 0.32.3 any pack forced a full walk of the
    // object tree on every start (4 min on gbni-1, 25+ min on es-1 under
    // import load) with every put waiting for it -- a write outage after
    // each restart. A dirty checkpoint is carried as an estimate while the
    // walk reconciles; puts are admitted against it (the configured
    // reserve_free headroom covers the bounded error) and the walk's total
    // replaces it when done.
    bool clean = false;
    const bool restored = restore_accounting(&clean);
    if (restored && clean) {
        accounting_trusted_.store(true, std::memory_order_release);
        scan_complete_.store(true, std::memory_order_release);
        Log::debug("storage accounting restored path=" + root_.string() +
                   " used=" + std::to_string(used_.load(std::memory_order_relaxed)));
    } else {
        if (restored) {
            accounting_estimate_.store(true, std::memory_order_release);
            Log::info("storage accounting estimate path=" + root_.string() +
                      " used=" + std::to_string(used_.load(std::memory_order_relaxed)) +
                      " (dirty checkpoint; admitting writes while the scan reconciles)");
        }
        scan_thread_ = std::jthread([this](std::stop_token stop) {
            run_supervised("local-store-scan", [this, stop] {
                try {
                    scan(stop);
                } catch (const std::exception& error) {
                    scan_failed_.store(true, std::memory_order_release);
                    accounting_cv_.notify_all();
                    Log::warn("storage accounting scan failed path=" + root_.string() +
                              " error=" + error.what());
                    throw;
                } catch (...) {
                    scan_failed_.store(true, std::memory_order_release);
                    accounting_cv_.notify_all();
                    Log::warn("storage accounting scan failed path=" + root_.string() +
                              " error=<unknown>");
                    throw;
                }
            });
        });
    }
    presence_thread_ = std::jthread([this](std::stop_token stop) {
        run_supervised("local-store-presence", [this, stop] { warm_presence_index(stop); });
    });
}

LocalStore::LocalStore(std::filesystem::path root, uint64_t limit,
                       std::array<uint8_t, 32> key, LocalStoreMode mode,
                       std::shared_ptr<DurabilityDomain> durability_domain)
    : LocalStore(std::move(root), LocalStoreOptions{limit, 0, 0, 0}, key, mode,
                 std::move(durability_domain)) {}

LocalStore::~LocalStore() {
    if (presence_thread_.joinable()) {
        presence_thread_.request_stop();
        presence_thread_.join();
    }
    if (scan_thread_.joinable()) {
        scan_thread_.request_stop();
        scan_thread_.join();
    }
    if (accounting_fd_ >= 0) {
        bool can_checkpoint = accounting_trusted_.load(std::memory_order_acquire);
        if (can_checkpoint && durability_domain_ && last_mutation_generation_) {
            try {
                durability_domain_->await_durable(last_mutation_generation_,
                                                  DurabilityUrgency::immediate);
            } catch (...) { can_checkpoint = false; }
        }
        if (can_checkpoint) {
            try { checkpoint_accounting_locked(); } catch (...) {}
        }
        close(accounting_fd_);
        accounting_fd_ = -1;
    }
}

std::filesystem::path LocalStore::path(const ObjectId& id) const {
    auto s = to_string(id);
    return objects_ / s.substr(0, 2) / s.substr(2, 2) / (s + ".obj");
}

void LocalStore::persist_accounting(uint64_t used, uint8_t operation, const ObjectId& id,
                                    uint64_t size, bool durable) {
    AccountingRecord record{++accounting_sequence_, used, operation, id, size};
    accounting_slot_ ^= 1U;
    auto encoded = encode_accounting(record);
    pwa_exact(accounting_fd_, encoded, accounting_slot_ * accounting_slot_size);
    if (durable && fsync(accounting_fd_) != 0)
        throw std::runtime_error("cannot sync accounting state: " + std::string(strerror(errno)));
}

bool LocalStore::restore_accounting(bool* clean) {
    std::optional<AccountingRecord> best;
    unsigned best_slot = 0;
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto bytes = pra_exact(accounting_fd_, accounting_slot_size, slot * accounting_slot_size);
        if (!bytes) continue;
        auto decoded = decode_accounting(*bytes);
        if (!decoded) continue;
        if (!best || decoded->sequence > best->sequence) {
            best = *decoded;
            best_slot = slot;
        }
    }
    if (!best) return false;
    accounting_sequence_ = best->sequence;
    accounting_slot_ = best_slot;
    used_.store(best->used, std::memory_order_relaxed);
    if (clean)
        *clean = best->operation != accounting_dirty;
    return true;
}

void LocalStore::ensure_accounting_dirty(std::unique_lock<std::mutex>& lock) {
    if (mode_ == LocalStoreMode::ephemeral || accounting_dirty_) return;
    accounting_state_cv_.wait(lock, [this] { return !accounting_dirty_in_progress_; });
    if (accounting_dirty_) return;
    accounting_dirty_in_progress_ = true;
    const auto used = used_.load(std::memory_order_relaxed);
    lock.unlock();
    try {
        ObjectId none{};
        persist_accounting(used, accounting_dirty, none, 0, true);
    } catch (...) {
        lock.lock();
        accounting_dirty_in_progress_ = false;
        accounting_state_cv_.notify_all();
        throw;
    }
    lock.lock();
    accounting_dirty_ = true;
    accounting_dirty_in_progress_ = false;
    accounting_state_cv_.notify_all();
}

void LocalStore::checkpoint_accounting_locked() {
    if (mode_ == LocalStoreMode::ephemeral || !accounting_dirty_) return;
    ObjectId none{};
    persist_accounting(used_.load(std::memory_order_relaxed), accounting_none, none, 0, true);
    accounting_dirty_ = false;
    accounting_trusted_.store(true, std::memory_order_release);
}

void LocalStore::reap_durable_generations_locked() {
    if (!durability_domain_) {
        provisional_generations_.clear();
        provisional_order_.clear();
        return;
    }
    const auto durable = durability_domain_->durable_generation();
    while (!provisional_order_.empty() && provisional_order_.front().first <= durable) {
        const auto [generation, id] = provisional_order_.front();
        provisional_order_.pop_front();
        const auto found = provisional_generations_.find(id);
        if (found != provisional_generations_.end() && found->second == generation)
            provisional_generations_.erase(found);
    }
}

bool LocalStore::physical_space_available_locked(uint64_t need) const {
    const auto before = used_.load(std::memory_order_relaxed);
    if (need > limit_ || reserved_write_bytes_ > limit_ - need ||
        before > limit_ - need - reserved_write_bytes_)
        return false;
    return true;
}

bool LocalStore::filesystem_space_available_for_reservations() const {
    uint64_t reserved = 0;
    {
        std::lock_guard lock(m_);
        reserved = reserved_write_bytes_;
    }
    std::error_code error;
    const auto space = std::filesystem::space(root_, error);
    if (error) return false;
    if (reserved > space.available)
        return false;
    return reserve_free_ <= space.available - reserved;
}

std::optional<LocalStore::LooseStamp>
LocalStore::loose_stamp(const std::filesystem::path& path) {
    struct stat value {};
    if (::stat(path.c_str(), &value) != 0 || value.st_size < 0)
        return {};
#if defined(__APPLE__)
    const auto modified = static_cast<int64_t>(value.st_mtimespec.tv_sec) * 1000000000LL +
                          value.st_mtimespec.tv_nsec;
    const auto changed = static_cast<int64_t>(value.st_ctimespec.tv_sec) * 1000000000LL +
                         value.st_ctimespec.tv_nsec;
#else
    const auto modified = static_cast<int64_t>(value.st_mtim.tv_sec) * 1000000000LL +
                          value.st_mtim.tv_nsec;
    const auto changed = static_cast<int64_t>(value.st_ctim.tv_sec) * 1000000000LL +
                         value.st_ctim.tv_nsec;
#endif
    return LooseStamp{static_cast<uint64_t>(value.st_dev),
                      static_cast<uint64_t>(value.st_ino),
                      static_cast<uint64_t>(value.st_size), modified, changed};
}

std::shared_ptr<std::mutex> LocalStore::object_mutex(const ObjectId& id) const {
    std::lock_guard lock(object_mutex_map_mutex_);
    if (auto found = object_mutexes_.find(id); found != object_mutexes_.end()) {
        if (auto existing = found->second.lock())
            return existing;
        object_mutexes_.erase(found);
    }
    auto created = std::make_shared<std::mutex>();
    object_mutexes_[id] = created;
    if (object_mutexes_.size() > 4096) {
        std::erase_if(object_mutexes_, [](const auto& item) { return item.second.expired(); });
    }
    return created;
}

void LocalStore::remember_verified_loose_locked(const ObjectId& id,
                                                const LooseStamp& stamp) const {
    const auto sequence = ++verified_loose_sequence_;
    verified_loose_[id] = {stamp, sequence};
    verified_loose_order_.push_back({sequence, id});
    while (verified_loose_.size() > verified_loose_limit && !verified_loose_order_.empty()) {
        const auto [candidate_sequence, candidate] = verified_loose_order_.front();
        verified_loose_order_.pop_front();
        auto found = verified_loose_.find(candidate);
        if (found != verified_loose_.end() && found->second.sequence == candidate_sequence)
            verified_loose_.erase(found);
    }
    if (verified_loose_order_.size() > verified_loose_limit * 4) {
        verified_loose_order_.clear();
        for (const auto& [candidate, verified] : verified_loose_)
            verified_loose_order_.push_back({verified.sequence, candidate});
        std::sort(verified_loose_order_.begin(), verified_loose_order_.end());
    }
}

void LocalStore::forget_verified_loose_locked(const ObjectId& id) const {
    verified_loose_.erase(id);
    present_loose_.erase(id);
}

void LocalStore::select_active_pack_locked(uint64_t next_record_size) {
    if (!pack_threshold_) throw std::runtime_error("packing is disabled");
    if (active_pack_.empty() ||
        (active_pack_size_ && active_pack_size_ + next_record_size > pack_target_size_)) {
        active_pack_ = packs_ / pack_name(next_pack_sequence_++);
        active_pack_size_ = 0;
    }
}

bool LocalStore::append_pack_record_locked(uint8_t type, const ObjectId& id,
                                           std::span<const uint8_t> data, uint64_t touched_ms,
                                           PackEntry* entry, uint64_t* record_size,
                                           std::unique_lock<std::mutex>& lock) {
    PackHeader h;
    h.type = type;
    h.touched_ms = touched_ms;
    h.id = id;
    const auto before_write = before_packed_write_for_tests_;
    lock.unlock();
    Bytes payload;
    Bytes header;
    uint64_t total = 0;
    std::filesystem::path target;
    uint64_t start = 0;
    int fd = -1;
    std::unique_lock<std::mutex> pack_io_lock;
    try {
        if (before_write)
            before_write(id);
        if (type == pack_put) {
            auto sealed = aes_gcm_seal(key_, data, id.bytes);
            payload = std::move(sealed.ciphertext);
            h.plain_size = data.size();
            h.payload_size = payload.size();
            h.nonce = sealed.nonce;
            h.tag = sealed.tag;
        }
        header = encode_pack_header(h);
        total = header.size() + payload.size();

        // The pack stream has its own physical single-writer domain. m_ stays
        // available throughout encryption and disk I/O, so unrelated viewer
        // reads and loose-object work are not trapped behind a loader append.
        pack_io_lock = std::unique_lock(pack_io_mutex_);
        select_active_pack_locked(total);
        target = active_pack_;
        start = active_pack_size_;
        fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0)
            throw std::runtime_error("cannot open pack: " + std::string(strerror(errno)));
        wa(fd, header);
        if (!payload.empty()) wa(fd, payload);
        if (::close(fd) != 0)
            throw std::runtime_error("cannot close pack: " + std::string(strerror(errno)));
        fd = -1;
        active_pack_size_ += total;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        // A failed append (ENOSPC, EIO, short write) must not poison the live
        // process with a torn record. Restore the previous durable boundary when
        // possible; if rollback itself fails, abandon this pack so later writes
        // never append behind the torn bytes. Restart recovery will truncate it.
        int rollback = target.empty() ? -1 : ::open(target.c_str(), O_WRONLY);
        if (rollback >= 0) {
            if (::ftruncate(rollback, static_cast<off_t>(start)) == 0)
                active_pack_size_ = start;
            else {
                active_pack_.clear();
                active_pack_size_ = 0;
            }
            ::close(rollback);
        } else {
            active_pack_.clear();
            active_pack_size_ = 0;
        }
        lock.lock();
        throw;
    }
    lock.lock();
    if (entry) {
        entry->file = target;
        entry->payload_offset = start + header.size();
        entry->payload_size = h.payload_size;
        entry->plain_size = h.plain_size;
        entry->record_size = total;
        entry->touched_unix_ms = touched_ms;
        entry->nonce = h.nonce;
        entry->tag = h.tag;
    }
    if (record_size) *record_size = total;
    return true;
}

std::optional<Bytes> LocalStore::get_packed_locked(
    const ObjectId& id, std::unique_lock<std::mutex>& lock) const {
    auto found = packed_.find(id);
    if (found == packed_.end()) return {};
    const auto entry = found->second;
    const auto before_read = before_packed_read_for_tests_;
    ++active_pack_readers_[entry.file];

    // A lightweight logical reader lease pins the indexed pack against
    // compaction before any filesystem call. This leaves even open(2) outside
    // the global index lock; compaction switches the index and event-waits for
    // old readers before unlinking the victim.
    lock.unlock();
    int fd = -1;
    auto release_reader = [&] {
        if (!lock.owns_lock())
            lock.lock();
        auto active = active_pack_readers_.find(entry.file);
        if (active == active_pack_readers_.end() || active->second == 0)
            throw std::logic_error("local store pack reader underflow");
        if (--active->second == 0)
            active_pack_readers_.erase(active);
        pack_readers_cv_.notify_all();
    };
    try {
        if (before_read)
            before_read(id);
        fd = ::open(entry.file.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("cannot open pack: " + std::string(strerror(errno)));
        auto payload = pra_exact(fd, static_cast<size_t>(entry.payload_size),
                                 entry.payload_offset);
        const int close_rc = ::close(fd);
        fd = -1;
        if (!payload)
            throw std::runtime_error("short packed object read");
        if (close_rc != 0)
            throw std::runtime_error("cannot close pack: " + std::string(strerror(errno)));
        auto plain = aes_gcm_open(key_, entry.nonce, entry.tag, *payload, id.bytes);
        if (plain.size() != entry.plain_size || object_id(plain) != id)
            throw std::runtime_error("packed object integrity failure");
        release_reader();
        return plain;
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        release_reader();
        throw;
    }
}

void LocalStore::rebuild_pack_index_locked(bool truncate_incomplete_tail) {
    packed_.clear();
    pack_dead_bytes_ = 0;
    next_pack_sequence_ = 1;
    active_pack_.clear();
    active_pack_size_ = 0;

    std::vector<std::pair<uint64_t, std::filesystem::path>> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packs_, error)) {
        if (error) break;
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.find(".tmp") != std::string::npos || name.starts_with(".compact-")) {
            std::error_code remove_error;
            std::filesystem::remove(entry.path(), remove_error);
            continue;
        }
        if (auto seq = pack_sequence(name))
            files.push_back({*seq, entry.path()});
    }
    if (error) throw std::runtime_error("cannot enumerate packs: " + error.message());
    std::sort(files.begin(), files.end());

    for (const auto& [sequence, file] : files) {
        next_pack_sequence_ = std::max(next_pack_sequence_, sequence + 1);
        int fd = ::open(file.c_str(), truncate_incomplete_tail ? O_RDWR : O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("cannot open pack during recovery: " +
                                     std::string(strerror(errno)));
        struct stat st{};
        if (::fstat(fd, &st) != 0 || st.st_size < 0) {
            const auto saved = errno;
            ::close(fd);
            throw std::runtime_error("cannot stat pack during recovery: " +
                                     std::string(strerror(saved)));
        }
        const uint64_t file_size = static_cast<uint64_t>(st.st_size);
        uint64_t offset = 0;
        while (offset < file_size) {
            if (file_size - offset < pack_header_size) {
                if (truncate_incomplete_tail && ::ftruncate(fd, static_cast<off_t>(offset)) == 0)
                    Log::warn("truncated incomplete pack tail path=" + file.string());
                break;
            }
            auto bytes = pra_exact(fd, pack_header_size, offset);
            if (!bytes) {
                ::close(fd);
                throw std::runtime_error("cannot read pack header during recovery");
            }
            auto header = decode_pack_header(*bytes);
            if (!header) {
                // Two shapes reach here. A power loss between write() and the
                // domain's syncfs can leave the file extended by a header whose
                // block never held one (zero-filled or partial): nothing was
                // acknowledged past this point and nothing decodable follows,
                // so the tail is discarded like the other two torn shapes. If a
                // decodable record does follow, the damage is inside the pack;
                // the unreadable span is skipped and left as dead bytes for
                // compaction, and the objects it held are repaired from
                // replicas. Neither shape takes the backend offline: until
                // 0.38.3 both threw, and one such header at a pack tail cost
                // gbni-1 its whole 8 TB backend (2026-09-12).
                const bool zero_header = std::all_of(bytes->begin(), bytes->end(),
                                                     [](uint8_t b) { return b == 0; });
                const auto next = find_next_pack_header(fd, offset + 1, file_size);
                if (!next) {
                    const auto discarded = file_size - offset;
                    if (truncate_incomplete_tail) {
                        if (::ftruncate(fd, static_cast<off_t>(offset)) != 0) {
                            const auto saved = errno;
                            ::close(fd);
                            throw std::runtime_error("cannot truncate torn pack tail at " +
                                                     file.string() + " offset=" +
                                                     std::to_string(offset) + ": " +
                                                     strerror(saved));
                        }
                        Log::warn("truncated undecodable pack tail path=" + file.string() +
                                  " offset=" + std::to_string(offset) +
                                  " bytes=" + std::to_string(discarded) +
                                  " zero_header=" + std::to_string(zero_header ? 1 : 0));
                    }
                    pack_recovery_truncated_tails_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                const auto skipped = *next - offset;
                Log::error("skipped unreadable pack region path=" + file.string() +
                           " offset=" + std::to_string(offset) +
                           " bytes=" + std::to_string(skipped) +
                           " zero_header=" + std::to_string(zero_header ? 1 : 0) +
                           " (objects recorded there are absent from this backend)");
                pack_dead_bytes_ += skipped;
                pack_recovery_skipped_regions_.fetch_add(1, std::memory_order_relaxed);
                pack_recovery_skipped_bytes_.fetch_add(skipped, std::memory_order_relaxed);
                offset = *next;
                continue;
            }
            const uint64_t total = pack_header_size + header->payload_size;
            if (total > file_size - offset) {
                if (truncate_incomplete_tail && ::ftruncate(fd, static_cast<off_t>(offset)) == 0)
                    Log::warn("truncated incomplete packed object path=" + file.string());
                break;
            }
            if (header->type == pack_put) {
                if (auto old = packed_.find(header->id); old != packed_.end())
                    pack_dead_bytes_ += old->second.record_size;
                PackEntry entry;
                entry.file = file;
                entry.payload_offset = offset + pack_header_size;
                entry.payload_size = header->payload_size;
                entry.plain_size = header->plain_size;
                entry.record_size = total;
                entry.touched_unix_ms = header->touched_ms;
                entry.nonce = header->nonce;
                entry.tag = header->tag;
                packed_[header->id] = entry;
            } else if (header->type == pack_remove) {
                if (auto old = packed_.find(header->id); old != packed_.end()) {
                    pack_dead_bytes_ += old->second.record_size;
                    packed_.erase(old);
                }
                pack_dead_bytes_ += total;
            } else {
                if (auto old = packed_.find(header->id); old != packed_.end())
                    old->second.touched_unix_ms = header->touched_ms;
                pack_dead_bytes_ += total;
            }
            offset += total;
        }
        ::close(fd);
        std::error_code size_error;
        const auto actual = std::filesystem::file_size(file, size_error);
        if (!size_error) {
            active_pack_ = file;
            active_pack_size_ = actual;
        }
    }
    if (active_pack_size_ >= pack_target_size_) {
        active_pack_.clear();
        active_pack_size_ = 0;
    }
}

bool LocalStore::put_loose_locked(const ObjectId& id, std::span<const uint8_t> data,
                                  StoreWriteDurability durability, uint64_t* deferred_generation,
                                  std::unique_lock<std::mutex>& lock) {
    // Encryption and physical I/O are object-scoped work. Reserve capacity and
    // make crash accounting dirty under the short store lock, then let unrelated
    // object reads/writes proceed while this object is sealed and installed.
    constexpr uint64_t loose_header_size = M.size() + sizeof(uint64_t) + 12 + 16;
    const uint64_t need = loose_header_size + data.size();
    if (!physical_space_available_locked(need)) return false;
    const bool ephemeral = mode_ == LocalStoreMode::ephemeral;
    reserved_write_bytes_ += need;
    auto before_write = before_loose_write_for_tests_;
    lock.unlock();
    if (!filesystem_space_available_for_reservations()) {
        lock.lock();
        reserved_write_bytes_ -= need;
        return false;
    }
    if (!ephemeral) {
        lock.lock();
        ensure_accounting_dirty(lock);
        lock.unlock();
    }
    std::string temp;
    int fd = -1;
    try {
        if (before_write)
            before_write(id);
        auto sealed = aes_gcm_seal(key_, data, id.bytes);
        Writer header;
        header.raw(M);
        header.u64(data.size());
        header.fixed(sealed.nonce);
        header.fixed(sealed.tag);
        if (header.data().size() + sealed.ciphertext.size() != need)
            throw std::logic_error("local store loose object size mismatch");

        auto p = path(id);
        std::filesystem::create_directories(p.parent_path());
        temp = p.string() + ".tmp." + std::to_string(getpid()) + "." +
               std::to_string(unix_ms());
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) throw std::runtime_error(strerror(errno));
        wa(fd, header.data());
        wa(fd, sealed.ciphertext);
        if (::close(fd) != 0) throw std::runtime_error(strerror(errno));
        fd = -1;
        if (::rename(temp.c_str(), p.c_str()) != 0) throw std::runtime_error(strerror(errno));
        const auto installed_stamp = loose_stamp(p);
        if (!installed_stamp)
            throw std::runtime_error("cannot stat installed local object");
        lock.lock();
        if (reserved_write_bytes_ < need)
            throw std::logic_error("local store write reservation underflow");
        reserved_write_bytes_ -= need;
        used_.fetch_add(need, std::memory_order_relaxed);
        remember_verified_loose_locked(id, *installed_stamp);
        present_loose_.insert(id);
        uint64_t generation = 0;
        if (!ephemeral && durability_domain_) {
            generation = durability_domain_->complete_mutation(p, p.parent_path());
            last_mutation_generation_ = std::max(last_mutation_generation_, generation);
            provisional_generations_[id] = generation;
            provisional_order_.push_back({generation, id});
            reap_durable_generations_locked();
        }
        if (deferred_generation) *deferred_generation = generation;
        if (!ephemeral && durability == StoreWriteDurability::immediate && generation) {
            lock.unlock();
            durability_domain_->await_durable(generation, DurabilityUrgency::immediate);
        }
        return true;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        std::error_code remove_error;
        if (!temp.empty())
            std::filesystem::remove(temp, remove_error);
        if (!lock.owns_lock())
            lock.lock();
        if (reserved_write_bytes_ < need)
            throw std::logic_error("local store write reservation underflow during failure");
        reserved_write_bytes_ -= need;
        throw;
    }
}

bool LocalStore::put_packed_locked(const ObjectId& id, std::span<const uint8_t> data,
                                   StoreWriteDurability durability, uint64_t* deferred_generation,
                                   std::unique_lock<std::mutex>& lock) {
    const uint64_t need = pack_header_size + data.size();
    if (!physical_space_available_locked(need)) return false;
    const bool ephemeral = mode_ == LocalStoreMode::ephemeral;
    reserved_write_bytes_ += need;
    lock.unlock();
    if (!filesystem_space_available_for_reservations()) {
        lock.lock();
        reserved_write_bytes_ -= need;
        return false;
    }
    lock.lock();
    if (!ephemeral) ensure_accounting_dirty(lock);
    PackEntry entry;
    uint64_t record_size = 0;
    try {
        append_pack_record_locked(pack_put, id, data, unix_ms(), &entry, &record_size, lock);
    } catch (...) {
        if (reserved_write_bytes_ < need)
            throw std::logic_error("local store packed write reservation underflow");
        reserved_write_bytes_ -= need;
        throw;
    }
    if (reserved_write_bytes_ < need)
        throw std::logic_error("local store packed write reservation underflow");
    reserved_write_bytes_ -= need;
    if (auto old = packed_.find(id); old != packed_.end())
        pack_dead_bytes_ += old->second.record_size;
    packed_[id] = entry;
    used_.fetch_add(record_size, std::memory_order_relaxed);

    uint64_t generation = 0;
    if (!ephemeral && durability_domain_) {
        generation = durability_domain_->complete_mutation(entry.file, packs_);
        last_mutation_generation_ = std::max(last_mutation_generation_, generation);
        provisional_generations_[id] = generation;
        provisional_order_.push_back({generation, id});
        reap_durable_generations_locked();
    }
    if (deferred_generation) *deferred_generation = generation;
    if (!ephemeral && durability == StoreWriteDurability::immediate && generation) {
        lock.unlock();
        durability_domain_->await_durable(generation, DurabilityUrgency::immediate);
    }
    return true;
}

bool LocalStore::put_impl(const ObjectId& id, std::span<const uint8_t> data,
                          StoreWriteDurability durability, uint64_t* deferred_generation) {
    if (object_id(data) != id) throw std::runtime_error("object hash mismatch");
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    std::unique_lock lock(m_);
    wait_for_accounting(lock);

    if (packed_.contains(id)) {
        std::optional<Bytes> existing;
        try {
            existing = get_packed_locked(id, lock);
        } catch (...) {
            existing.reset();
        }
        if (existing && existing->size() == data.size() &&
            std::equal(existing->begin(), existing->end(), data.begin())) {
            // A compact touch record makes the re-affirmation age crash-recoverable.
            uint64_t record_size = 0;
            if (mode_ != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
            const auto touched = unix_ms();
            append_pack_record_locked(pack_touch, id, {}, touched, nullptr, &record_size, lock);
            packed_.at(id).touched_unix_ms = touched;
            pack_dead_bytes_ += record_size;
            used_.fetch_add(record_size, std::memory_order_relaxed);
            uint64_t generation = 0;
            if (mode_ != LocalStoreMode::ephemeral && durability_domain_) {
                generation = durability_domain_->complete_mutation(active_pack_, packs_);
                last_mutation_generation_ = std::max(last_mutation_generation_, generation);
                provisional_generations_[id] = generation;
                provisional_order_.push_back({generation, id});
            }
            if (deferred_generation) *deferred_generation = generation;
            if (durability == StoreWriteDurability::immediate && generation) {
                lock.unlock();
                durability_domain_->await_durable(generation, DurabilityUrgency::immediate);
            }
            return true;
        }
        // A corrupt packed record is superseded by the new record. The old bytes
        // remain dead until compaction; no in-place rewrite can damage neighbours.
        return put_packed_locked(id, data, durability, deferred_generation, lock);
    }

    const auto loose_path = path(id);
    lock.unlock();
    const bool loose_exists = std::filesystem::exists(loose_path);
    lock.lock();
    if (loose_exists) {
        std::optional<LooseStamp> remembered;
        if (auto verified = verified_loose_.find(id); verified != verified_loose_.end())
            remembered = verified->second.stamp;
        std::optional<Bytes> existing;
        lock.unlock();
        const auto current_stamp = loose_stamp(loose_path);
        const bool verified_fast = remembered && current_stamp && *remembered == *current_stamp;
        if (verified_fast) {
            loose_reaffirmation_fast_paths_.fetch_add(1, std::memory_order_relaxed);
        } else {
            loose_reaffirmation_full_validations_.fetch_add(1, std::memory_order_relaxed);
            try {
                auto encoded = rf(loose_path);
                Reader r(encoded);
                auto magic = r.raw(M.size());
                if (!std::equal(magic.begin(), magic.end(), M.begin()))
                    throw std::runtime_error("bad object header");
                auto size = r.u64();
                auto nonce = r.fixed<12>();
                auto tag = r.fixed<16>();
                auto cipher = r.raw(r.remaining());
                auto plain = aes_gcm_open(key_, nonce, tag, cipher, id.bytes);
                if (plain.size() != size || object_id(plain) != id)
                    throw std::runtime_error("object integrity failure");
                existing = std::move(plain);
            } catch (...) {
                existing.reset();
            }
        }
        if (verified_fast ||
            (existing && existing->size() == data.size() &&
             std::equal(existing->begin(), existing->end(), data.begin()))) {
            std::error_code touch_error;
            std::filesystem::last_write_time(loose_path,
                                             std::filesystem::file_time_type::clock::now(),
                                             touch_error);
            const auto touched_stamp = loose_stamp(loose_path);
            lock.lock();
            if (touched_stamp)
                remember_verified_loose_locked(id, *touched_stamp);
            else
                forget_verified_loose_locked(id);
            reap_durable_generations_locked();
            uint64_t generation = 0;
            if (auto found = provisional_generations_.find(id);
                found != provisional_generations_.end())
                generation = found->second;
            if (deferred_generation) *deferred_generation = generation;
            if (durability == StoreWriteDurability::immediate && durability_domain_ &&
                generation > durability_domain_->durable_generation()) {
                lock.unlock();
                durability_domain_->await_durable(generation, DurabilityUrgency::immediate);
            }
            return true;
        }

        // Preserve the strong PUT acknowledgement contract: an externally
        // corrupted object is replaced by the supplied bytes rather than being
        // trusted merely because its content-addressed pathname exists.
        lock.lock();
        if (!remove_locked(id, lock)) {
            lock.unlock();
            const bool still_exists = std::filesystem::exists(loose_path);
            lock.lock();
            if (still_exists)
                throw std::runtime_error("cannot replace corrupt local object");
        }
    }

    if (pack_threshold_ && data.size() <= pack_threshold_)
        return put_packed_locked(id, data, durability, deferred_generation, lock);
    return put_loose_locked(id, data, durability, deferred_generation, lock);
}

std::optional<Bytes> LocalStore::get(const ObjectId& id) const {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    std::unique_lock lock(m_);
    if (packed_.contains(id))
        return get_packed_locked(id, lock);
    const auto p = path(id);
    const auto before_read = before_loose_read_for_tests_;
    lock.unlock();
    if (!std::filesystem::exists(p)) return {};
    if (before_read) before_read(id);
    auto encoded = rf(p);
    Reader r(encoded);
    auto magic = r.raw(M.size());
    if (!std::equal(magic.begin(), magic.end(), M.begin()))
        throw std::runtime_error("bad object header");
    auto size = r.u64();
    auto nonce = r.fixed<12>();
    auto tag = r.fixed<16>();
    auto cipher = r.raw(r.remaining());
    auto plain = aes_gcm_open(key_, nonce, tag, cipher, id.bytes);
    if (plain.size() != size || object_id(plain) != id)
        throw std::runtime_error("object integrity failure");
    const auto stamp = loose_stamp(p);
    if (stamp) {
        lock.lock();
        remember_verified_loose_locked(id, *stamp);
    }
    return plain;
}

bool LocalStore::has(const ObjectId& id) const noexcept {
    try {
        auto object_lock = object_mutex(id);
        std::lock_guard object_guard(*object_lock);
        {
            std::lock_guard lock(m_);
            if (packed_.contains(id) || present_loose_.contains(id))
                return true;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(path(id), error);
        const bool present = !error && size > 0;
        if (present) {
            std::lock_guard lock(m_);
            present_loose_.insert(id);
        }
        return present;
    } catch (...) {
        return false;
    }
}

bool LocalStore::valid(const ObjectId& id) const noexcept {
    try { return get(id).has_value(); } catch (...) { return false; }
}

bool LocalStore::remove_locked(const ObjectId& id, std::unique_lock<std::mutex>& lock) {
    forget_verified_loose_locked(id);
    if (auto found = packed_.find(id); found != packed_.end()) {
        if (mode_ != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
        uint64_t tomb_size = 0;
        append_pack_record_locked(pack_remove, id, {}, unix_ms(), nullptr, &tomb_size, lock);
        pack_dead_bytes_ += found->second.record_size + tomb_size;
        packed_.erase(found);
        used_.fetch_add(tomb_size, std::memory_order_relaxed);
        provisional_generations_.erase(id);
        if (mode_ != LocalStoreMode::ephemeral && durability_domain_) {
            const auto generation = durability_domain_->complete_mutation(active_pack_, packs_);
            last_mutation_generation_ = std::max(last_mutation_generation_, generation);
        }
        return true;
    }

    auto p = path(id);
    lock.unlock();
    std::error_code error;
    const auto size = std::filesystem::file_size(p, error);
    lock.lock();
    if (error) return false;
    if (mode_ != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
    lock.unlock();
    const bool removed = std::filesystem::remove(p, error);
    uint64_t generation = 0;
    if (removed && !error && mode_ != LocalStoreMode::ephemeral && durability_domain_)
        generation = durability_domain_->complete_mutation({}, p.parent_path());
    lock.lock();
    if (error || !removed) return false;
    const auto before = used_.fetch_sub(size, std::memory_order_relaxed);
    if (size > before) {
        used_.fetch_add(size, std::memory_order_relaxed);
        throw std::runtime_error("local accounting underflow");
    }
    provisional_generations_.erase(id);
    if (generation) {
        last_mutation_generation_ = std::max(last_mutation_generation_, generation);
    }
    return true;
}

bool LocalStore::put(const ObjectId& id, std::span<const uint8_t> data) {
    return put_impl(id, data, StoreWriteDurability::immediate, nullptr);
}

std::optional<uint64_t> LocalStore::put_deferred(const ObjectId& id,
                                                 std::span<const uint8_t> data) {
    uint64_t generation = 0;
    if (!put_impl(id, data, StoreWriteDurability::deferred, &generation)) return {};
    return generation;
}

void LocalStore::durability_barrier(uint64_t required_generation, DurabilityUrgency urgency) {
    if (mode_ == LocalStoreMode::ephemeral || !durability_domain_ || !required_generation) return;
    durability_domain_->await_durable(required_generation, urgency);
}

void LocalStore::durability_barrier() {
    if (mode_ == LocalStoreMode::ephemeral || !durability_domain_) return;
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_);
        generation = last_mutation_generation_;
    }
    if (generation)
        durability_domain_->await_durable(generation, DurabilityUrgency::immediate);
}

uint64_t LocalStore::durable_generation() const {
    return durability_domain_ ? durability_domain_->durable_generation() : 0;
}

uint64_t LocalStore::durability_domain_id() const noexcept {
    return durability_domain_ ? durability_domain_->id() : 0;
}

bool LocalStore::remove(const ObjectId& id) {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    std::unique_lock lock(m_);
    wait_for_accounting(lock);
    return remove_locked(id, lock);
}

bool LocalStore::remove_if_older_than(const ObjectId& id, std::chrono::milliseconds age) {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    std::unique_lock lock(m_);
    wait_for_accounting(lock);
    if (auto found = packed_.find(id); found != packed_.end()) {
        const auto now = unix_ms();
        if (now < found->second.touched_unix_ms ||
            now - found->second.touched_unix_ms < static_cast<uint64_t>(age.count()))
            return false;
        return remove_locked(id, lock);
    }
    lock.unlock();
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path(id), error);
    if (error || std::filesystem::file_time_type::clock::now() - modified < age) return false;
    lock.lock();
    return remove_locked(id, lock);
}

std::vector<ObjectId> LocalStore::list() const {
    std::vector<ObjectId> out;
    Cursor cursor;
    bool exhausted = false;
    while (!exhausted) {
        if (auto id = next_object(cursor, exhausted)) out.push_back(*id);
    }
    return out;
}

std::optional<ObjectId> LocalStore::next_object(Cursor& cursor, bool& exhausted) const {
    exhausted = false;
    if (!cursor.packed_done) {
        std::lock_guard lock(m_);
        auto it = cursor.packed_after ? packed_.upper_bound(*cursor.packed_after) : packed_.begin();
        if (it != packed_.end()) {
            cursor.packed_after = it->first;
            return it->first;
        }
        cursor.packed_done = true;
    }

    std::error_code error;
    if (!cursor.loose_initialized) {
        cursor.iterator = std::filesystem::recursive_directory_iterator(
            objects_, std::filesystem::directory_options::skip_permission_denied, error);
        cursor.loose_initialized = true;
        if (error) {
            cursor = {};
            exhausted = true;
            return {};
        }
    }
    const std::filesystem::recursive_directory_iterator end;
    while (cursor.iterator != end) {
        const auto entry = *cursor.iterator;
        cursor.iterator.increment(error);
        if (error) {
            cursor = {};
            exhausted = true;
            return {};
        }
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error) continue;
        auto name = entry.path().filename().string();
        if (name.size() != 68 || name.compare(64, 4, ".obj") != 0) continue;
        auto bytes = unhex(name.substr(0, 64));
        if (!bytes || bytes->size() != 32) continue;
        ObjectId id;
        std::copy(bytes->begin(), bytes->end(), id.bytes.begin());
        return id;
    }
    cursor = {};
    exhausted = true;
    return {};
}

std::filesystem::path LocalStore::object_path(const ObjectId& id) const {
    std::unique_lock lock(m_);
    if (auto found = packed_.find(id); found != packed_.end()) return found->second.file;
    return path(id);
}

uint64_t LocalStore::stored_size(const ObjectId& id) const {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    {
        std::lock_guard lock(m_);
        if (auto found = packed_.find(id); found != packed_.end())
            return found->second.record_size;
    }
    std::error_code error;
    auto size = std::filesystem::file_size(path(id), error);
    return error ? 0 : size;
}

std::filesystem::file_time_type LocalStore::last_write(const ObjectId& id) const {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    {
        std::lock_guard lock(m_);
        if (auto found = packed_.find(id); found != packed_.end())
            return file_time_from_unix_ms(found->second.touched_unix_ms);
    }
    std::error_code error;
    auto time = std::filesystem::last_write_time(path(id), error);
    return error ? std::filesystem::file_time_type::min() : time;
}

void LocalStore::touch(const ObjectId& id) {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    std::unique_lock lock(m_);
    if (auto found = packed_.find(id); found != packed_.end()) {
        uint64_t record_size = 0;
        const auto touched = unix_ms();
        if (mode_ != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
        append_pack_record_locked(pack_touch, id, {}, touched, nullptr, &record_size, lock);
        found->second.touched_unix_ms = touched;
        pack_dead_bytes_ += record_size;
        used_.fetch_add(record_size, std::memory_order_relaxed);
        if (mode_ != LocalStoreMode::ephemeral && durability_domain_) {
            const auto generation = durability_domain_->complete_mutation(active_pack_, packs_);
            last_mutation_generation_ = std::max(last_mutation_generation_, generation);
        }
        return;
    }
    lock.unlock();
    std::error_code error;
    std::filesystem::last_write_time(path(id), std::filesystem::file_time_type::clock::now(), error);
}

bool LocalStore::older_than(const ObjectId& id, std::chrono::milliseconds age) const {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    {
        std::lock_guard lock(m_);
        if (auto found = packed_.find(id); found != packed_.end()) {
            const auto now = unix_ms();
            return now >= found->second.touched_unix_ms &&
                   now - found->second.touched_unix_ms > static_cast<uint64_t>(age.count());
        }
    }
    std::error_code error;
    auto time = std::filesystem::last_write_time(path(id), error);
    if (error) return false;
    return std::filesystem::file_time_type::clock::now() - time > age;
}

bool LocalStore::is_packed(const ObjectId& id) const {
    auto object_lock = object_mutex(id);
    std::lock_guard object_guard(*object_lock);
    std::lock_guard lock(m_);
    return packed_.contains(id);
}

bool LocalStore::compact_packs_locked(std::unique_lock<std::mutex>& lock) {
    if (!pack_threshold_ || pack_dead_bytes_ == 0)
        return true;

    struct PackUsage {
        std::filesystem::path file;
        uint64_t size{};
        uint64_t live{};
        uint64_t dead{};
    };

    std::map<std::filesystem::path, uint64_t> live_by_pack;
    for (const auto& [_, entry] : packed_) {
        auto& live = live_by_pack[entry.file];
        if (entry.record_size > std::numeric_limits<uint64_t>::max() - live)
            throw std::runtime_error("pack compaction live-size overflow");
        live += entry.record_size;
    }
    const auto packed_snapshot = packed_;
    const auto before_compaction = before_pack_compaction_for_tests_;
    lock.unlock();
    if (before_compaction)
        before_compaction();

    std::vector<PackUsage> packs;
    uint64_t total_dead = 0;
    std::error_code enumerate_error;
    for (const auto& entry : std::filesystem::directory_iterator(packs_, enumerate_error)) {
        if (enumerate_error)
            break;
        if (!entry.is_regular_file() || !pack_sequence(entry.path().filename().string()))
            continue;
        std::error_code size_error;
        const auto size = entry.file_size(size_error);
        if (size_error)
            throw std::runtime_error("cannot size source pack: " + size_error.message());
        const auto found = live_by_pack.find(entry.path());
        const uint64_t live = found == live_by_pack.end() ? 0 : found->second;
        if (live > size)
            throw std::runtime_error("pack live accounting exceeds physical pack size");
        const uint64_t dead = size - live;
        if (dead > std::numeric_limits<uint64_t>::max() - total_dead)
            throw std::runtime_error("pack dead-size overflow");
        total_dead += dead;
        packs.push_back({entry.path(), size, live, dead});
    }
    if (enumerate_error)
        throw std::runtime_error("cannot enumerate source packs: " + enumerate_error.message());

    // The cached counter is only an admission hint. Derive the authoritative
    // value from the current index/files before selecting a victim so recovery
    // from an interrupted previous compaction cannot leave dead bytes hidden.
    auto victim = std::max_element(packs.begin(), packs.end(), [](const PackUsage& a,
                                                                  const PackUsage& b) {
        if (a.dead != b.dead)
            return a.dead < b.dead;
        return a.size < b.size;
    });
    if (victim == packs.end() || victim->dead == 0) {
        lock.lock();
        pack_dead_bytes_ = 0;
        return true;
    }

    // One maintenance invocation rewrites at most one source pack. Temporary
    // space is therefore bounded by that pack's live bytes rather than the live
    // size of the entire DATA store.
    std::error_code space_error;
    const auto space = std::filesystem::space(root_, space_error);
    if (space_error)
        throw std::runtime_error("cannot inspect free space for pack compaction: " +
                                 space_error.message());
    if (victim->live > space.available || reserve_free_ > space.available - victim->live)
        return false;

    std::vector<std::pair<ObjectId, PackEntry>> victim_entries;
    for (const auto& [id, entry] : packed_snapshot)
        if (entry.file == victim->file)
            victim_entries.push_back({id, entry});

    std::filesystem::path replacement;
    std::filesystem::path temp;
    int fd = -1;
    uint64_t replacement_size = 0;
    std::map<ObjectId, PackEntry> rebuilt;

    auto cleanup_temp = [&] {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        if (!temp.empty()) {
            std::error_code error;
            std::filesystem::remove(temp, error);
        }
    };

    try {
        if (!victim_entries.empty()) {
            lock.lock();
            const auto sequence = next_pack_sequence_++;
            lock.unlock();
            replacement = packs_ / pack_name(sequence);
            temp = packs_ / (".compact-" + std::to_string(getpid()) + "-" +
                             std::to_string(sequence) + ".tmp");
            fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0)
                throw std::runtime_error("cannot create compacted pack: " +
                                         std::string(strerror(errno)));

            for (const auto& [id, old_entry] : victim_entries) {
                if (old_entry.payload_offset < pack_header_size)
                    throw std::runtime_error("invalid packed object offset during compaction");
                const auto record_offset = old_entry.payload_offset - pack_header_size;
                int source = ::open(victim->file.c_str(), O_RDONLY);
                if (source < 0)
                    throw std::runtime_error("cannot open source pack during compaction: " +
                                             std::string(strerror(errno)));
                auto bytes = pra_exact(source, old_entry.record_size, record_offset);
                const auto saved = errno;
                ::close(source);
                if (!bytes)
                    throw std::runtime_error("cannot read live pack record during compaction: " +
                                             std::string(strerror(saved)));
                wa(fd, *bytes);

                auto next = old_entry;
                next.file = replacement;
                next.payload_offset = replacement_size + pack_header_size;
                rebuilt.emplace(id, std::move(next));
                replacement_size += old_entry.record_size;
            }

            if (::fsync(fd) != 0)
                throw std::runtime_error("cannot sync compacted pack: " +
                                         std::string(strerror(errno)));
            if (::close(fd) != 0) {
                fd = -1;
                throw std::runtime_error("cannot close compacted pack: " +
                                         std::string(strerror(errno)));
            }
            fd = -1;
            if (::rename(temp.c_str(), replacement.c_str()) != 0)
                throw std::runtime_error("cannot install compacted pack: " +
                                         std::string(strerror(errno)));
            temp.clear();
            syncdir(packs_);
        }

        lock.lock();
        if (mode_ != LocalStoreMode::ephemeral)
            ensure_accounting_dirty(lock);

        const auto before = used_.load(std::memory_order_relaxed);
        if (replacement_size > std::numeric_limits<uint64_t>::max() - before)
            throw std::runtime_error("pack compaction accounting overflow");
        used_.store(before + replacement_size, std::memory_order_relaxed);

        // The installed replacement has a strictly newer sequence than every
        // existing pack. Point the live index at it before removing the victim;
        // a failed unlink then leaves only harmless dead duplicate bytes.
        for (auto& [id, entry] : rebuilt)
            packed_[id] = std::move(entry);

        if (!replacement.empty() && replacement_size < pack_target_size_) {
            active_pack_ = replacement;
            active_pack_size_ = replacement_size;
        } else {
            active_pack_.clear();
            active_pack_size_ = 0;
        }
        lock.unlock();

        // Readers which selected the victim before the atomic index switch may
        // not have opened it yet. Wait without retaining m_; no new reader can
        // select the victim after the switch above.
        lock.lock();
        pack_readers_cv_.wait(lock, [&] {
            auto active = active_pack_readers_.find(victim->file);
            return active == active_pack_readers_.end() || active->second == 0;
        });
        lock.unlock();

        std::error_code remove_error;
        const bool removed = std::filesystem::remove(victim->file, remove_error);
        if (remove_error || !removed) {
            // All records in the old victim are now superseded by the newer
            // representation (or were already dead), so the entire old pack is
            // dead and can be retried by a later bounded compaction pass.
            Log::warn("cannot remove compacted source pack path=" + victim->file.string() +
                      (remove_error ? " error=" + remove_error.message() : ""));
        } else {
        }
        syncdir(packs_);

        uint64_t generation = 0;
        if (mode_ != LocalStoreMode::ephemeral && durability_domain_) {
            generation = durability_domain_->complete_mutation({}, packs_);
        }
        lock.lock();
        if (remove_error || !removed) {
            pack_dead_bytes_ = total_dead - victim->dead + victim->size;
        } else {
            const auto current_used = used_.fetch_sub(victim->size, std::memory_order_relaxed);
            if (victim->size > current_used) {
                used_.fetch_add(victim->size, std::memory_order_relaxed);
                throw std::runtime_error("pack compaction accounting underflow");
            }
            pack_dead_bytes_ = total_dead - victim->dead;
        }
        if (generation) {
            last_mutation_generation_ = std::max(last_mutation_generation_, generation);
        }
        return !remove_error && removed;
    } catch (...) {
        cleanup_temp();
        if (!lock.owns_lock())
            lock.lock();
        throw;
    }
}

bool LocalStore::compact_packs(std::stop_token stop) {
    std::unique_lock pack_io_lock(pack_io_mutex_);
    std::unique_lock lock(m_);
    if (!wait_for_accounting(lock, stop, true)) return false;
    return compact_packs_locked(lock);
}

void LocalStore::wait_for_accounting(std::unique_lock<std::mutex>& lock) const {
    (void)wait_for_accounting(lock, {});
}

bool LocalStore::wait_for_accounting(std::unique_lock<std::mutex>& lock,
                                     std::stop_token stop, bool exact) const {
    // Ordinary puts/removes proceed on a checkpoint estimate; only callers
    // that need the reconciled figure (compaction) wait for the walk.
    if (!exact && accounting_estimate_.load(std::memory_order_acquire) &&
        !scan_failed_.load(std::memory_order_acquire))
        return true;
    std::stop_callback wake_waiter(stop, [this] { accounting_cv_.notify_all(); });
    accounting_cv_.wait(lock, [this, stop] {
        return scan_complete_.load(std::memory_order_acquire) ||
               scan_failed_.load(std::memory_order_acquire) || stop.stop_requested();
    });
    if (stop.stop_requested()) return false;
    if (scan_failed_.load(std::memory_order_acquire))
        throw std::runtime_error("storage accounting reconciliation failed");
    return true;
}

namespace {
// "<64 hex>.obj" -> id; nullopt for anything else in the object tree.
std::optional<ObjectId> object_id_from_file_name(std::string_view name) {
    if (name.size() != 64 + 4 || !name.ends_with(".obj")) return std::nullopt;
    ObjectId id;
    for (size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (size_t nibble = 0; nibble < 2; ++nibble) {
            const char c = name[i * 2 + nibble];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
            value = (value << 4) | digit;
        }
        id.bytes[i] = static_cast<uint8_t>(value);
    }
    return id;
}
} // namespace

void LocalStore::warm_presence_index(std::stop_token stop) {
    const auto started = Clock::now();
    std::error_code error;
    std::vector<ObjectId> batch;
    batch.reserve(4096);
    uint64_t entries = 0;
    const auto flush = [&] {
        if (batch.empty()) return;
        std::lock_guard lock(m_);
        for (const auto& id : batch) present_loose_.insert(id);
        entries += batch.size();
        batch.clear();
    };
    // Directory names only: readdir supplies the file type, so nothing here
    // stats an object. The scan (when it runs) does the same as it walks.
    for (auto it = std::filesystem::recursive_directory_iterator(objects_, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (stop.stop_requested()) return;
        if (it.depth() < 2) continue;
        if (auto id = object_id_from_file_name(it->path().filename().string())) {
            batch.push_back(*id);
            if (batch.size() >= 4096) flush();
        }
    }
    flush();
    presence_index_entries_.store(entries, std::memory_order_relaxed);
    Log::debug("storage presence index warmed path=" + root_.string() +
               " objects=" + std::to_string(entries) + " elapsed_ms=" +
               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  Clock::now() - started).count()) +
               (error ? " error=" + error.message() : std::string{}));
}

void LocalStore::scan(std::stop_token stop) {
    Log::debug("storage accounting scan begin path=" + root_.string());
    uint64_t total = 0;
    std::error_code error;
    for (const auto& root : {objects_, packs_}) {
        for (auto it = std::filesystem::recursive_directory_iterator(root, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (stop.stop_requested()) return;
            note_startup_progress(); // an 80 s scan of a big store is progress, not a stall.
            if (!it->is_regular_file()) continue;
            auto name = it->path().filename().string();
            if (name.find(".tmp") != std::string::npos || name.starts_with(".compact-")) {
                std::error_code remove_error;
                std::filesystem::remove(it->path(), remove_error);
                continue;
            }
            std::error_code size_error;
            const auto size = it->file_size(size_error);
            if (!size_error) total += size;
            if (root == objects_ && size > 0) {
                if (auto id = object_id_from_file_name(name)) {
                    std::lock_guard lock(m_);
                    present_loose_.insert(*id);
                }
            }
        }
        if (error) break;
    }
    if (error) {
        scan_failed_.store(true, std::memory_order_release);
        accounting_cv_.notify_all();
        Log::warn("storage accounting scan failed path=" + root_.string() +
                  " error=" + error.message());
        return;
    }
    if (mode_ == LocalStoreMode::authoritative && durability_domain_) {
        try {
            const auto baseline = durability_domain_->complete_mutation();
            durability_domain_->await_durable(baseline, DurabilityUrgency::immediate);
            {
                std::lock_guard lock(m_);
                last_mutation_generation_ = std::max(last_mutation_generation_, baseline);
            }
        } catch (const std::exception& ex) {
            scan_failed_.store(true, std::memory_order_release);
            accounting_cv_.notify_all();
            Log::warn("storage accounting baseline durability failed path=" + root_.string() +
                      " error=" + ex.what());
            return;
        }
    }
    {
        std::lock_guard lock(m_);
        // Writes admitted against the estimate during the walk may or may
        // not have been seen by it; keep the larger figure (over-counting is
        // the safe direction, and bounded by what was written meanwhile).
        const auto running = used_.load(std::memory_order_relaxed);
        used_.store(accounting_estimate_.load(std::memory_order_acquire) ? std::max(total, running)
                                                                          : total,
                    std::memory_order_relaxed);
        accounting_estimate_.store(false, std::memory_order_release);
        accounting_trusted_.store(true, std::memory_order_release);
        if (mode_ != LocalStoreMode::ephemeral) {
            accounting_dirty_ = true;
            checkpoint_accounting_locked();
        }
    }
    scan_complete_.store(true, std::memory_order_release);
    accounting_cv_.notify_all();
    Log::debug("storage accounting scan complete path=" + root_.string() +
               " used=" + std::to_string(total));
}

NodeId load_or_create_node_id(const std::filesystem::path& state) {
    std::filesystem::create_directories(state);
    auto file = state / "node-id";
    if (std::filesystem::exists(file)) {
        std::ifstream in(file);
        std::string text;
        in >> text;
        auto bytes = unhex(text);
        if (!bytes || bytes->size() != 16)
            throw std::runtime_error("invalid node id");
        NodeId id;
        std::copy(bytes->begin(), bytes->end(), id.bytes.begin());
        return id;
    }
    auto id = random_node_id();
    auto tmp = file.string() + ".tmp";
    {
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) throw std::runtime_error(strerror(errno));
        auto text = to_string(id) + "\n";
        wa(fd, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        (void)fsync(fd);
        close(fd);
    }
    if (::rename(tmp.c_str(), file.c_str()) != 0)
        throw std::runtime_error(strerror(errno));
    syncdir(state);
    return id;
}
} // namespace macha
