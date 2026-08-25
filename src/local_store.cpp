// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_store.hpp"
#include "codec.hpp"
#include "log.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> M{'D', 'H', 'T', 'O', 'B', 'J', '0', '1'};
constexpr std::array<uint8_t, 8> A{'M', 'A', 'C', 'H', 'A', 'A', 'C', '1'};
constexpr size_t accounting_slot_size = 128;
constexpr size_t accounting_body_size = accounting_slot_size - 32;
constexpr uint8_t accounting_none = 0;
constexpr uint8_t accounting_put = 1;
constexpr uint8_t accounting_remove = 2;
constexpr uint8_t accounting_dirty = 3;

struct AccountingRecord {
    uint64_t sequence{};
    uint64_t used{};
    uint8_t operation{};
    ObjectId id{};
    uint64_t size{};
};

void pwa_exact(int fd, std::span<const uint8_t> data, uint64_t offset) {
    size_t done = 0;
    while (done < data.size()) {
        auto n = ::pwrite(fd, data.data() + done, data.size() - done,
                          static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("cannot write accounting state: " + std::string(strerror(errno)));
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
            if (errno == EINTR)
                continue;
            return {};
        }
        if (n == 0)
            return {};
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
    while (body.data().size() < accounting_body_size)
        body.u8(0);
    if (body.data().size() != accounting_body_size)
        throw std::runtime_error("internal accounting record size error");
    auto hash = sha256(body.data());
    Writer out;
    out.raw(body.data());
    out.fixed(hash.bytes);
    return out.take();
}

std::optional<AccountingRecord> decode_accounting(std::span<const uint8_t> bytes) {
    if (bytes.size() != accounting_slot_size)
        return {};
    auto body = bytes.first(accounting_body_size);
    auto expected = sha256(body);
    if (!std::equal(expected.bytes.begin(), expected.bytes.end(),
                    bytes.begin() + accounting_body_size))
        return {};
    try {
        Reader reader(body);
        auto magic = reader.raw(A.size());
        if (!std::equal(magic.begin(), magic.end(), A.begin()))
            return {};
        AccountingRecord record;
        record.sequence = reader.u64();
        record.used = reader.u64();
        record.operation = reader.u8();
        record.size = reader.u64();
        record.id.bytes = reader.fixed<32>();
        if (record.operation > accounting_dirty)
            return {};
        return record;
    } catch (...) {
        return {};
    }
}
void wa(int f, std::span<const uint8_t> b) {
    size_t p = 0;
    while (p < b.size()) {
        auto n = ::write(f, b.data() + p, b.size() - p);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(strerror(errno));
        }
        p += n;
    }
}
Bytes rf(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("cannot open object: " + std::string(strerror(errno)));

    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        auto error = std::string(strerror(errno));
        close(fd);
        throw std::runtime_error("cannot stat object: " + error);
    }

    Bytes out(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < out.size()) {
        auto n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            auto error = std::string(strerror(errno));
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
    int f = ::open(p.c_str(), O_RDONLY | O_DIRECTORY);
    if (f >= 0) {
        (void)fsync(f);
        close(f);
    }
}

#if !defined(__linux__)
void sync_file(const std::filesystem::path& p) {
    int f = ::open(p.c_str(), O_RDONLY);
    if (f < 0)
        throw std::runtime_error("cannot open deferred object for sync: " +
                                 std::string(strerror(errno)));
    int rc;
    do { rc = ::fsync(f); } while (rc != 0 && errno == EINTR);
    const int saved = errno;
    ::close(f);
    if (rc != 0)
        throw std::runtime_error("cannot sync deferred object: " + std::string(strerror(saved)));
}
#endif
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

LocalStore::LocalStore(std::filesystem::path r, uint64_t l, std::array<uint8_t, 32> k,
                       LocalStoreMode mode)
    : root_(std::move(r)), objects_(root_ / "objects"),
      accounting_path_(root_ / ".macha.accounting"), limit_(l), key_(k), mode_(mode) {
    std::filesystem::create_directories(objects_);
    if (mode_ == LocalStoreMode::ephemeral) {
        // Cache contents are explicitly disposable. Reconcile their byte count
        // from the directory tree and never create a false durability contract
        // through the authoritative accounting journal.
        scan_thread_ = std::jthread([this](std::stop_token stop) { scan(stop); });
        return;
    }

    accounting_fd_ = ::open(accounting_path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (accounting_fd_ < 0)
        throw std::runtime_error("cannot open accounting state: " + std::string(strerror(errno)));
    if (restore_accounting()) {
        accounting_trusted_.store(true, std::memory_order_release);
        scan_complete_.store(true, std::memory_order_release);
        Log::debug("storage accounting restored path=" + root_.string() +
                   " used=" + std::to_string(used_.load(std::memory_order_relaxed)));
    } else {
        scan_thread_ = std::jthread([this](std::stop_token stop) { scan(stop); });
    }
}

LocalStore::~LocalStore() {
    if (scan_thread_.joinable()) {
        scan_thread_.request_stop();
        scan_thread_.join();
    }
    if (accounting_fd_ >= 0) {
        try {
            durability_barrier();
        } catch (...) {
            // Destructors cannot report a failed final checkpoint. A durable
            // dirty marker remains behind, forcing exact tree reconciliation
            // rather than trusting stale accounting on the next start.
        }
        if (accounting_trusted_.load(std::memory_order_acquire) && !accounting_dirty_) {
            try {
                ObjectId none{};
                persist_accounting(used_.load(std::memory_order_relaxed), accounting_none, none, 0, true);
            } catch (...) {
            }
        }
        close(accounting_fd_);
        accounting_fd_ = -1;
    }
}
std::filesystem::path LocalStore::path(const ObjectId& i) const {
    auto s = to_string(i);
    return objects_ / s.substr(0, 2) / s.substr(2, 2) / (s + ".obj");
}

void LocalStore::persist_accounting(uint64_t used, uint8_t operation, const ObjectId& id,
                                    uint64_t size, bool durable) {
    AccountingRecord record;
    record.sequence = ++accounting_sequence_;
    record.used = used;
    record.operation = operation;
    record.id = id;
    record.size = size;
    accounting_slot_ ^= 1U;
    auto encoded = encode_accounting(record);
    pwa_exact(accounting_fd_, encoded, accounting_slot_ * accounting_slot_size);
    if (durable && fsync(accounting_fd_) != 0)
        throw std::runtime_error("cannot sync accounting state: " + std::string(strerror(errno)));
}

bool LocalStore::restore_accounting() {
    std::optional<AccountingRecord> best;
    unsigned best_slot = 0;
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto bytes = pra_exact(accounting_fd_, accounting_slot_size,
                               slot * accounting_slot_size);
        if (!bytes)
            continue;
        auto decoded = decode_accounting(*bytes);
        if (!decoded)
            continue;
        if (!best || decoded->sequence > best->sequence) {
            best = *decoded;
            best_slot = slot;
        }
    }
    if (!best)
        return false;

    accounting_sequence_ = best->sequence;
    accounting_slot_ = best_slot;
    if (best->operation == accounting_dirty) {
        // A deferred publication generation began but never installed a clean
        // accounting checkpoint. Object data may be wholly durable, partially
        // durable, or absent; the object tree is authoritative and is rebuilt
        // in the background before new capacity mutations are admitted.
        return false;
    }
    uint64_t restored = best->used;
    if (best->operation == accounting_put) {
        if (std::filesystem::exists(path(best->id))) {
            if (std::numeric_limits<uint64_t>::max() - restored < best->size)
                return false;
            restored += best->size;
        }
    } else if (best->operation == accounting_remove) {
        if (!std::filesystem::exists(path(best->id))) {
            if (best->size > restored)
                return false;
            restored -= best->size;
        }
    }
    used_.store(restored, std::memory_order_relaxed);
    if (best->operation != accounting_none) {
        ObjectId none{};
        persist_accounting(restored, accounting_none, none, 0, true);
    }
    return true;
}

void LocalStore::mark_accounting_dirty_locked() {
    if (mode_ == LocalStoreMode::ephemeral || accounting_dirty_)
        return;
    ObjectId none{};
    persist_accounting(used_.load(std::memory_order_relaxed), accounting_dirty, none, 0, true);
    accounting_dirty_ = true;
}

void LocalStore::checkpoint_accounting_locked() {
    if (mode_ == LocalStoreMode::ephemeral || !accounting_dirty_)
        return;
    ObjectId none{};
    persist_accounting(used_.load(std::memory_order_relaxed), accounting_none, none, 0, true);
    accounting_dirty_ = false;
    accounting_trusted_.store(true, std::memory_order_release);
}

bool LocalStore::put_impl(const ObjectId& i, std::span<const uint8_t> d,
                          StoreWriteDurability durability, uint64_t* deferred_generation) {
    if (object_id(d) != i)
        throw std::runtime_error("object hash mismatch");
    auto p = path(i);
    // Existing-object reaffirmation participates in the same mutation lock as
    // age-conditional GC removal. This closes the check-age/remove race when a
    // new write reuses an old content hash while maintenance is sweeping it.
    std::unique_lock g(m_);
    if (std::filesystem::exists(p)) {
        try {
            auto existing = get(i);
            if (existing && existing->size() == d.size() &&
                std::equal(existing->begin(), existing->end(), d.begin())) {
                // An immediate caller may encounter an object installed by a
                // concurrent WAL-backed generation which has not crossed its
                // stable-storage barrier yet. Preserve the historical strict
                // put() contract by making that generation durable before
                // reaffirming the object to the immediate caller.
                if (mode_ == LocalStoreMode::authoritative &&
                    durability == StoreWriteDurability::immediate && accounting_dirty_)
                    durability_barrier_locked(mutation_generation_);
                if (deferred_generation)
                    *deferred_generation = accounting_dirty_ ? mutation_generation_
                                                               : durable_generation_;
                touch(i);
                return true;
            }
        } catch (...) {
            // A pathname is not a valid replica. Fall through to the mutation
            // path and atomically replace it with the caller's known-good bytes.
        }
    }
    wait_for_accounting(g);
    if (std::filesystem::exists(p)) {
        try {
            auto existing = get(i);
            if (existing && existing->size() == d.size() &&
                std::equal(existing->begin(), existing->end(), d.begin())) {
                // An immediate caller may encounter an object installed by a
                // concurrent WAL-backed generation which has not crossed its
                // stable-storage barrier yet. Preserve the historical strict
                // put() contract by making that generation durable before
                // reaffirming the object to the immediate caller.
                if (mode_ == LocalStoreMode::authoritative &&
                    durability == StoreWriteDurability::immediate && accounting_dirty_)
                    durability_barrier_locked(mutation_generation_);
                if (deferred_generation)
                    *deferred_generation = accounting_dirty_ ? mutation_generation_
                                                               : durable_generation_;
                touch(i);
                return true;
            }
        } catch (...) {
        }
        if (!remove_locked(i, durability) && std::filesystem::exists(p))
            throw std::runtime_error("cannot replace corrupt local object");
    }
    auto s = aes_gcm_seal(key_, d, i.bytes);
    Writer h;
    h.raw(M);
    h.u64(d.size());
    h.fixed(s.nonce);
    h.fixed(s.tag);
    uint64_t need = h.data().size() + s.ciphertext.size();
    if (used_.load(std::memory_order_relaxed) + need > limit_)
        return false;
    const auto before = used_.load(std::memory_order_relaxed);
    const bool ephemeral = mode_ == LocalStoreMode::ephemeral;
    const bool deferred = !ephemeral && durability == StoreWriteDurability::deferred;
    const bool transactional_accounting = !ephemeral && !deferred && !accounting_dirty_;

    if (deferred) {
        // The spool/journal is the WAL for this publication generation. Persist
        // one DIRTY accounting marker before the first provisional object, then
        // allow the OS to batch all object data and namespace changes until the
        // publication barrier.
        mark_accounting_dirty_locked();
    } else if (transactional_accounting) {
        // Ordinary authoritative puts retain the historical immediate contract:
        // a successful return means this individual object and its pathname are
        // stable, while the small pending record keeps accounting O(1) recoverable.
        persist_accounting(before, accounting_put, i, need, true);
    }

    std::filesystem::create_directories(p.parent_path());
    auto t = p.string() + ".tmp." + std::to_string(getpid()) + "." + std::to_string(unix_ms());
    int f = ::open(t.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (f < 0)
        throw std::runtime_error(strerror(errno));
    try {
        wa(f, h.data());
        wa(f, s.ciphertext);
        if (!ephemeral && !deferred && fsync(f))
            throw std::runtime_error(strerror(errno));
        if (close(f))
            throw std::runtime_error(strerror(errno));
        f = -1;
        if (rename(t.c_str(), p.c_str()))
            throw std::runtime_error(strerror(errno));
        if (!ephemeral && !deferred)
            syncdir(p.parent_path());
#if !defined(__linux__)
        if (deferred) {
            deferred_files_.push_back(p);
            deferred_directories_.push_back(p.parent_path());
        }
#endif
        used_.store(before + need, std::memory_order_relaxed);
        if (transactional_accounting) {
            ObjectId none{};
            // The next mutation's durable intent also flushes this clean checkpoint;
            // destructor performs a final durable sync. The older slot retains the
            // durable pending record until then, so crash recovery remains O(1).
            persist_accounting(before + need, accounting_none, none, 0, false);
        }
        if (deferred) {
            ++mutation_generation_;
            if (deferred_generation)
                *deferred_generation = mutation_generation_;
        }
        return true;
    } catch (...) {
        if (f >= 0)
            close(f);
        std::error_code e;
        std::filesystem::remove(t, e);
        if (transactional_accounting && !std::filesystem::exists(p)) {
            ObjectId none{};
            try { persist_accounting(before, accounting_none, none, 0, true); } catch (...) {}
        }
        throw;
    }
}
std::optional<Bytes> LocalStore::get(const ObjectId& i) const {
    auto p = path(i);
    if (!std::filesystem::exists(p))
        return {};
    auto e = rf(p);
    Reader r(e);
    auto m = r.raw(8);
    if (!std::equal(m.begin(), m.end(), M.begin()))
        throw std::runtime_error("bad object header");
    auto sz = r.u64();
    auto n = r.fixed<12>();
    auto t = r.fixed<16>();
    auto c = r.raw(r.remaining());
    auto d = aes_gcm_open(key_, n, t, c, i.bytes);
    if (d.size() != sz || object_id(d) != i)
        throw std::runtime_error("object integrity failure");
    return d;
}
bool LocalStore::has(const ObjectId& i) const {
    return std::filesystem::exists(path(i));
}
bool LocalStore::valid(const ObjectId& i) const noexcept {
    try {
        return get(i).has_value();
    } catch (...) {
        return false;
    }
}
bool LocalStore::remove_locked(const ObjectId& i, StoreWriteDurability durability) {
    auto p = path(i);
    std::error_code e;
    auto n = std::filesystem::file_size(p, e);
    if (e)
        return false;
    const auto before = used_.load(std::memory_order_relaxed);
    if (n > before)
        throw std::runtime_error("local accounting underflow");

    const bool ephemeral = mode_ == LocalStoreMode::ephemeral;
    const bool deferred = !ephemeral && durability == StoreWriteDurability::deferred;
    const bool transactional_accounting = !ephemeral && !deferred && !accounting_dirty_;
    if (deferred)
        mark_accounting_dirty_locked();
    else if (transactional_accounting)
        persist_accounting(before, accounting_remove, i, n, true);

    if (!std::filesystem::remove(p, e)) {
        if (transactional_accounting) {
            ObjectId none{};
            persist_accounting(before, accounting_none, none, 0, true);
        }
        return false;
    }
    if (!ephemeral && !deferred)
        syncdir(p.parent_path());
#if !defined(__linux__)
    if (deferred)
        deferred_directories_.push_back(p.parent_path());
#endif
    used_.store(before - n, std::memory_order_relaxed);
    if (transactional_accounting) {
        ObjectId none{};
        persist_accounting(before - n, accounting_none, none, 0, false);
    }
    return true;
}

void LocalStore::durability_barrier_locked(uint64_t required_generation) {
    if (mode_ == LocalStoreMode::ephemeral || accounting_fd_ < 0 ||
        required_generation <= durable_generation_)
        return;
    if (required_generation > mutation_generation_)
        throw std::runtime_error("storage durability generation was never admitted");
    if (!accounting_dirty_)
        throw std::runtime_error("storage durability generation accounting state is inconsistent");

#if defined(__linux__)
    int rc;
    do { rc = ::syncfs(accounting_fd_); } while (rc != 0 && errno == EINTR);
    if (rc != 0)
        throw std::runtime_error("cannot sync deferred storage generation: " +
                                 std::string(strerror(errno)));
#else
    for (const auto& file : deferred_files_)
        sync_file(file);
    std::sort(deferred_directories_.begin(), deferred_directories_.end());
    deferred_directories_.erase(
        std::unique(deferred_directories_.begin(), deferred_directories_.end()),
        deferred_directories_.end());
    for (const auto& directory : deferred_directories_)
        syncdir(directory);
#endif

    // syncfs/fsync above is a cut through every deferred mutation admitted while
    // the store lock is held, not merely through the caller's requested point.
    // Remember the complete cut so later barriers for older generations are
    // immediate even after newer writes make accounting dirty again.
    const auto cut = mutation_generation_;
    checkpoint_accounting_locked();
    durable_generation_ = cut;
#if !defined(__linux__)
    deferred_files_.clear();
    deferred_directories_.clear();
#endif
}

bool LocalStore::put(const ObjectId& id, std::span<const uint8_t> data,
                     StoreWriteDurability durability) {
    return put_impl(id, data, durability, nullptr);
}

std::optional<uint64_t> LocalStore::put_deferred(const ObjectId& id,
                                                 std::span<const uint8_t> data) {
    uint64_t generation = 0;
    if (!put_impl(id, data, StoreWriteDurability::deferred, &generation))
        return {};
    return generation;
}

void LocalStore::durability_barrier(uint64_t required_generation) {
    if (mode_ == LocalStoreMode::ephemeral || accounting_fd_ < 0)
        return;
    std::unique_lock g(m_);
    wait_for_accounting(g);
    durability_barrier_locked(required_generation);
}

void LocalStore::durability_barrier() {
    if (mode_ == LocalStoreMode::ephemeral || accounting_fd_ < 0)
        return;
    std::unique_lock g(m_);
    wait_for_accounting(g);
    durability_barrier_locked(mutation_generation_);
}

uint64_t LocalStore::durable_generation() const {
    std::lock_guard g(m_);
    return durable_generation_;
}

bool LocalStore::remove(const ObjectId& i) {
    std::unique_lock g(m_);
    wait_for_accounting(g);
    return remove_locked(i, StoreWriteDurability::immediate);
}

bool LocalStore::remove_if_older_than(const ObjectId& i, std::chrono::milliseconds age) {
    std::unique_lock g(m_);
    wait_for_accounting(g);
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path(i), error);
    if (error || std::filesystem::file_time_type::clock::now() - modified < age)
        return false;
    return remove_locked(i, StoreWriteDurability::immediate);
}
std::vector<ObjectId> LocalStore::list() const {
    std::vector<ObjectId> out;
    Cursor cursor;
    bool exhausted = false;
    while (!exhausted) {
        if (auto id = next_object(cursor, exhausted))
            out.push_back(*id);
    }
    return out;
}

std::optional<ObjectId> LocalStore::next_object(Cursor& cursor, bool& exhausted) const {
    exhausted = false;
    std::error_code error;
    if (!cursor.initialized) {
        cursor.iterator = std::filesystem::recursive_directory_iterator(
            objects_, std::filesystem::directory_options::skip_permission_denied, error);
        cursor.initialized = true;
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
            // A disk disappearing or a directory being removed while maintenance
            // is walking it ends this pass. refresh() will independently update
            // backend state; the next pass starts from the root again.
            cursor = {};
            exhausted = true;
            return {};
        }
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error)
            continue;
        auto name = entry.path().filename().string();
        if (name.size() != 68 || name.compare(64, 4, ".obj") != 0)
            continue;
        auto bytes = unhex(name.substr(0, 64));
        if (!bytes || bytes->size() != 32)
            continue;
        ObjectId id;
        std::copy(bytes->begin(), bytes->end(), id.bytes.begin());
        return id;
    }

    cursor = {};
    exhausted = true;
    return {};
}
std::filesystem::path LocalStore::object_path(const ObjectId& i) const {
    return path(i);
}
uint64_t LocalStore::stored_size(const ObjectId& i) const {
    std::error_code e;
    auto n = std::filesystem::file_size(path(i), e);
    return e ? 0 : n;
}
std::filesystem::file_time_type LocalStore::last_write(const ObjectId& i) const {
    std::error_code e;
    auto t = std::filesystem::last_write_time(path(i), e);
    return e ? std::filesystem::file_time_type::min() : t;
}
void LocalStore::touch(const ObjectId& i) {
    std::error_code e;
    std::filesystem::last_write_time(path(i), std::filesystem::file_time_type::clock::now(), e);
}
bool LocalStore::older_than(const ObjectId& i, std::chrono::milliseconds age) const {
    std::error_code e;
    auto t = std::filesystem::last_write_time(path(i), e);
    if (e)
        return false;
    return std::filesystem::file_time_type::clock::now() - t > age;
}
void LocalStore::wait_for_accounting(std::unique_lock<std::mutex>& lock) const {
    // Only mutations need exact quota accounting. Reads, health and maintenance
    // enumeration never wait for the startup reconciliation. Avoid a condition
    // variable tied to object lifetime: the scan owns no caller lock and this
    // path is used only during the short first-start reconciliation window.
    while (!scan_complete_.load(std::memory_order_acquire)) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lock.lock();
    }
}

void LocalStore::scan(std::stop_token stop) {
    // Capacity accounting is intentionally reconciled in the background. The
    // old constructor walked the entire object tree before the backend could be
    // declared online, which made startup proportional to store size. Mutating
    // operations wait for this one initial reconciliation; reads remain
    // available immediately.
    Log::debug("storage accounting scan begin path=" + root_.string());
    uint64_t n = 0;
    std::error_code e;
    for (auto it = std::filesystem::recursive_directory_iterator(objects_, e);
         !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
        if (stop.stop_requested())
            break;
        const auto& x = *it;
        if (!x.is_regular_file())
            continue;
        auto name = x.path().filename().string();
        if (name.find(".tmp.") != std::string::npos) {
            std::error_code r;
            std::filesystem::remove(x.path(), r);
            continue;
        }
        std::error_code size_error;
        auto size = x.file_size(size_error);
        if (!size_error)
            n += size;
    }
    if (stop.stop_requested())
        return;
    {
        std::lock_guard guard(m_);
        used_.store(n, std::memory_order_relaxed);
        if (mode_ == LocalStoreMode::authoritative) {
            ObjectId none{};
            persist_accounting(n, accounting_none, none, 0, true);
            accounting_trusted_.store(true, std::memory_order_release);
            accounting_dirty_ = false;
        }
        scan_complete_.store(true, std::memory_order_release);
    }
    Log::debug("storage accounting scan complete path=" + root_.string() +
               " used=" + std::to_string(n));
}
NodeId load_or_create_node_id(const std::filesystem::path& r) {
    std::filesystem::create_directories(r);
    auto p = r / "node.id";
    if (std::filesystem::exists(p)) {
        std::ifstream f(p);
        std::string s;
        f >> s;
        auto b = unhex(s);
        if (!b || b->size() != 16)
            throw std::runtime_error("invalid node.id");
        NodeId i;
        std::copy(b->begin(), b->end(), i.bytes.begin());
        return i;
    }
    auto i = random_node_id();
    auto t = p.string() + ".tmp." + std::to_string(getpid());
    auto encoded = to_string(i) + "\n";
    int fd = ::open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create node.id: " + std::string(strerror(errno)));
    try {
        wa(fd, {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()});
        if (fsync(fd))
            throw std::runtime_error("cannot sync node.id: " + std::string(strerror(errno)));
        if (close(fd))
            throw std::runtime_error("cannot close node.id: " + std::string(strerror(errno)));
        fd = -1;
        if (::rename(t.c_str(), p.c_str()))
            throw std::runtime_error("cannot install node.id: " + std::string(strerror(errno)));
        syncdir(p.parent_path());
    } catch (...) {
        if (fd >= 0)
            close(fd);
        std::error_code error;
        std::filesystem::remove(t, error);
        throw;
    }
    return i;
}
} // namespace macha
