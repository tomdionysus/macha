// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_frontend.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "log.hpp"
#include "macos_unicode.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace macha {
namespace {

std::string canonical_path(std::string_view path) {
    return macos_fuse_composed_name(normalize_path(std::string(path)));
}

bool under_path(std::string_view path, std::string_view parent) {
    if (path == parent)
        return true;
    return path.size() > parent.size() && path.starts_with(parent) &&
           (parent == "/" || path[parent.size()] == '/');
}

void check_deadline(Clock::time_point deadline, const std::atomic_bool& cancelled) {
    if (cancelled.load(std::memory_order_relaxed) || Clock::now() >= deadline)
        throw FsError(ETIMEDOUT, "FUSE request deadline exceeded");
}

size_t pread_exact(int fd, std::span<uint8_t> out, uint64_t offset) {
    size_t done = 0;
    while (done < out.size()) {
        auto n = ::pread(fd, out.data() + done, out.size() - done,
                         static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += static_cast<size_t>(n);
    }
    return done;
}

size_t pwrite_exact(int fd, std::span<const uint8_t> data, uint64_t offset) {
    size_t done = 0;
    while (done < data.size()) {
        auto n = ::pwrite(fd, data.data() + done, data.size() - done,
                          static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += static_cast<size_t>(n);
    }
    return done;
}

void write_exact(int fd, std::span<const uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw FsError(errno ? errno : EIO, "short FUSE journal write");
        done += static_cast<size_t>(n);
    }
}

void fsync_fd(int fd, const char* what) {
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    if (rc != 0)
        throw FsError(errno, what);
}

void sync_directory(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        throw FsError(errno, "cannot open FUSE spool directory for sync");
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    const int saved = errno;
    ::close(fd);
    if (rc != 0)
        throw FsError(saved, "cannot sync FUSE spool directory");
}

void encode_fuse_entry(Writer& writer, const FsEntry& entry) {
    writer.u8(static_cast<uint8_t>(entry.type));
    writer.u32(entry.mode);
    writer.u32(entry.uid);
    writer.u32(entry.gid);
    writer.u64(entry.size);
    writer.i64(entry.ctime_ns);
    writer.i64(entry.mtime_ns);
    writer.u64(entry.version);
    if (entry.extents.size() > UINT32_MAX)
        throw FsError(EFBIG, "too many extents for FUSE journal");
    writer.u32(static_cast<uint32_t>(entry.extents.size()));
    for (const auto& extent : entry.extents) {
        writer.u64(extent.offset);
        writer.u64(extent.length);
        writer.fixed(extent.id.bytes);
        writer.u8(extent.hole ? 1 : 0);
    }
}

FsEntry decode_fuse_entry(Reader& reader) {
    FsEntry entry;
    const auto type = reader.u8();
    if (type != static_cast<uint8_t>(EntryType::directory) &&
        type != static_cast<uint8_t>(EntryType::file))
        throw DecodeError("invalid FUSE journal entry type");
    entry.type = static_cast<EntryType>(type);
    entry.mode = reader.u32();
    entry.uid = reader.u32();
    entry.gid = reader.u32();
    entry.size = reader.u64();
    entry.ctime_ns = reader.i64();
    entry.mtime_ns = reader.i64();
    entry.version = reader.u64();
    const auto count = reader.u32();
    if (count > 4U * 1024U * 1024U)
        throw DecodeError("FUSE journal extent count too large");
    entry.extents.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ExtentRef extent;
        extent.offset = reader.u64();
        extent.length = reader.u64();
        extent.id.bytes = reader.fixed<32>();
        const auto hole = reader.u8();
        if (hole > 1)
            throw DecodeError("invalid FUSE journal hole flag");
        extent.hole = hole != 0;
        entry.extents.push_back(extent);
    }
    return entry;
}

bool same_file_content(const FsEntry& a, const FsEntry& b) {
    return a.type == b.type && a.size == b.size && a.extents == b.extents;
}

void merge_range(std::vector<FuseDirtyRange>& ranges, uint64_t offset, uint64_t length) {
    if (!length)
        return;
    uint64_t begin = offset;
    uint64_t end = offset + length;
    auto it = ranges.begin();
    while (it != ranges.end() && it->offset + it->length < begin)
        ++it;
    while (it != ranges.end() && it->offset <= end) {
        begin = std::min(begin, it->offset);
        end = std::max(end, it->offset + it->length);
        it = ranges.erase(it);
    }
    ranges.insert(it, FuseDirtyRange{begin, end - begin});
}

void clip_ranges(std::vector<FuseDirtyRange>& ranges, uint64_t size) {
    for (auto it = ranges.begin(); it != ranges.end();) {
        if (it->offset >= size) {
            it = ranges.erase(it);
            continue;
        }
        if (it->offset + it->length > size)
            it->length = size - it->offset;
        ++it;
    }
}

bool retryable_backend_error(const std::exception& error) {
    if (const auto* fs = dynamic_cast<const FsError*>(&error)) {
        switch (fs->code()) {
        case EAGAIN:
        case EIO:
        case EINTR:
        case ETIMEDOUT:
        case ENETDOWN:
        case ENETUNREACH:
        case ECONNRESET:
        case ECONNABORTED:
        case ENOTCONN:
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case EPIPE:
        case EBUSY:
            return true;
        default:
            return false;
        }
    }
    return true;
}

FuseEntryAttributes fuse_attributes(const FsEntry& entry) {
    return FuseEntryAttributes{entry.type, entry.mode, entry.uid, entry.gid, entry.size,
                               entry.ctime_ns, entry.mtime_ns, entry.version};
}

} // namespace

class FuseReadSession {
  public:
    std::mutex mutex;
    uint64_t inode{};
    uint64_t base_version{};
    std::shared_ptr<ReadHandle> reader;
};

class ScopedFd {
    int fd_{-1};

  public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ~ScopedFd() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    int get() const noexcept { return fd_; }

    void reset(int fd = -1) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
};

struct FuseFrontend::State {
    struct DataOp {
        enum class Kind : uint8_t { write, truncate };
        Kind kind{Kind::write};
        uint64_t sequence{};
        uint64_t offset{};
        uint64_t length{};
        uint64_t spool_offset{};
        uint64_t size{};
        int64_t mtime_ns{};
        int64_t ctime_ns{};
    };

    struct Inode {
        mutable std::mutex mutex;
        uint64_t id{};
        FsEntry base;
        FsEntry visible;
        std::string current_path;
        std::optional<std::string> published_path;
        uint64_t namespace_sequence{};
        uint64_t next_data_sequence{1};
        uint64_t requested_data_sequence{};
        // Highest write/truncate sequence whose spool bytes and journal record
        // have completed the local durability barrier. data_ops may also contain
        // newer POSIX-buffered writes which are visible through this node but
        // are not eligible for distributed publication yet.
        uint64_t durable_data_sequence{};
        uint64_t published_data_sequence{};
        uint64_t requested_namespace_sequence{};
        std::vector<DataOp> data_ops;
        int spool_fd{-1};
        std::filesystem::path spool_path;
        uint64_t spool_end{};
        // Writes admitted to the local spool can wait together for one durable
        // payload+journal barrier. admitted_size reserves O_APPEND offsets while
        // those writes are not yet visible to readers.
        uint64_t admitted_size{};
        size_t durability_pending{};
        std::condition_variable_any durability_cv;
        size_t open_handles{};
        bool data_queued{};
        bool data_running{};
        bool data_deferred{};
        std::optional<int> backend_error;
        uint64_t journal_epoch{};
        uint64_t unconfirmed_data_sequence{};
        std::optional<FsEntry> unconfirmed_data_entry;

        ~Inode() {
            if (spool_fd >= 0)
                ::close(spool_fd);
        }
    };

    struct NamespaceOp {
        enum class Kind : uint8_t { mkdir, create, rmdir, unlink, rename, chmod, chown, utimens };
        Kind kind{};
        uint64_t sequence{};
        std::string from;
        std::string to;
        bool noreplace{};
        uint32_t mode{};
        uint32_t uid{};
        uint32_t gid{};
        bool set_uid{};
        bool set_gid{};
        int64_t mtime_ns{};
        int64_t ctime_ns{};
        std::vector<uint64_t> affected;
        // Namespace identities displaced by this operation (e.g. rename-over).
        // They remain valid for already-open handles but no longer own a published path.
        std::vector<uint64_t> removed;
    };

    enum class JournalRecord : uint8_t {
        inode = 1,
        namespace_op = 2,
        data_op = 3,
        namespace_published = 4,
        namespace_done = 5,
        data_published = 6,
        data_done = 7,
    };

    struct JournalInode {
        uint64_t id{};
        FsEntry base;
        FsEntry visible;
        std::string current_path;
        std::optional<std::string> published_path;
        uint64_t namespace_sequence{};
        uint64_t next_data_sequence{1};
    };

    struct JournalRecovery {
        std::map<uint64_t, JournalInode> inodes;
        std::map<uint64_t, NamespaceOp> namespace_ops;
        std::set<uint64_t> namespace_published;
        std::set<uint64_t> namespace_done;
        std::map<uint64_t, std::vector<DataOp>> data_ops;
        std::map<uint64_t, std::pair<uint64_t, FsEntry>> data_published;
        std::map<uint64_t, uint64_t> data_done;
        std::set<uint64_t> data_history_inodes;
        uint64_t max_inode{};
        uint64_t max_namespace_sequence{};
        size_t pending_operations{};
    };

    struct BrokerTask {
        Clock::time_point deadline{};
        std::shared_ptr<std::atomic_bool> cancelled;
        std::function<void(Clock::time_point, std::atomic_bool&)> fn;
    };

    struct DurabilityTicket {
        std::shared_ptr<Inode> inode;
        DataOp op;
    };

    struct BrokerQueue {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::deque<BrokerTask> tasks;
        std::vector<std::jthread> workers;
    };

    struct HintState {
        FsEntry entry;
        size_t first{};
        size_t last{};
        Clock::time_point expires{};
    };

    FileSystem& fs;
    FuseConfig config;
    std::filesystem::path spool_dir;
    std::filesystem::path journal_path;
    std::filesystem::path journal_dir;
    // Admission serialises the descriptor+operation pair against journal
    // compaction. Without it, the last completing operation could reset the
    // journal after an inode descriptor was observed as present but before the
    // new operation record was appended.
    std::mutex journal_admission_mutex;
    std::mutex journal_mutex;
    int journal_fd{-1};
    bool journal_poisoned{};
    std::atomic_uint64_t journal_epoch{1};
    std::atomic_size_t durable_pending_operations{};
    // Descriptor admission may precede the group-committed data-op frame. Keep
    // journal compaction from dropping that descriptor in the gap.
    std::atomic_size_t journal_inflight_admissions{};

    std::mutex durability_mutex;
    std::condition_variable_any durability_cv;
    std::deque<std::shared_ptr<DurabilityTicket>> durability_queue;
    std::jthread durability_worker;
    bool durability_poisoned{};
    std::exception_ptr durability_error;
    std::atomic_uint64_t durability_batches{};
    std::atomic_uint64_t durability_writes{};

    mutable std::mutex namespace_mutex;
    std::map<std::string, std::shared_ptr<Inode>, std::less<>> paths;
    std::map<uint64_t, std::shared_ptr<Inode>> inodes;
    uint64_t next_inode{2};
    uint64_t next_namespace_sequence{1};
    std::mutex namespace_apply_mutex;

    std::mutex namespace_queue_mutex;
    std::condition_variable_any namespace_cv;
    std::deque<NamespaceOp> namespace_queue;
    std::deque<NamespaceOp> namespace_unconfirmed;
    bool namespace_inflight{};
    std::jthread namespace_worker;
    std::atomic_uint64_t published_namespace_sequence{};
    std::mutex publication_mutex;
    std::condition_variable_any publication_cv;

    std::mutex data_queue_mutex;
    std::condition_variable_any data_cv;
    std::deque<std::shared_ptr<Inode>> data_queue;
    std::vector<std::jthread> data_workers;
    std::atomic_size_t active_data{};
    // Number of writable FUSE handles currently open. A writable handle is a
    // stronger foreground signal than a recent-operation timer: while rsync is
    // feeding a file, background publication must not fan out merely because
    // there was a short gap between kernel callbacks.
    std::atomic_size_t open_writers{};

    std::array<BrokerQueue, 6> broker;
    std::atomic_size_t broker_pending{};
    std::atomic_bool stopping{};

    mutable std::mutex hint_mutex;
    std::map<uint64_t, HintState> hint_states;
    std::function<void()> hint_wake_callback;

    std::mutex refresh_mutex;
    std::atomic_uint64_t refreshed_namespace_revision{};

    std::atomic_uint64_t timed_out_requests{};
    std::atomic_uint64_t merged_publications{};
    std::atomic_uint64_t backend_failures{};

    explicit State(FileSystem& filesystem, FuseConfig policy)
        : fs(filesystem), config(std::move(policy)),
          spool_dir(config.spool_path.value_or(fs.node().config().state_path / "fuse-spool")),
          journal_path(config.operation_journal_path.value_or(spool_dir / "operations.log")),
          journal_dir(journal_path.parent_path().empty() ? std::filesystem::path(".")
                                                        : journal_path.parent_path()) {}

    ~State() {
        if (journal_fd >= 0)
            ::close(journal_fd);
    }

    static constexpr std::array<uint8_t, 8> journal_magic{
        'M', 'A', 'C', 'H', 'F', 'U', 'S', '1'};
    static constexpr uint32_t max_journal_record = 16U * 1024U * 1024U;

    static void encode_namespace_op(Writer& writer, const NamespaceOp& op) {
        writer.u8(static_cast<uint8_t>(op.kind));
        writer.u64(op.sequence);
        writer.string(op.from);
        writer.string(op.to);
        writer.u8(op.noreplace ? 1 : 0);
        writer.u32(op.mode);
        writer.u32(op.uid);
        writer.u32(op.gid);
        writer.u8(op.set_uid ? 1 : 0);
        writer.u8(op.set_gid ? 1 : 0);
        writer.i64(op.mtime_ns);
        writer.i64(op.ctime_ns);
        if (op.affected.size() > UINT32_MAX || op.removed.size() > UINT32_MAX)
            throw FsError(EFBIG, "too many FUSE namespace identities");
        writer.u32(static_cast<uint32_t>(op.affected.size()));
        for (auto id : op.affected)
            writer.u64(id);
        writer.u32(static_cast<uint32_t>(op.removed.size()));
        for (auto id : op.removed)
            writer.u64(id);
    }

    static NamespaceOp decode_namespace_op(Reader& reader) {
        NamespaceOp op;
        const auto kind = reader.u8();
        if (kind > static_cast<uint8_t>(NamespaceOp::Kind::utimens))
            throw DecodeError("invalid FUSE namespace journal operation");
        op.kind = static_cast<NamespaceOp::Kind>(kind);
        op.sequence = reader.u64();
        op.from = canonical_path(reader.string());
        op.to = reader.string();
        if (!op.to.empty())
            op.to = canonical_path(op.to);
        const auto noreplace = reader.u8();
        if (noreplace > 1)
            throw DecodeError("invalid FUSE namespace noreplace flag");
        op.noreplace = noreplace != 0;
        op.mode = reader.u32();
        op.uid = reader.u32();
        op.gid = reader.u32();
        const auto set_uid = reader.u8();
        const auto set_gid = reader.u8();
        if (set_uid > 1 || set_gid > 1)
            throw DecodeError("invalid FUSE namespace ownership flag");
        op.set_uid = set_uid != 0;
        op.set_gid = set_gid != 0;
        op.mtime_ns = reader.i64();
        op.ctime_ns = reader.i64();
        const auto affected = reader.u32();
        if (affected > 4U * 1024U * 1024U)
            throw DecodeError("FUSE namespace affected set too large");
        op.affected.reserve(affected);
        for (uint32_t i = 0; i < affected; ++i)
            op.affected.push_back(reader.u64());
        const auto removed = reader.u32();
        if (removed > 4U * 1024U * 1024U)
            throw DecodeError("FUSE namespace removed set too large");
        op.removed.reserve(removed);
        for (uint32_t i = 0; i < removed; ++i)
            op.removed.push_back(reader.u64());
        return op;
    }

    static void encode_data_op(Writer& writer, uint64_t inode, const DataOp& op) {
        writer.u64(inode);
        writer.u8(static_cast<uint8_t>(op.kind));
        writer.u64(op.sequence);
        writer.u64(op.offset);
        writer.u64(op.length);
        writer.u64(op.spool_offset);
        writer.u64(op.size);
        writer.i64(op.mtime_ns);
        writer.i64(op.ctime_ns);
    }

    static std::pair<uint64_t, DataOp> decode_data_op(Reader& reader) {
        const auto inode = reader.u64();
        DataOp op;
        const auto kind = reader.u8();
        if (kind > static_cast<uint8_t>(DataOp::Kind::truncate))
            throw DecodeError("invalid FUSE data journal operation");
        op.kind = static_cast<DataOp::Kind>(kind);
        op.sequence = reader.u64();
        op.offset = reader.u64();
        op.length = reader.u64();
        op.spool_offset = reader.u64();
        op.size = reader.u64();
        op.mtime_ns = reader.i64();
        op.ctime_ns = reader.i64();
        return {inode, op};
    }

    void install_empty_journal_locked() {
        std::filesystem::create_directories(journal_dir);
        const auto temp = journal_path.string() + ".tmp." + std::to_string(getpid()) + "." +
                          std::to_string(unix_ms());
        int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            throw FsError(errno, "cannot create FUSE operation journal");
        try {
            write_exact(fd, journal_magic);
            fsync_fd(fd, "cannot sync FUSE operation journal");
            if (::close(fd) != 0)
                throw FsError(errno, "cannot close FUSE operation journal");
            fd = -1;
            if (::rename(temp.c_str(), journal_path.c_str()) != 0)
                throw FsError(errno, "cannot install FUSE operation journal");
            sync_directory(journal_dir);
        } catch (...) {
            if (fd >= 0)
                ::close(fd);
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            throw;
        }
    }

    void open_journal_append_locked() {
        if (journal_fd >= 0)
            ::close(journal_fd);
        journal_fd = ::open(journal_path.c_str(), O_WRONLY | O_APPEND);
        if (journal_fd < 0)
            throw FsError(errno, "cannot open FUSE operation journal");
        journal_poisoned = false;
    }

    static Bytes journal_frame(std::span<const uint8_t> payload) {
        if (payload.size() > max_journal_record)
            throw FsError(EFBIG, "FUSE operation journal record too large");
        Writer frame;
        frame.u32(static_cast<uint32_t>(payload.size()));
        frame.raw(payload);
        frame.fixed(sha256(payload).bytes);
        return frame.data();
    }

    void append_journal_records_locked(const std::vector<Bytes>& payloads) {
        if (journal_poisoned)
            throw FsError(EIO, "FUSE operation journal is unavailable after a previous write failure");
        if (payloads.empty())
            return;
        const auto start = ::lseek(journal_fd, 0, SEEK_END);
        if (start < 0)
            throw FsError(errno, "cannot seek FUSE operation journal");
        try {
            for (const auto& payload : payloads) {
                auto frame = journal_frame(payload);
                write_exact(journal_fd, frame);
            }
            fsync_fd(journal_fd, "cannot sync FUSE operation journal");
        } catch (...) {
            // A durability batch is all-or-nothing from recovery's point of
            // view. Do not leave earlier records from a failed batch in front
            // of later successful records.
            bool rolled_back = false;
            if (::ftruncate(journal_fd, start) == 0) {
                try {
                    fsync_fd(journal_fd, "cannot sync rolled-back FUSE operation journal");
                    rolled_back = true;
                } catch (...) {
                }
            }
            if (!rolled_back)
                journal_poisoned = true;
            throw;
        }
    }

    void append_journal_record_locked(std::span<const uint8_t> payload) {
        std::vector<Bytes> payloads;
        payloads.emplace_back(payload.begin(), payload.end());
        append_journal_records_locked(payloads);
    }

    void reset_journal_locked() {
        if (durable_pending_operations.load(std::memory_order_relaxed) != 0 ||
            journal_inflight_admissions.load(std::memory_order_relaxed) != 0)
            return;
        if (journal_fd >= 0) {
            ::close(journal_fd);
            journal_fd = -1;
        }
        install_empty_journal_locked();
        open_journal_append_locked();
        journal_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    void reset_journal_if_idle() {
        std::lock_guard admission_lock(journal_admission_mutex);
        std::lock_guard journal_lock(journal_mutex);
        reset_journal_locked();
    }

    void journal_inode_locked(const std::shared_ptr<Inode>& inode) {
        const auto epoch = journal_epoch.load(std::memory_order_relaxed);
        if (inode->journal_epoch == epoch)
            return;
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::inode));
        payload.u64(inode->id);
        payload.string(inode->current_path);
        payload.u8(inode->published_path ? 1 : 0);
        if (inode->published_path)
            payload.string(*inode->published_path);
        encode_fuse_entry(payload, inode->base);
        encode_fuse_entry(payload, inode->visible);
        payload.u64(inode->namespace_sequence);
        payload.u64(inode->next_data_sequence);
        std::lock_guard journal_lock(journal_mutex);
        append_journal_record_locked(payload.data());
        inode->journal_epoch = epoch;
    }

    void journal_namespace_operation(const NamespaceOp& op) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::namespace_op));
        encode_namespace_op(payload, op);
        std::lock_guard lock(journal_mutex);
        append_journal_record_locked(payload.data());
        durable_pending_operations.fetch_add(1, std::memory_order_relaxed);
    }

    void journal_data_operation(uint64_t inode, const DataOp& op) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_op));
        encode_data_op(payload, inode, op);
        std::lock_guard lock(journal_mutex);
        append_journal_record_locked(payload.data());
        durable_pending_operations.fetch_add(1, std::memory_order_relaxed);
    }

    void journal_data_batch(const std::vector<std::shared_ptr<DurabilityTicket>>& batch) {
        std::vector<Bytes> payloads;
        payloads.reserve(batch.size());
        for (const auto& ticket : batch) {
            Writer payload;
            payload.u8(static_cast<uint8_t>(JournalRecord::data_op));
            encode_data_op(payload, ticket->inode->id, ticket->op);
            payloads.push_back(payload.data());
        }

        std::lock_guard admission_lock(journal_admission_mutex);
        {
            std::lock_guard journal_lock(journal_mutex);
            append_journal_records_locked(payloads);
        }
        durable_pending_operations.fetch_add(batch.size(), std::memory_order_relaxed);
        const auto previous =
            journal_inflight_admissions.fetch_sub(batch.size(), std::memory_order_relaxed);
        if (previous < batch.size()) {
            journal_inflight_admissions.store(0, std::memory_order_relaxed);
            Log::error("FUSE journal admission accounting underflow after durable batch");
        }
    }

    void journal_namespace_published(uint64_t sequence) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::namespace_published));
        payload.u64(sequence);
        std::lock_guard lock(journal_mutex);
        append_journal_record_locked(payload.data());
    }

    void journal_namespace_done(uint64_t sequence) {
        std::lock_guard admission_lock(journal_admission_mutex);
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::namespace_done));
        payload.u64(sequence);
        std::lock_guard lock(journal_mutex);
        append_journal_record_locked(payload.data());
        const auto previous = durable_pending_operations.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 0)
            throw std::logic_error("FUSE journal namespace completion underflow");
        if (previous == 1)
            reset_journal_locked();
    }

    void journal_data_published(uint64_t inode, uint64_t sequence, const FsEntry& entry) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_published));
        payload.u64(inode);
        payload.u64(sequence);
        encode_fuse_entry(payload, entry);
        std::lock_guard lock(journal_mutex);
        append_journal_record_locked(payload.data());
    }

    bool journal_data_done(uint64_t inode, uint64_t sequence, size_t retired) {
        std::lock_guard admission_lock(journal_admission_mutex);
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_done));
        payload.u64(inode);
        payload.u64(sequence);
        std::lock_guard lock(journal_mutex);
        append_journal_record_locked(payload.data());
        const auto previous = durable_pending_operations.fetch_sub(retired, std::memory_order_relaxed);
        if (previous < retired)
            throw std::logic_error("FUSE journal data completion underflow");
        // Data bytes are cleaned up after this durable completion marker. Do
        // not compact the journal until that cleanup has succeeded; otherwise
        // a crash could leave a non-empty spool with no durable explanation.
        return previous == retired;
    }

    static size_t read_fd_all(int fd, std::span<uint8_t> out) {
        size_t done = 0;
        while (done < out.size()) {
            auto n = ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(done));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw FsError(errno, "cannot read FUSE operation journal");
            if (n == 0)
                break;
            done += static_cast<size_t>(n);
        }
        return done;
    }

    static uint32_t journal_be32(std::span<const uint8_t> bytes) {
        if (bytes.size() < 4)
            throw DecodeError("truncated FUSE journal frame length");
        return (static_cast<uint32_t>(bytes[0]) << 24) |
               (static_cast<uint32_t>(bytes[1]) << 16) |
               (static_cast<uint32_t>(bytes[2]) << 8) |
               static_cast<uint32_t>(bytes[3]);
    }

    void parse_journal_record(JournalRecovery& recovery, std::span<const uint8_t> payload,
                              size_t frame_offset) {
        Reader reader(payload);
        const auto raw_type = reader.u8();
        if (raw_type < static_cast<uint8_t>(JournalRecord::inode) ||
            raw_type > static_cast<uint8_t>(JournalRecord::data_done))
            throw DecodeError("unknown FUSE journal record type");
        const auto type = static_cast<JournalRecord>(raw_type);
        switch (type) {
        case JournalRecord::inode: {
            JournalInode inode;
            inode.id = reader.u64();
            if (!inode.id)
                throw DecodeError("invalid FUSE journal inode id");
            inode.current_path = reader.string();
            if (!inode.current_path.empty())
                inode.current_path = canonical_path(inode.current_path);
            const auto published = reader.u8();
            if (published > 1)
                throw DecodeError("invalid FUSE journal published-path flag");
            if (published)
                inode.published_path = canonical_path(reader.string());
            inode.base = decode_fuse_entry(reader);
            inode.visible = decode_fuse_entry(reader);
            inode.namespace_sequence = reader.u64();
            inode.next_data_sequence = reader.u64();
            recovery.max_inode = std::max(recovery.max_inode, inode.id);
            recovery.max_namespace_sequence =
                std::max(recovery.max_namespace_sequence, inode.namespace_sequence);
            if (recovery.inodes.contains(inode.id))
                throw DecodeError("duplicate FUSE journal inode descriptor");
            recovery.inodes.emplace(inode.id, std::move(inode));
            break;
        }
        case JournalRecord::namespace_op: {
            auto op = decode_namespace_op(reader);
            if (!op.sequence)
                throw DecodeError("invalid FUSE namespace journal sequence");
            recovery.max_namespace_sequence = std::max(recovery.max_namespace_sequence, op.sequence);
            for (auto id : op.affected)
                recovery.max_inode = std::max(recovery.max_inode, id);
            for (auto id : op.removed)
                recovery.max_inode = std::max(recovery.max_inode, id);
            if (recovery.namespace_ops.contains(op.sequence))
                throw DecodeError("duplicate FUSE namespace journal sequence");
            recovery.namespace_ops.emplace(op.sequence, std::move(op));
            break;
        }
        case JournalRecord::data_op: {
            auto [inode, op] = decode_data_op(reader);
            if (inode <= 1 || !op.sequence)
                throw DecodeError("invalid FUSE data journal identity");
            recovery.max_inode = std::max(recovery.max_inode, inode);
            recovery.data_history_inodes.insert(inode);
            auto& operations = recovery.data_ops[inode];
            if (!operations.empty() && operations.back().sequence >= op.sequence)
                throw DecodeError("non-monotonic FUSE data journal sequence");
            operations.push_back(op);
            break;
        }
        case JournalRecord::namespace_published: {
            const auto sequence = reader.u64();
            if (!recovery.namespace_ops.contains(sequence) ||
                !recovery.namespace_published.insert(sequence).second)
                throw DecodeError("invalid FUSE namespace published marker");
            break;
        }
        case JournalRecord::namespace_done: {
            const auto sequence = reader.u64();
            if (!recovery.namespace_ops.contains(sequence) ||
                !recovery.namespace_published.contains(sequence) ||
                !recovery.namespace_done.insert(sequence).second)
                throw DecodeError("invalid FUSE namespace completion marker");
            break;
        }
        case JournalRecord::data_published: {
            const auto inode = reader.u64();
            const auto sequence = reader.u64();
            auto entry = decode_fuse_entry(reader);
            auto operations = recovery.data_ops.find(inode);
            if (operations == recovery.data_ops.end() || operations->second.empty() ||
                operations->second.back().sequence < sequence)
                throw DecodeError("FUSE data published marker has no operation prefix");
            auto found = recovery.data_published.find(inode);
            if (found != recovery.data_published.end() && found->second.first >= sequence)
                throw DecodeError("non-monotonic FUSE data published marker");
            recovery.data_published[inode] = {sequence, std::move(entry)};
            break;
        }
        case JournalRecord::data_done: {
            const auto inode = reader.u64();
            const auto sequence = reader.u64();

            // A durable data_done is the retirement watermark. Runtime emits it
            // only after the target generation has been committed and observed,
            // and spool reclamation is ordered after this marker. Recovery may
            // therefore trust it even when the older, redundant data_published
            // proof is missing, but only when the journal itself contains the
            // exact ordered data operation being retired.
            auto operations = recovery.data_ops.find(inode);
            const bool has_operation =
                operations != recovery.data_ops.end() &&
                std::any_of(operations->second.begin(), operations->second.end(),
                            [&](const DataOp& op) { return op.sequence == sequence; });
            if (!has_operation)
                throw DecodeError("FUSE data completion marker has no operation prefix");

            auto done = recovery.data_done.find(inode);
            if (done != recovery.data_done.end() && done->second >= sequence)
                throw DecodeError("non-monotonic FUSE data completion marker");

            auto published = recovery.data_published.find(inode);
            if (published == recovery.data_published.end() || published->second.first < sequence) {
                Log::warn("FUSE journal recovery accepted data completion without published "
                          "prefix inode=" + std::to_string(inode) +
                          " sequence=" + std::to_string(sequence) +
                          " frame_offset=" + std::to_string(frame_offset));
            }
            recovery.data_done[inode] = sequence;
            break;
        }
        }
        reader.finish();
    }

    JournalRecovery load_journal() {
        std::filesystem::create_directories(spool_dir);
        std::filesystem::create_directories(journal_dir);
        std::lock_guard lock(journal_mutex);
        if (!std::filesystem::exists(journal_path))
            install_empty_journal_locked();

        int fd = ::open(journal_path.c_str(), O_RDWR);
        if (fd < 0)
            throw FsError(errno, "cannot open FUSE operation journal for recovery");
        try {
            struct stat statbuf {};
            if (::fstat(fd, &statbuf) != 0)
                throw FsError(errno, "cannot stat FUSE operation journal");
            if (statbuf.st_size < static_cast<off_t>(journal_magic.size()))
                throw std::runtime_error("FUSE operation journal header is missing");
            Bytes bytes(static_cast<size_t>(statbuf.st_size));
            if (read_fd_all(fd, bytes) != bytes.size())
                throw std::runtime_error("short FUSE operation journal read");
            if (!std::equal(journal_magic.begin(), journal_magic.end(), bytes.begin()))
                throw std::runtime_error("unsupported or corrupt FUSE operation journal header");

            JournalRecovery recovery;
            size_t position = journal_magic.size();
            size_t last_good = position;
            while (position < bytes.size()) {
                if (bytes.size() - position < 4)
                    break;
                const auto length = journal_be32({bytes.data() + position, 4});
                if (length > max_journal_record)
                    throw std::runtime_error("FUSE operation journal record length is corrupt");
                const size_t frame_size = 4ULL + length + 32ULL;
                if (bytes.size() - position < frame_size)
                    break;
                std::span<const uint8_t> payload(bytes.data() + position + 4, length);
                Hash256 expected;
                std::copy_n(bytes.begin() + static_cast<ptrdiff_t>(position + 4 + length), 32,
                            expected.bytes.begin());
                if (sha256(payload) != expected) {
                    // A crash can leave the final append at its full logical
                    // length while some tail sectors (including the checksum)
                    // were not durably written. Only an EOF checksum failure is
                    // therefore a recoverable torn append; corruption before a
                    // later frame remains fatal.
                    if (position + frame_size == bytes.size())
                        break;
                    throw std::runtime_error("FUSE operation journal checksum mismatch");
                }
                parse_journal_record(recovery, payload, position);
                position += frame_size;
                last_good = position;
            }
            if (last_good != bytes.size()) {
                if (::ftruncate(fd, static_cast<off_t>(last_good)) != 0)
                    throw FsError(errno, "cannot trim torn FUSE operation journal tail");
                fsync_fd(fd, "cannot sync trimmed FUSE operation journal");
                Log::warn("trimmed incomplete FUSE operation journal tail bytes=" +
                          std::to_string(bytes.size() - last_good));
            }
            if (::close(fd) != 0)
                throw FsError(errno, "cannot close FUSE operation journal after recovery");
            fd = -1;

            size_t pending = 0;
            for (const auto& [sequence, _] : recovery.namespace_ops)
                if (!recovery.namespace_done.contains(sequence))
                    ++pending;
            for (const auto& [inode, operations] : recovery.data_ops) {
                const auto done = recovery.data_done[inode];
                pending += static_cast<size_t>(std::count_if(
                    operations.begin(), operations.end(),
                    [&](const DataOp& op) { return op.sequence > done; }));
            }
            recovery.pending_operations = pending;
            durable_pending_operations.store(pending, std::memory_order_relaxed);
            open_journal_append_locked();
            return recovery;
        } catch (...) {
            if (fd >= 0)
                ::close(fd);
            throw;
        }
    }

    void throw_if_durability_poisoned() {
        std::lock_guard lock(durability_mutex);
        if (!durability_poisoned)
            return;
        if (durability_error)
            std::rethrow_exception(durability_error);
        throw FsError(EIO, "FUSE durability coordinator is unavailable");
    }

    void wait_for_inode_durability(const std::shared_ptr<Inode>& inode,
                                   Clock::time_point deadline, std::atomic_bool& cancelled) {
        std::unique_lock lock(inode->mutex);
        while (inode->durability_pending && !inode->backend_error) {
            if (deadline == Clock::time_point::max()) {
                inode->durability_cv.wait(lock);
            } else if (inode->durability_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                check_deadline(deadline, cancelled);
            }
        }
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "FUSE inode durability error");
        check_deadline(deadline, cancelled);
    }

    uint64_t durable_sequence(const std::shared_ptr<Inode>& inode) const {
        std::lock_guard lock(inode->mutex);
        return inode->durable_data_sequence;
    }

    void wait_for_inode_publication(const std::shared_ptr<Inode>& inode, uint64_t target,
                                    Clock::time_point deadline, std::atomic_bool& cancelled) {
        if (!target)
            return;
        std::unique_lock queue_lock(data_queue_mutex);
        while (true) {
            {
                std::lock_guard inode_lock(inode->mutex);
                if (inode->backend_error)
                    throw FsError(*inode->backend_error, "FUSE inode publication error");
                // replay_data advances this only after WriteHandle::commit() has
                // made every referenced extent durable to the configured data
                // policy and committed the resulting file metadata. Local
                // snapshot confirmation/overlay retirement may lag, but cluster
                // durability has already been achieved at this watermark.
                if (inode->published_data_sequence >= target)
                    return;
            }
            check_deadline(deadline, cancelled);
            if (deadline == Clock::time_point::max()) {
                data_cv.wait(queue_lock);
            } else if (data_cv.wait_until(queue_lock, deadline) == std::cv_status::timeout) {
                check_deadline(deadline, cancelled);
            }
        }
    }

    void enqueue_durability(const std::shared_ptr<DurabilityTicket>& ticket) {
        {
            std::lock_guard lock(durability_mutex);
            // Queue even after a concurrent poison transition: the coordinator
            // owns failure accounting for admitted descriptors and inode state.
            durability_queue.push_back(ticket);
        }
        durability_cv.notify_one();
    }

    static int durability_error_code(const std::exception_ptr& error) {
        try {
            if (error)
                std::rethrow_exception(error);
        } catch (const FsError& e) {
            return e.code();
        } catch (...) {
        }
        return EIO;
    }

    void fail_durability_batch(const std::vector<std::shared_ptr<DurabilityTicket>>& batch,
                               const std::exception_ptr& error) {
        {
            std::lock_guard admission_lock(journal_admission_mutex);
            const auto previous =
                journal_inflight_admissions.fetch_sub(batch.size(), std::memory_order_relaxed);
            if (previous < batch.size()) {
                journal_inflight_admissions.store(0, std::memory_order_relaxed);
                Log::error("FUSE journal admission accounting underflow after durability failure");
            }
        }
        const auto code = durability_error_code(error);
        for (const auto& ticket : batch) {
            {
                std::lock_guard lock(ticket->inode->mutex);
                ticket->inode->backend_error = code;
                if (ticket->inode->durability_pending)
                    --ticket->inode->durability_pending;
                if (!ticket->inode->durability_pending)
                    ticket->inode->admitted_size = ticket->inode->visible.size;
            }
            ticket->inode->durability_cv.notify_all();
        }
    }

    void finish_durability_batch(const std::vector<std::shared_ptr<DurabilityTicket>>& batch) {
        for (const auto& ticket : batch) {
            auto inode = ticket->inode;
            {
                std::lock_guard lock(inode->mutex);
                // write() made the operation immediately visible before it
                // returned. The durability worker only advances the publication
                // watermark after payload -> journal ordering is on stable
                // storage. A single worker consumes tickets FIFO, so per-inode
                // sequences become durable in admission order.
                inode->durable_data_sequence =
                    std::max(inode->durable_data_sequence, ticket->op.sequence);
                if (inode->durability_pending)
                    --inode->durability_pending;
                if (!inode->durability_pending)
                    inode->admitted_size = inode->visible.size;
            }
            inode->durability_cv.notify_all();
        }
    }

    void durability_loop(std::stop_token stop) {
        while (true) {
            std::vector<std::shared_ptr<DurabilityTicket>> batch;
            {
                std::unique_lock lock(durability_mutex);
                durability_cv.wait(lock, [&] {
                    return stop.stop_requested() || !durability_queue.empty();
                });
                if (durability_queue.empty() && stop.stop_requested())
                    break;

                // POSIX write() acknowledgement is decoupled from stable
                // storage, so even one sequential writer can place several
                // operations in this queue before the barrier runs. Give those
                // admissions a very small coalescing window and amortise the
                // spool+journal fsync pair across the batch. close/release and
                // fsync explicitly wait for the resulting durability watermark.
                if (!stop.stop_requested())
                    durability_cv.wait_for(lock, std::chrono::milliseconds(25), [&] {
                        return stop.stop_requested();
                    });
                batch.assign(durability_queue.begin(), durability_queue.end());
                durability_queue.clear();
                if (durability_poisoned) {
                    auto error = durability_error ? durability_error : std::make_exception_ptr(
                        FsError(EIO, "FUSE durability coordinator is unavailable"));
                    lock.unlock();
                    fail_durability_batch(batch, error);
                    continue;
                }
            }

            const auto started = Clock::now();
            try {
                std::set<int> spool_fds;
                for (const auto& ticket : batch) {
                    std::lock_guard lock(ticket->inode->mutex);
                    if (ticket->inode->spool_fd < 0)
                        throw FsError(EIO, "missing FUSE write spool during durability commit");
                    spool_fds.insert(ticket->inode->spool_fd);
                }
                for (auto fd : spool_fds)
                    fsync_fd(fd, "FUSE local spool group sync failed");

                // Payload durability always precedes descriptor durability.
                // A process/power failure before this point may lose an
                // acknowledged-but-not-synchronised write (normal POSIX write
                // semantics), but recovery can never observe a durable data-op
                // descriptor whose spool bytes were not made durable first.
                journal_data_batch(batch);
                finish_durability_batch(batch);
                durability_batches.fetch_add(1, std::memory_order_relaxed);
                durability_writes.fetch_add(batch.size(), std::memory_order_relaxed);

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - started).count();
                if (Log::enabled(LogLevel::debug) && elapsed >= 100)
                    Log::debug("DIAG FUSE durability batch writes=" +
                               std::to_string(batch.size()) + " spool_fds=" +
                               std::to_string(spool_fds.size()) + " elapsed_ms=" +
                               std::to_string(elapsed));
            } catch (...) {
                auto error = std::current_exception();
                {
                    std::lock_guard lock(durability_mutex);
                    durability_poisoned = true;
                    durability_error = error;
                }
                fail_durability_batch(batch, error);
            }
        }
    }

    static size_t class_index(FuseOperationClass operation) {
        return static_cast<size_t>(operation);
    }

    std::shared_ptr<Inode> resolve_locked(std::string_view path) const {
        auto found = paths.find(canonical_path(path));
        if (found == paths.end())
            throw FsError(ENOENT, "missing");
        return found->second;
    }

    std::shared_ptr<Inode> resolve_inode(uint64_t id) const {
        std::lock_guard lock(namespace_mutex);
        auto found = inodes.find(id);
        if (found == inodes.end())
            throw FsError(EBADF, "unknown FUSE inode");
        return found->second;
    }

    void require_parent_locked(const std::string& path) const {
        auto parent = parent_path(path);
        auto found = paths.find(canonical_path(parent));
        if (found == paths.end())
            throw FsError(ENOENT, "parent missing");
        std::lock_guard inode_lock(found->second->mutex);
        if (found->second->visible.type != EntryType::directory)
            throw FsError(ENOTDIR, "parent not directory");
    }

    static bool snapshot_has_path(const MetadataSnapshot& snapshot, std::string_view path) {
        return snapshot.entries.find(normalize_path(std::string(path))) != snapshot.entries.end();
    }

    static const FsEntry* snapshot_entry(const MetadataSnapshot& snapshot, std::string_view path) {
        auto found = snapshot.entries.find(normalize_path(std::string(path)));
        return found == snapshot.entries.end() ? nullptr : &found->second;
    }

    static bool namespace_effect_confirmed(const NamespaceOp& op, const MetadataSnapshot& snapshot) {
        switch (op.kind) {
        case NamespaceOp::Kind::mkdir: {
            auto entry = snapshot_entry(snapshot, op.from);
            return entry && entry->type == EntryType::directory &&
                   entry->mode == (op.mode & 07777) && entry->uid == op.uid && entry->gid == op.gid;
        }
        case NamespaceOp::Kind::create: {
            auto entry = snapshot_entry(snapshot, op.from);
            return entry && entry->type == EntryType::file &&
                   entry->mode == (op.mode & 07777) && entry->uid == op.uid && entry->gid == op.gid;
        }
        case NamespaceOp::Kind::rmdir:
        case NamespaceOp::Kind::unlink:
            return !snapshot_has_path(snapshot, op.from);
        case NamespaceOp::Kind::rename:
            return !snapshot_has_path(snapshot, op.from) && snapshot_has_path(snapshot, op.to);
        case NamespaceOp::Kind::chmod: {
            auto entry = snapshot_entry(snapshot, op.from);
            return entry && entry->mode == (op.mode & 07777);
        }
        case NamespaceOp::Kind::chown: {
            auto entry = snapshot_entry(snapshot, op.from);
            return entry && (!op.set_uid || entry->uid == op.uid) &&
                   (!op.set_gid || entry->gid == op.gid);
        }
        case NamespaceOp::Kind::utimens: {
            auto entry = snapshot_entry(snapshot, op.from);
            return entry && entry->mtime_ns == op.mtime_ns;
        }
        }
        return false;
    }

    static bool contains_inode(const std::vector<uint64_t>& ids, uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    static bool snapshot_path_shadowed(std::string_view path, const JournalRecovery& recovery) {
        const auto key = canonical_path(path);
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (recovery.namespace_done.contains(sequence))
                continue;
            switch (op.kind) {
            case NamespaceOp::Kind::rename:
                // The locally acknowledged view has already removed the old
                // subtree and, for rename-over, displaced the destination.
                // A stale decoded backend snapshot must not resurrect either.
                if (under_path(key, op.from) || under_path(key, op.to))
                    return true;
                break;
            case NamespaceOp::Kind::rmdir:
            case NamespaceOp::Kind::unlink:
                if (under_path(key, op.from))
                    return true;
                break;
            case NamespaceOp::Kind::mkdir:
            case NamespaceOp::Kind::create:
            case NamespaceOp::Kind::chmod:
            case NamespaceOp::Kind::chown:
            case NamespaceOp::Kind::utimens:
                if (key == canonical_path(op.from))
                    return true;
                break;
            }
        }
        return false;
    }

    static std::string transformed_path(std::string path, uint64_t inode, const NamespaceOp& op) {
        if (contains_inode(op.removed, inode))
            return {};
        if (!contains_inode(op.affected, inode))
            return path;
        switch (op.kind) {
        case NamespaceOp::Kind::mkdir:
        case NamespaceOp::Kind::create:
            return op.from;
        case NamespaceOp::Kind::rmdir:
        case NamespaceOp::Kind::unlink:
            return {};
        case NamespaceOp::Kind::rename:
            if (path.empty())
                return path;
            if (under_path(path, op.from))
                return op.to + path.substr(op.from.size());
            return path;
        case NamespaceOp::Kind::chmod:
        case NamespaceOp::Kind::chown:
        case NamespaceOp::Kind::utimens:
            return path;
        }
        return path;
    }

    size_t namespace_pending_locked() const {
        return namespace_queue.size() + namespace_unconfirmed.size() +
               (namespace_inflight ? 1U : 0U);
    }

    bool namespace_capacity_available() {
        std::lock_guard lock(namespace_queue_mutex);
        return namespace_pending_locked() < config.max_pending_operations;
    }

    void enqueue_namespace(NamespaceOp operation) {
        {
            std::lock_guard lock(namespace_queue_mutex);
            // Admission capacity was checked while namespace_apply_mutex was
            // held. Do not re-check after the durable/local mutation: the worker
            // can transiently count one operation as both inflight and awaiting
            // metadata confirmation, and rejecting here would leave an accepted
            // durable operation absent from the in-memory queue until restart.
            namespace_queue.push_back(std::move(operation));
        }
        namespace_cv.notify_one();
    }

    int ensure_spool_locked(const std::shared_ptr<Inode>& inode) {
        if (inode->spool_fd >= 0)
            return inode->spool_fd;
        std::filesystem::create_directories(spool_dir);
        inode->spool_path = spool_dir / ("inode-" + std::to_string(inode->id) + ".spool");
        const bool existed = std::filesystem::exists(inode->spool_path);
        int fd = ::open(inode->spool_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0)
            throw FsError(errno, "cannot open FUSE spool");
        try {
            auto end = ::lseek(fd, 0, SEEK_END);
            if (end < 0)
                throw FsError(errno, "cannot seek FUSE spool");
            // If the spool inode itself is new, make the directory entry durable
            // before any journal record is allowed to refer to it.
            if (!existed) {
                fsync_fd(fd, "cannot sync new FUSE spool");
                sync_directory(spool_dir);
            }
            inode->spool_fd = fd;
            inode->spool_end = static_cast<uint64_t>(end);
            return fd;
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

    bool retire_spool_locked(Inode& inode) {
        if (inode.spool_path.empty()) {
            if (inode.spool_fd >= 0) {
                ::close(inode.spool_fd);
                inode.spool_fd = -1;
            }
            inode.spool_end = 0;
            return true;
        }

        int fd = inode.spool_fd;
        bool temporary = false;
        if (fd < 0) {
            fd = ::open(inode.spool_path.c_str(), O_RDWR);
            if (fd < 0) {
                Log::warn("cannot open retired FUSE spool inode=" +
                          std::to_string(inode.id) + " error=" + std::strerror(errno));
                return false;
            }
            temporary = true;
        }

        bool clean = true;
        if (::ftruncate(fd, 0) != 0) {
            clean = false;
            Log::warn("cannot truncate retired FUSE spool inode=" +
                      std::to_string(inode.id) + " error=" + std::strerror(errno));
        } else {
            inode.spool_end = 0;
            try {
                fsync_fd(fd, "cannot sync retired FUSE spool");
            } catch (const std::exception& e) {
                clean = false;
                Log::warn("cannot sync retired FUSE spool inode=" +
                          std::to_string(inode.id) + " error=" + e.what());
            }
        }

        if (temporary) {
            ::close(fd);
        } else {
            ::close(inode.spool_fd);
            inode.spool_fd = -1;
        }
        return clean;
    }

    void request_data_publication(const std::shared_ptr<Inode>& inode) {
        bool enqueue = false;
        {
            std::lock_guard inode_lock(inode->mutex);
            if (inode->data_ops.empty() ||
                inode->durable_data_sequence <= inode->published_data_sequence)
                return;
            // Never expose acknowledged-but-not-yet-durable local writes to the
            // distributed publication path. They remain a node-local overlay
            // until the durability worker advances this watermark.
            inode->requested_data_sequence = inode->durable_data_sequence;
            inode->requested_namespace_sequence = inode->namespace_sequence;
            // Only one committed-but-not-yet-observed data generation may be in
            // flight for an inode. Keeping later writes in the durable overlay
            // prevents an older available metadata view from becoming the base.
            if (inode->unconfirmed_data_entry) {
                inode->data_deferred = true;
                ++merged_publications;
                return;
            }
            if (inode->data_queued || inode->data_running) {
                ++merged_publications;
                return;
            }
            enqueue = true;
        }
        if (!enqueue)
            return;

        std::lock_guard queue_lock(data_queue_mutex);
        std::lock_guard inode_lock(inode->mutex);
        if (inode->data_queued || inode->data_running || inode->unconfirmed_data_entry)
            return;
        if (data_queue.size() >= config.max_pending_operations) {
            inode->data_deferred = true;
            return;
        }
        inode->data_queued = true;
        inode->data_deferred = false;
        data_queue.push_back(inode);
        data_cv.notify_one();
    }

    void admit_deferred() {
        std::vector<std::shared_ptr<Inode>> candidates;
        {
            std::lock_guard lock(namespace_mutex);
            candidates.reserve(inodes.size());
            for (const auto& [_, inode] : inodes)
                candidates.push_back(inode);
        }
        std::lock_guard queue_lock(data_queue_mutex);
        for (const auto& inode : candidates) {
            if (data_queue.size() >= config.max_pending_operations)
                break;
            std::lock_guard inode_lock(inode->mutex);
            if (!inode->data_deferred || inode->data_queued || inode->data_running ||
                inode->unconfirmed_data_entry)
                continue;
            inode->data_deferred = false;
            inode->data_queued = true;
            data_queue.push_back(inode);
        }
        data_cv.notify_all();
    }

    void mark_backend_error(const std::vector<uint64_t>& affected, int error) {
        std::lock_guard lock(namespace_mutex);
        for (auto id : affected) {
            auto found = inodes.find(id);
            if (found == inodes.end())
                continue;
            std::lock_guard inode_lock(found->second->mutex);
            found->second->backend_error = error;
        }
    }

    void apply_namespace_backend(const NamespaceOp& op) {
        try {
            switch (op.kind) {
            case NamespaceOp::Kind::mkdir:
                fs.mkdir(op.from, op.mode, op.uid, op.gid);
                break;
            case NamespaceOp::Kind::create: {
                auto committed = fs.create_file(op.from, op.mode, op.uid, op.gid);
                std::lock_guard lock(namespace_mutex);
                if (!op.affected.empty()) {
                    auto found = inodes.find(op.affected.front());
                    if (found != inodes.end()) {
                        std::lock_guard inode_lock(found->second->mutex);
                        found->second->base = committed;
                        found->second->published_path = op.from;
                    }
                }
                break;
            }
            case NamespaceOp::Kind::rmdir:
                fs.rmdir(op.from);
                break;
            case NamespaceOp::Kind::unlink:
                fs.unlink(op.from);
                break;
            case NamespaceOp::Kind::rename:
                fs.rename(op.from, op.to, op.noreplace);
                break;
            case NamespaceOp::Kind::chmod:
                fs.chmod(op.from, op.mode);
                break;
            case NamespaceOp::Kind::chown:
                fs.chown(op.from, op.uid, op.gid, op.set_uid, op.set_gid);
                break;
            case NamespaceOp::Kind::utimens:
                fs.utimens(op.from, op.mtime_ns);
                break;
            }
        } catch (const FsError&) {
            // A process can die after the distributed mutation commits but before
            // its local published marker is fsynced. Recovery must be able to
            // replay that operation without turning an already-achieved state
            // into a false permanent error. Re-read the node's local committed
            // snapshot on this exceptional path and accept the operation iff its
            // desired effect is already visible there.
            auto view = fs.local_snapshot_view();
            if (!namespace_effect_confirmed(op, *view.snapshot))
                throw;
            if (op.kind == NamespaceOp::Kind::create && !op.affected.empty()) {
                if (auto entry = snapshot_entry(*view.snapshot, op.from)) {
                    std::lock_guard lock(namespace_mutex);
                    auto found = inodes.find(op.affected.front());
                    if (found != inodes.end()) {
                        std::lock_guard inode_lock(found->second->mutex);
                        found->second->base = *entry;
                    }
                }
            }
        }
    }

    void namespace_success(const NamespaceOp& op) {
        std::lock_guard lock(namespace_mutex);
        for (auto id : op.affected) {
            auto found = inodes.find(id);
            if (found == inodes.end())
                continue;
            std::lock_guard inode_lock(found->second->mutex);
            found->second->backend_error.reset();
        }
        if (op.kind == NamespaceOp::Kind::rename) {
            for (auto id : op.removed) {
                auto found = inodes.find(id);
                if (found == inodes.end())
                    continue;
                std::lock_guard inode_lock(found->second->mutex);
                found->second->published_path.reset();
            }
            for (auto id : op.affected) {
                auto found = inodes.find(id);
                if (found == inodes.end())
                    continue;
                std::lock_guard inode_lock(found->second->mutex);
                if (found->second->published_path && under_path(*found->second->published_path, op.from))
                    found->second->published_path =
                        op.to + found->second->published_path->substr(op.from.size());
            }
        } else if (op.kind == NamespaceOp::Kind::unlink || op.kind == NamespaceOp::Kind::rmdir) {
            for (auto id : op.affected) {
                auto found = inodes.find(id);
                if (found == inodes.end())
                    continue;
                std::lock_guard inode_lock(found->second->mutex);
                found->second->published_path.reset();
            }
        } else if (op.kind == NamespaceOp::Kind::mkdir) {
            if (!op.affected.empty()) {
                auto found = inodes.find(op.affected.front());
                if (found != inodes.end()) {
                    std::lock_guard inode_lock(found->second->mutex);
                    found->second->published_path = op.from;
                }
            }
        }
    }

    void namespace_loop(std::stop_token stop) {
        while (!stop.stop_requested() && !stopping.load()) {
            NamespaceOp op;
            {
                std::unique_lock lock(namespace_queue_mutex);
                namespace_cv.wait(lock, stop, [&] { return !namespace_queue.empty() || stopping.load(); });
                if (stop.stop_requested() || stopping.load())
                    break;
                op = namespace_queue.front();
                namespace_queue.pop_front();
                namespace_inflight = true;
            }

            bool published = false;
            std::chrono::milliseconds backoff{50};
            while (!published && !stop.stop_requested() && !stopping.load()) {
                try {
                    // Namespace publication is also asynchronous convergence. It
                    // must not take the shared metadata transaction while viewer
                    // playback is waiting for storage/RPC service.
                    wait_for_playback_quiet();
                    {
                        std::lock_guard backend(publication_mutex);
                        apply_namespace_backend(op);
                    }
                    namespace_success(op);
                    journal_namespace_published(op.sequence);
                    published = true;
                } catch (const std::exception& e) {
                    ++backend_failures;
                    const bool retryable = retryable_backend_error(e);
                    if (!retryable) {
                        int error = EIO;
                        if (const auto* fs_error = dynamic_cast<const FsError*>(&e))
                            error = fs_error->code();
                        auto errored = op.affected;
                        errored.insert(errored.end(), op.removed.begin(), op.removed.end());
                        mark_backend_error(errored, error);
                        Log::error("FUSE async namespace publication blocked seq=" +
                                   std::to_string(op.sequence) + " error=" + e.what());
                    } else {
                        Log::debug("FUSE async namespace publication retry seq=" +
                                   std::to_string(op.sequence) + " error=" + e.what());
                    }
                    // Never retire or skip an acknowledged durable namespace op.
                    // A non-retryable backend error has no automatic conflict
                    // resolver today; preserve ordering and keep retrying at a
                    // bounded cadence rather than claiming convergence.
                    std::unique_lock wait_lock(namespace_queue_mutex);
                    namespace_cv.wait_for(wait_lock, stop, backoff, [&] {
                        return stopping.load();
                    });
                    backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
                }
            }

            if (published) {
                published_namespace_sequence.store(op.sequence, std::memory_order_release);
                bool confirmed = false;
                if (auto available = fs.available_snapshot_view())
                    confirmed = namespace_effect_confirmed(op, *available->snapshot);
                if (confirmed) {
                    journal_namespace_done(op.sequence);
                } else {
                    std::lock_guard lock(namespace_queue_mutex);
                    namespace_unconfirmed.push_back(op);
                }
                publication_cv.notify_all();
            } else if (!stopping.load()) {
                // Stop-requested workers leave the durable operation pending for
                // startup replay. It was popped from RAM only for this worker.
                std::lock_guard lock(namespace_queue_mutex);
                namespace_queue.push_front(op);
            }
            {
                std::lock_guard lock(namespace_queue_mutex);
                namespace_inflight = false;
            }
            namespace_cv.notify_all();
        }
    }

    struct DataSnapshot {
        uint64_t target_sequence{};
        uint64_t required_namespace_sequence{};
        std::vector<DataOp> operations;
        std::optional<std::string> published_path;
        std::filesystem::path spool_path;
    };

    DataSnapshot snapshot_data(const std::shared_ptr<Inode>& inode) {
        std::lock_guard lock(inode->mutex);
        DataSnapshot snapshot;
        if (inode->unconfirmed_data_entry)
            return snapshot;
        snapshot.target_sequence = inode->requested_data_sequence;
        // Namespace mutations accepted after flush/release still order the
        // eventual data publication. Waiting through the inode's latest local
        // namespace sequence means a queued rename/unlink cannot leave a data
        // writer committing through a path which the frontend has already
        // superseded.
        snapshot.required_namespace_sequence =
            std::max(inode->requested_namespace_sequence, inode->namespace_sequence);
        snapshot.published_path = inode->published_path;
        snapshot.spool_path = inode->spool_path;
        for (const auto& op : inode->data_ops) {
            if (op.sequence > inode->published_data_sequence &&
                op.sequence <= snapshot.target_sequence)
                snapshot.operations.push_back(op);
        }
        return snapshot;
    }

    bool playback_quiet() const {
        return config.publication_quiet.count() <= 0 ||
               fs.foreground_idle_for() >= config.publication_quiet;
    }

    void wait_for_playback_quiet() {
        while (!stopping.load(std::memory_order_relaxed) && !playback_quiet()) {
            const auto idle = fs.foreground_idle_for();
            const auto remaining = idle < config.publication_quiet
                                       ? config.publication_quiet - idle
                                       : std::chrono::milliseconds(0);
            if (remaining <= std::chrono::milliseconds(0))
                break;
            std::unique_lock lock(data_queue_mutex);
            data_cv.wait_for(lock, remaining, [&] {
                return stopping.load(std::memory_order_relaxed);
            });
        }
        if (stopping.load(std::memory_order_relaxed))
            throw FsError(EINTR, "FUSE publication stopping");
    }

    void replay_data(const std::shared_ptr<Inode>& inode, DataSnapshot snapshot) {
        if (snapshot.operations.empty())
            return;

        std::unique_lock sequence_lock(data_queue_mutex);
        publication_cv.wait(sequence_lock, [&] {
            return stopping.load() ||
                   published_namespace_sequence.load(std::memory_order_acquire) >=
                       snapshot.required_namespace_sequence;
        });
        sequence_lock.unlock();
        if (stopping.load())
            return;

        {
            std::lock_guard lock(inode->mutex);
            snapshot.published_path = inode->published_path;
            if (inode->backend_error)
                throw FsError(*inode->backend_error, "FUSE inode has asynchronous backend error");
        }
        if (!snapshot.published_path) {
            const auto retired = snapshot.operations.size();
            const bool journal_idle = journal_data_done(inode->id, snapshot.target_sequence, retired);
            bool spool_clean = true;
            std::lock_guard lock(inode->mutex);
            inode->data_ops.erase(
                std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                               [&](const DataOp& op) { return op.sequence <= snapshot.target_sequence; }),
                inode->data_ops.end());
            inode->published_data_sequence =
                std::max(inode->published_data_sequence, snapshot.target_sequence);
            if (inode->data_ops.empty() && !inode->durability_pending)
                spool_clean = retire_spool_locked(*inode);
            if (journal_idle && spool_clean)
                reset_journal_if_idle();
            return;
        }

        std::shared_ptr<WriteHandle> writer;
        ScopedFd replay_spool;
        try {
            writer = fs.open_write(*snapshot.published_path, false, config.write_through_cache);
            constexpr size_t chunk_size = 256 * 1024;
            Bytes buffer(chunk_size);
            for (const auto& op : snapshot.operations) {
                if (stopping.load())
                    throw FsError(EINTR, "FUSE publication stopping");
                if (op.kind == DataOp::Kind::truncate) {
                    wait_for_playback_quiet();
                    writer->truncate(op.size);
                    continue;
                }
                uint64_t done = 0;
                while (done < op.length) {
                    // Only the locally durable prefix reaches this path. The
                    // write bytes and operation descriptor have completed the
                    // spool -> journal durability barrier, so distributed
                    // publication may yield to viewer playback between bounded
                    // replay chunks without endangering recoverable input.
                    wait_for_playback_quiet();
                    const auto chunk = static_cast<size_t>(
                        std::min<uint64_t>(buffer.size(), op.length - done));
                    if (replay_spool.get() < 0) {
                        const int fd = ::open(snapshot.spool_path.c_str(), O_RDONLY);
                        if (fd < 0)
                            throw FsError(errno, "cannot open FUSE write spool for publication");
                        replay_spool.reset(fd);
                    }
                    if (pread_exact(replay_spool.get(), {buffer.data(), chunk},
                                    op.spool_offset + done) != chunk)
                        throw FsError(EIO, "short read from FUSE write spool");
                    if (writer->write(op.offset + done, {buffer.data(), chunk}) != chunk)
                        throw FsError(EIO, "short replay into Macha write handle");
                    done += chunk;
                }
            }
            {
                wait_for_playback_quiet();
                std::lock_guard backend(publication_mutex);
                writer->commit();
            }
        } catch (const FsError& e) {
            if (e.code() == ENOENT || e.code() == EAGAIN) {
                std::lock_guard lock(inode->mutex);
                if (inode->published_path != snapshot.published_path ||
                    inode->namespace_sequence > snapshot.required_namespace_sequence)
                    throw FsError(EAGAIN, "FUSE namespace advanced during data publication");
            }
            throw;
        }
        auto committed = writer->committed_entry();

        // Record that the backend accepted this exact file generation before
        // changing the in-memory publication watermark. A crash after the
        // backend commit but before this marker simply replays idempotently.
        journal_data_published(inode->id, snapshot.target_sequence, committed);
        {
            std::lock_guard lock(inode->mutex);
            inode->base = committed;
            inode->published_data_sequence =
                std::max(inode->published_data_sequence, snapshot.target_sequence);
            inode->unconfirmed_data_sequence = snapshot.target_sequence;
            inode->unconfirmed_data_entry = committed;
        }

        // Retire the durable overlay only after MetadataManager's decoded view
        // demonstrates the committed content. If propagation lags, a later
        // namespace-facing request will perform the same confirmation.
        if (auto available = fs.available_snapshot_view())
            (void)confirm_data_from_snapshot(inode, *available->snapshot);
    }

    bool data_slot_available() const {
        // Playback is stricter than mounted-filesystem foreground activity.
        // FUSE data has already been admitted to the local spool, so starting a
        // distributed publication while a viewer is waiting serves no latency
        // purpose and only creates storage/network contention.
        if (!playback_quiet())
            return false;
        const bool recent_mount_activity = config.publication_quiet.count() > 0 &&
            fs.interactive_idle_for() < config.publication_quiet;
        const bool mount_busy =
            open_writers.load(std::memory_order_relaxed) > 0 || recent_mount_activity;
        const auto limit = mount_busy ? config.foreground_commit_workers : config.commit_workers;
        return active_data.load(std::memory_order_relaxed) < limit;
    }

    void data_loop(std::stop_token stop) {
        while (!stop.stop_requested() && !stopping.load()) {
            std::shared_ptr<Inode> inode;
            {
                std::unique_lock lock(data_queue_mutex);
                while (!stop.stop_requested() && !stopping.load()) {
                    if (data_queue.empty()) {
                        data_cv.wait(lock, stop, [&] {
                            return stopping.load() || !data_queue.empty();
                        });
                        continue;
                    }
                    if (data_slot_available())
                        break;

                    // Active publications and writer close both notify data_cv.
                    // The only transition without a producer event is expiry of
                    // publication_quiet, so sleep directly to that deadline rather
                    // than waking every 50 ms while the queue is throttled.
                    const auto playback_idle = fs.foreground_idle_for();
                    if (config.publication_quiet.count() > 0 &&
                        playback_idle < config.publication_quiet) {
                        data_cv.wait_for(lock, stop, config.publication_quiet - playback_idle, [&] {
                            return stopping.load() || data_queue.empty() || data_slot_available();
                        });
                        continue;
                    }
                    const auto mount_idle = fs.interactive_idle_for();
                    if (open_writers.load(std::memory_order_relaxed) == 0 &&
                        config.publication_quiet.count() > 0 && mount_idle < config.publication_quiet) {
                        data_cv.wait_for(lock, stop, config.publication_quiet - mount_idle, [&] {
                            return stopping.load() || data_queue.empty() || data_slot_available();
                        });
                    } else {
                        data_cv.wait(lock, stop, [&] {
                            return stopping.load() || data_queue.empty() || data_slot_available();
                        });
                    }
                }
                if (stop.stop_requested() || stopping.load())
                    break;
                if (data_queue.empty() || !data_slot_available())
                    continue;
                inode = data_queue.front();
                data_queue.pop_front();
                ++active_data;
            }
            {
                std::lock_guard lock(inode->mutex);
                inode->data_queued = false;
                inode->data_running = true;
            }

            bool retry = false;
            try {
                auto snapshot = snapshot_data(inode);
                replay_data(inode, std::move(snapshot));
            } catch (const std::exception& e) {
                ++backend_failures;
                retry = retryable_backend_error(e);
                Log::debug(std::string("FUSE async data publication ") +
                           (retry ? "retry" : "failed") + " inode=" +
                           std::to_string(inode->id) + " error=" + e.what());
                if (!retry) {
                    int code = EIO;
                    if (const auto* fs_error = dynamic_cast<const FsError*>(&e))
                        code = fs_error->code();
                    std::lock_guard lock(inode->mutex);
                    inode->backend_error = code;
                }
            }

            {
                std::lock_guard lock(inode->mutex);
                inode->data_running = false;
                const bool still_requested =
                    inode->requested_data_sequence > inode->published_data_sequence;
                if ((retry || still_requested) && !inode->unconfirmed_data_entry)
                    inode->data_deferred = true;
            }
            --active_data;
            data_cv.notify_all();
            if (retry)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            admit_deferred();
        }
    }

    void broker_loop(size_t index, std::stop_token stop) {
        auto& queue = broker[index];
        while (!stop.stop_requested() && !stopping.load()) {
            BrokerTask task;
            {
                std::unique_lock lock(queue.mutex);
                queue.cv.wait(lock, stop, [&] { return !queue.tasks.empty() || stopping.load(); });
                if (stop.stop_requested() || stopping.load())
                    break;
                task = std::move(queue.tasks.front());
                queue.tasks.pop_front();
            }
            if (!task.cancelled->load(std::memory_order_relaxed))
                task.fn(task.deadline, *task.cancelled);
            --broker_pending;
        }
    }

    struct RecoveryPaths {
        std::string current;
        std::optional<std::string> published;
        uint64_t namespace_sequence{};
    };

    RecoveryPaths recovery_paths(uint64_t inode, const JournalInode& descriptor,
                                 const JournalRecovery& recovery) const {
        RecoveryPaths result{descriptor.current_path, descriptor.published_path,
                             descriptor.namespace_sequence};
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (!contains_inode(op.affected, inode) && !contains_inode(op.removed, inode))
                continue;
            result.current = transformed_path(std::move(result.current), inode, op);
            result.namespace_sequence = std::max(result.namespace_sequence, sequence);
            if (recovery.namespace_published.contains(sequence) ||
                recovery.namespace_done.contains(sequence)) {
                auto published = transformed_path(result.published.value_or(std::string{}), inode, op);
                if (published.empty())
                    result.published.reset();
                else
                    result.published = std::move(published);
            }
        }
        return result;
    }

    size_t pending_data_count(const JournalRecovery& recovery, uint64_t inode,
                              uint64_t through = std::numeric_limits<uint64_t>::max()) const {
        auto found = recovery.data_ops.find(inode);
        if (found == recovery.data_ops.end())
            return 0;
        auto done = recovery.data_done.find(inode);
        const uint64_t completed = done == recovery.data_done.end() ? 0 : done->second;
        return static_cast<size_t>(std::count_if(found->second.begin(), found->second.end(),
            [&](const DataOp& op) { return op.sequence > completed && op.sequence <= through; }));
    }

    void reconcile_recovery(JournalRecovery& recovery, const MetadataSnapshot& snapshot) {
        // A backend mutation may have committed immediately before the process
        // stopped, after its durable "published" marker but before the local
        // journal could retire it.  Only retire work once the immutable decoded
        // metadata snapshot actually demonstrates the accepted effect.
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (recovery.namespace_done.contains(sequence) ||
                !recovery.namespace_published.contains(sequence))
                continue;
            if (!namespace_effect_confirmed(op, snapshot))
                continue;
            journal_namespace_done(sequence);
            recovery.namespace_done.insert(sequence);
        }

        for (const auto& [inode, published] : recovery.data_published) {
            const auto done = recovery.data_done.find(inode);
            if (done != recovery.data_done.end() && done->second >= published.first)
                continue;
            auto descriptor = recovery.inodes.find(inode);
            if (descriptor == recovery.inodes.end())
                throw std::runtime_error("FUSE journal data publication has no inode descriptor");
            auto paths = recovery_paths(inode, descriptor->second, recovery);
            if (!paths.published)
                continue;
            auto entry = snapshot_entry(snapshot, *paths.published);
            if (!entry || !same_file_content(*entry, published.second))
                continue;
            const auto retired = pending_data_count(recovery, inode, published.first);
            if (!retired)
                continue;
            (void)journal_data_done(inode, published.first, retired);
            recovery.data_done[inode] = published.first;
        }
    }

    static void apply_pending_namespace_metadata(Inode& inode, uint64_t id,
                                                 const JournalRecovery& recovery) {
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (recovery.namespace_done.contains(sequence) || !contains_inode(op.affected, id))
                continue;
            switch (op.kind) {
            case NamespaceOp::Kind::chmod:
                inode.visible.mode = op.mode & 07777;
                inode.visible.ctime_ns = op.ctime_ns;
                ++inode.visible.version;
                break;
            case NamespaceOp::Kind::chown:
                if (op.set_uid) inode.visible.uid = op.uid;
                if (op.set_gid) inode.visible.gid = op.gid;
                inode.visible.ctime_ns = op.ctime_ns;
                ++inode.visible.version;
                break;
            case NamespaceOp::Kind::utimens:
                inode.visible.mtime_ns = op.mtime_ns;
                inode.visible.ctime_ns = op.ctime_ns;
                ++inode.visible.version;
                break;
            case NamespaceOp::Kind::mkdir:
            case NamespaceOp::Kind::create:
            case NamespaceOp::Kind::rmdir:
            case NamespaceOp::Kind::unlink:
            case NamespaceOp::Kind::rename:
                break;
            }
        }
    }

    static void apply_pending_data_metadata(Inode& inode, const std::vector<DataOp>& operations) {
        for (const auto& op : operations) {
            if (op.kind == DataOp::Kind::truncate)
                inode.visible.size = op.size;
            else
                inode.visible.size = std::max(inode.visible.size, op.offset + op.length);
            inode.visible.mtime_ns = op.mtime_ns;
            inode.visible.ctime_ns = op.ctime_ns;
        }
    }

    void quarantine_spool_tail(const std::filesystem::path& path, uint64_t keep, uint64_t size) {
        if (size <= keep)
            return;
        const auto orphan = path.string() + ".orphan." + std::to_string(unix_ms());
        int source = ::open(path.c_str(), O_RDONLY);
        if (source < 0)
            throw FsError(errno, "cannot open FUSE spool for orphan recovery");
        int target = ::open(orphan.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (target < 0) {
            const int saved = errno;
            ::close(source);
            throw FsError(saved, "cannot preserve orphaned FUSE spool bytes");
        }
        try {
            Bytes buffer(256 * 1024);
            uint64_t offset = keep;
            while (offset < size) {
                const auto chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - offset));
                if (pread_exact(source, {buffer.data(), chunk}, offset) != chunk)
                    throw FsError(EIO, "cannot read orphaned FUSE spool bytes");
                write_exact(target, {buffer.data(), chunk});
                offset += chunk;
            }
            fsync_fd(target, "cannot sync preserved orphaned FUSE spool bytes");
            ::close(source);
            source = -1;
            if (::close(target) != 0)
                throw FsError(errno, "cannot close preserved orphaned FUSE spool bytes");
            target = -1;
            int original = ::open(path.c_str(), O_RDWR);
            if (original < 0)
                throw FsError(errno, "cannot trim FUSE spool after orphan preservation");
            if (::ftruncate(original, static_cast<off_t>(keep)) != 0) {
                const int saved = errno;
                ::close(original);
                throw FsError(saved, "cannot trim FUSE spool after orphan preservation");
            }
            fsync_fd(original, "cannot sync trimmed FUSE spool");
            ::close(original);
            sync_directory(spool_dir);
            Log::warn("preserved orphaned FUSE spool tail path=" + path.string() +
                      " bytes=" + std::to_string(size - keep) + " as=" + orphan);
        } catch (...) {
            if (source >= 0) ::close(source);
            if (target >= 0) ::close(target);
            throw;
        }
    }

    void recover_spool(const std::shared_ptr<Inode>& inode, const std::vector<DataOp>& operations) {
        uint64_t required = 0;
        bool has_write = false;
        for (const auto& op : operations) {
            if (op.kind != DataOp::Kind::write)
                continue;
            has_write = true;
            if (op.spool_offset > std::numeric_limits<uint64_t>::max() - op.length)
                throw std::runtime_error("FUSE journal spool range overflow");
            required = std::max(required, op.spool_offset + op.length);
        }
        if (!has_write)
            return;
        inode->spool_path = spool_dir / ("inode-" + std::to_string(inode->id) + ".spool");
        int fd = ::open(inode->spool_path.c_str(), O_RDWR);
        if (fd < 0)
            throw std::runtime_error("FUSE journal references missing spool " + inode->spool_path.string());
        struct stat statbuf {};
        if (::fstat(fd, &statbuf) != 0) {
            const int saved = errno;
            ::close(fd);
            throw FsError(saved, "cannot stat recovered FUSE spool");
        }
        const auto size = static_cast<uint64_t>(statbuf.st_size);
        if (size < required) {
            ::close(fd);
            throw std::runtime_error("FUSE spool is shorter than its durable operation journal");
        }
        ::close(fd);
        if (size > required)
            quarantine_spool_tail(inode->spool_path, required, size);
        // Recovery validates durable spool state but does not retain one file
        // descriptor per dirty inode. Replay/read paths open the spool lazily.
        inode->spool_fd = -1;
        inode->spool_end = required;
    }

    void preserve_unreferenced_spool(const std::filesystem::path& path, uint64_t size,
                                    std::string_view reason) {
        const auto preserved = path.string() + ".orphan." + std::to_string(unix_ms()) + "." +
                               std::to_string(getpid());
        if (::rename(path.c_str(), preserved.c_str()) != 0)
            throw FsError(errno, "cannot preserve unreferenced FUSE spool");
        sync_directory(spool_dir);
        Log::warn("preserved unreferenced FUSE spool path=" + path.string() +
                  " bytes=" + std::to_string(size) + " as=" + preserved +
                  " reason=" + std::string(reason) +
                  "; no durable journal attribution exists, so bytes were not replayed");
    }

    void validate_recovery_spools(const JournalRecovery& recovery) {
        std::error_code ec;
        bool directory_changed = false;
        for (const auto& entry : std::filesystem::directory_iterator(spool_dir, ec)) {
            if (ec)
                throw std::runtime_error("cannot enumerate FUSE spool directory: " + ec.message());
            if (!entry.is_regular_file())
                continue;
            const auto name = entry.path().filename().string();
            if (!name.starts_with("inode-") || !name.ends_with(".spool"))
                continue;
            const auto number = name.substr(6, name.size() - 6 - 6);
            uint64_t id = 0;
            try {
                size_t consumed = 0;
                id = std::stoull(number, &consumed);
                if (consumed != number.size())
                    throw std::invalid_argument("trailing");
            } catch (...) {
                if (entry.file_size() == 0) {
                    std::filesystem::remove(entry.path(), ec);
                    if (ec)
                        throw std::runtime_error("cannot remove empty unrecognised FUSE spool: " +
                                                 ec.message());
                    directory_changed = true;
                    ec.clear();
                    continue;
                }
                const auto size = entry.file_size();
                preserve_unreferenced_spool(entry.path(), size, "unrecognised spool filename");
                directory_changed = true;
                continue;
            }
            if (recovery.data_history_inodes.contains(id)) {
                if (pending_data_count(recovery, id) > 0)
                    continue;
                // A crash may occur after the durable data_done record but
                // before journal compaction/truncation of its spool. The done
                // watermark proves these bytes are no longer part of recovery.
                std::filesystem::remove(entry.path(), ec);
                if (ec)
                    throw std::runtime_error("cannot remove retired FUSE spool: " + ec.message());
                directory_changed = true;
                continue;
            }
            const auto size = entry.file_size();
            if (!size) {
                std::filesystem::remove(entry.path(), ec);
                if (ec)
                    throw std::runtime_error("cannot remove empty FUSE spool: " + ec.message());
                directory_changed = true;
                ec.clear();
                continue;
            }
            preserve_unreferenced_spool(entry.path(), size,
                                        "operation journal has no history for inode");
            directory_changed = true;
        }
        if (ec)
            throw std::runtime_error("cannot enumerate FUSE spool directory: " + ec.message());
        if (directory_changed)
            sync_directory(spool_dir);
    }

    MetadataSnapshotView wait_for_initial_namespace() {
        bool announced = false;
        for (;;) {
            try {
                return fs.local_snapshot_view();
            } catch (const MetadataNotReady& error) {
                if (!announced) {
                    Log::debug("FUSE waiting for initial metadata: " + std::string(error.what()));
                    announced = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void initialise_namespace(JournalRecovery recovery) {
        auto view = wait_for_initial_namespace();
        const auto& snapshot = *view.snapshot;
        std::filesystem::create_directories(spool_dir);
        reconcile_recovery(recovery, snapshot);
        validate_recovery_spools(recovery);

        std::set<uint64_t> needed;
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (recovery.namespace_done.contains(sequence))
                continue;
            needed.insert(op.affected.begin(), op.affected.end());
            needed.insert(op.removed.begin(), op.removed.end());
        }
        for (const auto& [inode, operations] : recovery.data_ops) {
            const auto done = recovery.data_done[inode];
            if (std::any_of(operations.begin(), operations.end(),
                            [&](const DataOp& op) { return op.sequence > done; }))
                needed.insert(inode);
        }

        std::map<std::string, uint64_t, std::less<>> descriptor_by_published_path;
        std::map<uint64_t, RecoveryPaths> recovered_paths;
        for (auto id : needed) {
            auto descriptor = recovery.inodes.find(id);
            if (descriptor == recovery.inodes.end())
                throw std::runtime_error("FUSE operation journal references an inode without descriptor");
            auto state = recovery_paths(id, descriptor->second, recovery);
            if (state.published)
                descriptor_by_published_path[canonical_path(*state.published)] = id;
            recovered_paths[id] = std::move(state);
        }

        next_inode = std::max<uint64_t>(2, recovery.max_inode + 1);
        next_namespace_sequence = std::max<uint64_t>(1, recovery.max_namespace_sequence + 1);

        std::lock_guard lock(namespace_mutex);
        for (const auto& [path, entry] : snapshot.entries) {
            const auto key = canonical_path(path);
            if (snapshot_path_shadowed(key, recovery))
                continue;
            auto mapped = descriptor_by_published_path.find(key);
            if (mapped != descriptor_by_published_path.end())
                continue;
            auto inode = std::make_shared<Inode>();
            inode->id = path == "/" ? 1 : next_inode++;
            inode->base = entry;
            inode->visible = entry;
            inode->current_path = path;
            inode->published_path = path;
            paths[key] = inode;
            inodes[inode->id] = std::move(inode);
        }

        for (auto id : needed) {
            const auto& descriptor = recovery.inodes.at(id);
            const auto& rpaths = recovered_paths.at(id);
            auto inode = std::make_shared<Inode>();
            inode->id = id;
            inode->journal_epoch = journal_epoch.load(std::memory_order_relaxed);
            inode->namespace_sequence = rpaths.namespace_sequence;
            inode->current_path = rpaths.current;
            inode->published_path = rpaths.published;
            inode->next_data_sequence = descriptor.next_data_sequence;

            const FsEntry* committed = nullptr;
            if (rpaths.published)
                committed = snapshot_entry(snapshot, *rpaths.published);
            inode->base = committed ? *committed : descriptor.base;
            inode->visible = inode->base;
            if (!committed && descriptor.visible.type == inode->base.type)
                inode->visible = descriptor.visible;
            apply_pending_namespace_metadata(*inode, id, recovery);

            auto operations = recovery.data_ops.find(id);
            const auto done = recovery.data_done[id];
            if (operations != recovery.data_ops.end()) {
                for (const auto& op : operations->second) {
                    inode->next_data_sequence = std::max(inode->next_data_sequence, op.sequence + 1);
                    if (op.sequence > done)
                        inode->data_ops.push_back(op);
                }
            }
            apply_pending_data_metadata(*inode, inode->data_ops);
            if (!inode->data_ops.empty()) {
                inode->durable_data_sequence = inode->data_ops.back().sequence;
                inode->requested_data_sequence = inode->durable_data_sequence;
            } else {
                inode->durable_data_sequence = done;
            }
            auto published = recovery.data_published.find(id);
            if (published != recovery.data_published.end() && published->second.first > done) {
                inode->published_data_sequence = published->second.first;
                inode->unconfirmed_data_sequence = published->second.first;
                inode->unconfirmed_data_entry = published->second.second;
                inode->base = published->second.second;
            } else {
                inode->published_data_sequence = done;
            }
            inode->requested_namespace_sequence = inode->namespace_sequence;
            recover_spool(inode, inode->data_ops);
            if (!inode->current_path.empty()) {
                const auto key = canonical_path(inode->current_path);
                auto existing = paths.find(key);
                if (existing != paths.end() && existing->second->id != inode->id)
                    throw std::runtime_error("FUSE journal recovery produced duplicate namespace path " + key);
                paths[key] = inode;
            }
            inodes[id] = inode;
        }

        if (!paths.contains(canonical_path("/")))
            throw std::runtime_error("FUSE frontend cannot initialise without namespace root");

        // Journal compaction intentionally drops completed history, so the
        // first sequence present after restart need not be 1. Every sequence
        // before the first unpublished operation is already known published.
        uint64_t published_prefix = recovery.max_namespace_sequence;
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            (void)op;
            if (!recovery.namespace_published.contains(sequence) &&
                !recovery.namespace_done.contains(sequence)) {
                published_prefix = sequence > 0 ? sequence - 1 : 0;
                break;
            }
        }
        published_namespace_sequence.store(published_prefix, std::memory_order_release);

        {
            std::lock_guard queue_lock(namespace_queue_mutex);
            for (const auto& [sequence, op] : recovery.namespace_ops) {
                if (recovery.namespace_done.contains(sequence))
                    continue;
                if (recovery.namespace_published.contains(sequence))
                    namespace_unconfirmed.push_back(op);
                else
                    namespace_queue.push_back(op);
            }
        }
        refreshed_namespace_revision.store(view.namespace_revision, std::memory_order_release);
        if (recovery.pending_operations) {
            Log::warn("recovered durable FUSE operations pending=" +
                      std::to_string(durable_pending_operations.load(std::memory_order_relaxed)) +
                      " namespace=" + std::to_string(namespace_queue.size() + namespace_unconfirmed.size()) +
                      " inodes=" + std::to_string(needed.size()));
        }
        reset_journal_if_idle();
    }

    void resume_recovered_data() {
        std::vector<std::shared_ptr<Inode>> recovered;
        {
            std::lock_guard lock(namespace_mutex);
            for (const auto& [_, inode] : inodes) {
                std::lock_guard inode_lock(inode->mutex);
                if (!inode->data_ops.empty() &&
                    inode->requested_data_sequence > inode->published_data_sequence &&
                    !inode->unconfirmed_data_entry)
                    recovered.push_back(inode);
            }
        }
        for (const auto& inode : recovered)
            request_data_publication(inode);
    }

    void confirm_namespace_from_snapshot(const MetadataSnapshot& snapshot) {
        while (true) {
            NamespaceOp op;
            {
                std::lock_guard queue_lock(namespace_queue_mutex);
                if (namespace_unconfirmed.empty())
                    return;
                op = namespace_unconfirmed.front();
            }
            if (!namespace_effect_confirmed(op, snapshot))
                return;
            journal_namespace_done(op.sequence);
            {
                std::lock_guard queue_lock(namespace_queue_mutex);
                if (!namespace_unconfirmed.empty() &&
                    namespace_unconfirmed.front().sequence == op.sequence)
                    namespace_unconfirmed.pop_front();
            }
            namespace_cv.notify_all();
        }
    }

    bool confirm_data_from_snapshot(const std::shared_ptr<Inode>& inode,
                                    const MetadataSnapshot& snapshot) {
        bool more = false;
        bool spool_clean = true;
        bool journal_idle = false;
        {
            // Serialise observation, durable retirement and in-memory retirement
            // on the inode. replay_data() may race a namespace-facing refresh;
            // without this lock spanning journal_data_done(), both observers
            // could retire the same durable operation prefix.
            std::lock_guard lock(inode->mutex);
            if (!inode->unconfirmed_data_entry || !inode->unconfirmed_data_sequence)
                return false;

            const auto target = inode->unconfirmed_data_sequence;
            const auto expected = *inode->unconfirmed_data_entry;
            const auto path = inode->published_path;
            const auto retired = static_cast<size_t>(std::count_if(
                inode->data_ops.begin(), inode->data_ops.end(),
                [&](const DataOp& op) { return op.sequence <= target; }));
            if (!retired)
                return false;
            if (path) {
                auto entry = snapshot_entry(snapshot, *path);
                if (!entry || !same_file_content(*entry, expected))
                    return false;
            }

            journal_idle = journal_data_done(inode->id, target, retired);
            if (path) {
                auto entry = snapshot_entry(snapshot, *path);
                if (entry)
                    inode->base = *entry;
            } else {
                inode->base = expected;
            }
            inode->data_ops.erase(
                std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                               [&](const DataOp& op) { return op.sequence <= target; }),
                inode->data_ops.end());
            inode->unconfirmed_data_entry.reset();
            inode->unconfirmed_data_sequence = 0;
            if (inode->data_ops.empty() && !inode->durability_pending)
                spool_clean = retire_spool_locked(*inode);
            more = inode->requested_data_sequence > inode->published_data_sequence;
            if (more)
                inode->data_deferred = true;
        }
        if (journal_idle && spool_clean)
            reset_journal_if_idle();
        if (more)
            admit_deferred();
        data_cv.notify_all();
        return true;
    }

    void confirm_data_from_snapshot(const MetadataSnapshot& snapshot) {
        std::vector<std::shared_ptr<Inode>> candidates;
        {
            std::lock_guard lock(namespace_mutex);
            candidates.reserve(inodes.size());
            for (const auto& [_, inode] : inodes) {
                std::lock_guard inode_lock(inode->mutex);
                if (inode->unconfirmed_data_entry)
                    candidates.push_back(inode);
            }
        }
        for (const auto& inode : candidates)
            (void)confirm_data_from_snapshot(inode, snapshot);
    }

    bool have_unconfirmed_namespace() {
        std::lock_guard lock(namespace_queue_mutex);
        return !namespace_unconfirmed.empty();
    }

    void refresh_namespace_if_stale() {
        const auto current_revision = refreshed_namespace_revision.load(std::memory_order_acquire);
        const bool pending_confirmation = have_unconfirmed_namespace();
        if (fs.available_namespace_revision() <= current_revision && !pending_confirmation)
            return;

        // A metadata-generation notice is only evidence that a newer snapshot
        // exists somewhere in the cluster. It is not permission for a kernel
        // getattr/readdir to perform quorum I/O. Adopt only an immutable snapshot
        // which MetadataManager has already obtained and decoded; metadata repair
        // is responsible for making newer generations locally available.
        std::lock_guard refresh_lock(refresh_mutex);
        {
            std::lock_guard queue_lock(namespace_queue_mutex);
            // Local FUSE namespace operations are already reflected optimistically
            // in paths/inodes. Do not race a backend publication with a snapshot
            // adoption; a subsequent namespace-facing request will retry.
            if (namespace_inflight || !namespace_queue.empty())
                return;
        }

        const auto available = fs.available_snapshot_view();
        if (!available)
            return;
        const auto& view = *available;
        const auto& snapshot = *view.snapshot;

        // A backend publication is not retired merely because its RPC returned.
        // First observe the effect in MetadataManager's immutable decoded view.
        // This prevents a concurrently available older view from resurrecting a
        // rename/unlink or hiding a locally acknowledged mkdir/create.
        confirm_namespace_from_snapshot(snapshot);
        confirm_data_from_snapshot(snapshot);
        if (have_unconfirmed_namespace())
            return;
        if (view.namespace_revision <=
            refreshed_namespace_revision.load(std::memory_order_acquire))
            return;

        std::lock_guard lock(namespace_mutex);
        {
            // Close the admission race between the earlier queue check and
            // taking namespace_mutex. Admissions publish their queue entry
            // before releasing namespace_mutex, so observing a non-empty queue
            // here means this snapshot predates accepted local state.
            std::lock_guard queue_lock(namespace_queue_mutex);
            if (namespace_inflight || !namespace_queue.empty() || !namespace_unconfirmed.empty())
                return;
        }
        std::set<std::string, std::less<>> seen;
        for (const auto& [path, entry] : snapshot.entries) {
            const auto key = canonical_path(path);
            seen.insert(key);
            auto found = paths.find(key);
            if (found == paths.end()) {
                auto inode = std::make_shared<Inode>();
                inode->id = path == "/" ? 1 : next_inode++;
                inode->base = entry;
                inode->visible = entry;
                inode->current_path = path;
                inode->published_path = path;
                paths[key] = inode;
                inodes[inode->id] = std::move(inode);
                continue;
            }
            auto inode = found->second;
            std::lock_guard inode_lock(inode->mutex);
            const bool content_changed = !same_file_content(inode->base, entry);
            // Metadata snapshots do not carry a stable distributed inode id.
            // A dirty/open inode whose name now denotes replacement content
            // must be detached from that pathname before any queued publication
            // can run; otherwise old writes can be committed into the new file.
            // For a read-only descriptor, a non-advancing entry version plus
            // changed content is the replacement/rename-over signature produced
            // by current filesystem metadata. Advancing versions remain ordinary
            // in-place content changes and stay attached to the same open inode.
            const bool replaced_open_inode =
                content_changed &&
                (!inode->data_ops.empty() || inode->durability_pending ||
                 inode->unconfirmed_data_entry ||
                 (inode->open_handles != 0 && entry.version <= inode->base.version));
            if (replaced_open_inode && path != "/") {
                inode->published_path.reset();
                auto replacement = std::make_shared<Inode>();
                replacement->id = next_inode++;
                replacement->base = entry;
                replacement->visible = entry;
                replacement->current_path = path;
                replacement->published_path = path;
                found->second = replacement;
                inodes[replacement->id] = std::move(replacement);
                continue;
            }
            inode->published_path = path;
            if (inode->data_ops.empty() && !inode->durability_pending &&
                !inode->unconfirmed_data_entry) {
                inode->base = entry;
                inode->visible = entry;
                inode->admitted_size = entry.size;
            }
        }

        for (auto it = paths.begin(); it != paths.end();) {
            if (it->first == canonical_path("/") || seen.contains(it->first)) {
                ++it;
                continue;
            }
            auto inode = it->second;
            std::lock_guard inode_lock(inode->mutex);
            // Remote unlink has the same POSIX name semantics as a local unlink:
            // remove the directory edge immediately. Open handles and dirty
            // state retain the detached inode object, but never ownership of the
            // old pathname and therefore cannot resurrect it on publication.
            inode->published_path.reset();
            it = paths.erase(it);
        }
        refreshed_namespace_revision.store(view.namespace_revision, std::memory_order_release);
    }

    void start() {
        auto recovery = load_journal();
        initialise_namespace(std::move(recovery));

        // Local write durability is independent of distributed publication. It
        // must be available before broker write workers can accept callbacks.
        durability_worker = std::jthread([this](std::stop_token stop) { durability_loop(stop); });

        // Guarantee at least one independent worker for each operation class,
        // then distribute the remaining budget toward reads/writes/lookups.
        const std::array<size_t, 12> preference{2, 3, 2, 3, 2, 0, 2, 3, 0, 1, 4, 5};
        size_t workers = 0;
        for (size_t i = 0; i < broker.size() && workers < config.request_workers; ++i, ++workers)
            broker[i].workers.emplace_back([this, i](std::stop_token stop) { broker_loop(i, stop); });
        size_t preference_index = 0;
        while (workers < config.request_workers) {
            const auto index = preference[preference_index++ % preference.size()];
            broker[index].workers.emplace_back([this, index](std::stop_token stop) { broker_loop(index, stop); });
            ++workers;
        }

        namespace_worker = std::jthread([this](std::stop_token stop) { namespace_loop(stop); });
        data_workers.reserve(config.commit_workers);
        for (size_t i = 0; i < config.commit_workers; ++i)
            data_workers.emplace_back([this](std::stop_token stop) { data_loop(stop); });

        // Recovery is reconstructed before worker startup so kernel-visible state
        // is complete before the frontend is exposed. Resume asynchronous
        // convergence only after all workers are available.
        resume_recovered_data();
        namespace_cv.notify_all();
    }

    void stop() {
        if (stopping.exchange(true))
            return;
        namespace_cv.notify_all();
        data_cv.notify_all();
        publication_cv.notify_all();
        for (auto& queue : broker)
            queue.cv.notify_all();
        if (namespace_worker.joinable()) namespace_worker.request_stop();
        for (auto& worker : data_workers) worker.request_stop();
        for (auto& queue : broker)
            for (auto& worker : queue.workers) worker.request_stop();
        if (namespace_worker.joinable()) namespace_worker.join();
        for (auto& worker : data_workers) if (worker.joinable()) worker.join();
        for (auto& queue : broker) {
            queue.cv.notify_all();
            for (auto& worker : queue.workers) if (worker.joinable()) worker.join();
        }

        // Broker writers wait for their durability tickets. Keep the durability
        // coordinator alive until all broker workers have returned, then drain
        // any final admitted batch before stopping it.
        if (durability_worker.joinable()) {
            durability_worker.request_stop();
            durability_cv.notify_all();
            durability_worker.join();
        }
    }
};

FuseFrontend::FuseFrontend(FileSystem& filesystem, FuseConfig config)
    : state_(std::make_unique<State>(filesystem, std::move(config))) {
    state_->start();
}

FuseFrontend::~FuseFrontend() {
    stop();
}

std::chrono::milliseconds FuseFrontend::timeout_for(FuseOperationClass operation) const {
    switch (operation) {
    case FuseOperationClass::lookup: return state_->config.timeouts.lookup;
    case FuseOperationClass::namespace_mutation: return state_->config.timeouts.namespace_mutation;
    case FuseOperationClass::read: return state_->config.timeouts.read;
    case FuseOperationClass::write: return state_->config.timeouts.write;
    case FuseOperationClass::sync: return state_->config.timeouts.sync;
    case FuseOperationClass::lifecycle: return state_->config.timeouts.lifecycle;
    }
    return state_->config.absolute_request_timeout;
}

std::chrono::milliseconds FuseFrontend::absolute_timeout() const {
    return state_->config.absolute_request_timeout;
}

const FuseConfig& FuseFrontend::config() const {
    return state_->config;
}

bool FuseFrontend::submit_task(FuseOperationClass operation, Clock::time_point deadline,
                               std::shared_ptr<std::atomic_bool> cancelled,
                               std::function<void(Clock::time_point, std::atomic_bool&)> fn) {
    auto current = state_->broker_pending.load(std::memory_order_relaxed);
    while (true) {
        if (current >= state_->config.max_pending_requests)
            return false;
        if (state_->broker_pending.compare_exchange_weak(current, current + 1))
            break;
    }
    auto& queue = state_->broker[State::class_index(operation)];
    {
        std::lock_guard lock(queue.mutex);
        if (state_->stopping.load()) {
            --state_->broker_pending;
            return false;
        }
        queue.tasks.push_back(State::BrokerTask{deadline, std::move(cancelled), std::move(fn)});
    }
    queue.cv.notify_one();
    return true;
}

void FuseFrontend::note_timeout() {
    ++state_->timed_out_requests;
}

FuseEntryAttributes FuseFrontend::getattr(std::string_view path) {
    const auto requested = std::string(path);
    return dispatch(FuseOperationClass::lookup, [this, requested](Clock::time_point deadline,
                                                                  std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        std::shared_ptr<State::Inode> inode;
        {
            std::lock_guard lock(state_->namespace_mutex);
            inode = state_->resolve_locked(requested);
        }
        std::lock_guard inode_lock(inode->mutex);
        check_deadline(deadline, cancelled);
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "asynchronous backend error");
        return fuse_attributes(inode->visible);
    });
}

std::vector<std::pair<std::string, FuseEntryAttributes>> FuseFrontend::readdir(std::string_view path) {
    const auto requested = canonical_path(path);
    return dispatch(FuseOperationClass::lookup, [this, requested](Clock::time_point deadline,
                                                                  std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        std::vector<std::pair<std::string, FuseEntryAttributes>> out;
        std::lock_guard lock(state_->namespace_mutex);
        auto parent = state_->resolve_locked(requested);
        {
            std::lock_guard inode_lock(parent->mutex);
            if (parent->visible.type != EntryType::directory)
                throw FsError(ENOTDIR, "not directory");
        }
        for (const auto& [_, inode] : state_->paths) {
            check_deadline(deadline, cancelled);
            std::lock_guard inode_lock(inode->mutex);
            if (inode->current_path == "/" || parent_path(inode->current_path) != parent->current_path)
                continue;
            out.push_back({base_name(inode->current_path), fuse_attributes(inode->visible)});
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return out;
    });
}

void FuseFrontend::mkdir(std::string_view path, uint32_t mode, uint32_t uid, uint32_t gid) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested, mode, uid, gid](Clock::time_point deadline,
                                               std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available())
            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            if (state_->paths.contains(requested))
                throw FsError(EEXIST, "exists");
            state_->require_parent_locked(requested);
            check_deadline(deadline, cancelled);
            auto inode = std::make_shared<State::Inode>();
            inode->id = state_->next_inode++;
            inode->visible.type = EntryType::directory;
            inode->visible.mode = mode & 07777;
            inode->visible.uid = uid;
            inode->visible.gid = gid;
            const auto now = wall_time_ns();
            inode->visible.ctime_ns = inode->visible.mtime_ns = now;
            inode->base = inode->visible;
            inode->admitted_size = inode->visible.size;
            inode->current_path = requested;
            inode->namespace_sequence = state_->next_namespace_sequence++;

            op.kind = State::NamespaceOp::Kind::mkdir;
            op.sequence = inode->namespace_sequence;
            op.from = requested;
            op.mode = mode;
            op.uid = uid;
            op.gid = gid;
            op.ctime_ns = now;
            op.affected = {inode->id};

            // Descriptor + operation are fsynced before the optimistic namespace
            // becomes visible to the kernel and before success can be returned.
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            state_->paths[requested] = inode;
            state_->inodes[inode->id] = inode;
            state_->enqueue_namespace(std::move(op));
        }
    });
}

void FuseFrontend::rmdir(std::string_view path) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested](Clock::time_point deadline, std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available())
            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            auto inode = state_->resolve_locked(requested);
            std::lock_guard inode_lock(inode->mutex);
            if (inode->visible.type != EntryType::directory)
                throw FsError(ENOTDIR, "not directory");
            if (requested == "/")
                throw FsError(EBUSY, "cannot remove root");
            for (const auto& [_, candidate] : state_->paths) {
                if (candidate == inode) continue;
                std::lock_guard candidate_lock(candidate->mutex);
                if (parent_path(candidate->current_path) == inode->current_path)
                    throw FsError(ENOTEMPTY, "directory not empty");
            }
            check_deadline(deadline, cancelled);
            const auto sequence = state_->next_namespace_sequence++;
            op.kind = State::NamespaceOp::Kind::rmdir;
            op.sequence = sequence;
            op.from = requested;
            op.ctime_ns = wall_time_ns();
            op.affected = {inode->id};
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            inode->namespace_sequence = sequence;
            inode->current_path.clear();
            state_->paths.erase(requested);
            state_->enqueue_namespace(std::move(op));
        }
    });
}

void FuseFrontend::unlink(std::string_view path) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested](Clock::time_point deadline, std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available())
            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            auto inode = state_->resolve_locked(requested);
            std::lock_guard inode_lock(inode->mutex);
            if (inode->visible.type != EntryType::file)
                throw FsError(EISDIR, "directory");
            check_deadline(deadline, cancelled);
            const auto sequence = state_->next_namespace_sequence++;
            op.kind = State::NamespaceOp::Kind::unlink;
            op.sequence = sequence;
            op.from = requested;
            op.ctime_ns = wall_time_ns();
            op.affected = {inode->id};
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            inode->namespace_sequence = sequence;
            inode->current_path.clear();
            state_->paths.erase(requested);
            state_->enqueue_namespace(std::move(op));
        }
    });
}

void FuseFrontend::rename(std::string_view from, std::string_view to, bool noreplace) {
    const auto source = canonical_path(from);
    const auto destination = canonical_path(to);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, source, destination, noreplace](Clock::time_point deadline,
                                                     std::atomic_bool& cancelled) {
        if (source == destination)
            return;
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available())
            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            auto root = state_->resolve_locked(source);
            if (source == "/")
                throw FsError(EBUSY, "cannot rename root");
            state_->require_parent_locked(destination);
            if (destination.starts_with(source + "/"))
                throw FsError(EINVAL, "cannot move directory under itself");

            std::shared_ptr<State::Inode> displaced_inode;
            auto existing = state_->paths.find(destination);
            if (existing != state_->paths.end()) {
                if (noreplace)
                    throw FsError(EEXIST, "destination exists");
                displaced_inode = existing->second;
                {
                    std::scoped_lock pair_lock(root->mutex, displaced_inode->mutex);
                    if (root->visible.type != displaced_inode->visible.type)
                        throw FsError(root->visible.type == EntryType::directory ? ENOTDIR : EISDIR,
                                      "rename type mismatch");
                }
                {
                    std::lock_guard destination_lock(displaced_inode->mutex);
                    if (displaced_inode->visible.type == EntryType::directory) {
                        for (const auto& [_, candidate] : state_->paths) {
                            if (candidate == displaced_inode || candidate == root) continue;
                            std::lock_guard candidate_lock(candidate->mutex);
                            if (parent_path(candidate->current_path) == displaced_inode->current_path)
                                throw FsError(ENOTEMPTY, "destination directory not empty");
                        }
                    }
                }
            }

            std::vector<std::shared_ptr<State::Inode>> affected_inodes;
            for (const auto& [_, inode] : state_->paths) {
                std::lock_guard inode_lock(inode->mutex);
                if (under_path(inode->current_path, source))
                    affected_inodes.push_back(inode);
            }
            const auto sequence = state_->next_namespace_sequence++;
            op.kind = State::NamespaceOp::Kind::rename;
            op.sequence = sequence;
            op.from = source;
            op.to = destination;
            op.noreplace = noreplace;
            op.ctime_ns = wall_time_ns();
            op.affected.reserve(affected_inodes.size());
            {
                std::vector<std::shared_ptr<State::Inode>> journal_inodes = affected_inodes;
                if (displaced_inode)
                    journal_inodes.push_back(displaced_inode);
                std::sort(journal_inodes.begin(), journal_inodes.end(),
                          [](const auto& a, const auto& b) { return a->id < b->id; });
                journal_inodes.erase(
                    std::unique(journal_inodes.begin(), journal_inodes.end(),
                                [](const auto& a, const auto& b) { return a->id == b->id; }),
                    journal_inodes.end());
                std::vector<std::unique_lock<std::mutex>> inode_locks;
                inode_locks.reserve(journal_inodes.size());
                for (const auto& inode : journal_inodes)
                    inode_locks.emplace_back(inode->mutex);

                // Inode locks always precede journal admission throughout the
                // frontend. Keeping that order here avoids a rename/write
                // deadlock while still preventing journal compaction between
                // the descriptor set and the rename operation record.
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                for (const auto& inode : affected_inodes) {
                    state_->journal_inode_locked(inode);
                    op.affected.push_back(inode->id);
                }
                if (displaced_inode) {
                    state_->journal_inode_locked(displaced_inode);
                    op.removed.push_back(displaced_inode->id);
                }
                state_->journal_namespace_operation(op);
            }

            // Only after the complete operation is durable do we expose the
            // optimistic rename in the local inode/path overlay.
            if (displaced_inode) {
                std::lock_guard inode_lock(displaced_inode->mutex);
                displaced_inode->current_path.clear();
                displaced_inode->namespace_sequence = sequence;
                state_->paths.erase(destination);
            }
            for (const auto& inode : affected_inodes) {
                std::lock_guard inode_lock(inode->mutex);
                const auto old = inode->current_path;
                state_->paths.erase(canonical_path(old));
                inode->current_path = destination + old.substr(source.size());
                inode->namespace_sequence = sequence;
            }
            for (const auto& inode : affected_inodes) {
                std::lock_guard inode_lock(inode->mutex);
                state_->paths[canonical_path(inode->current_path)] = inode;
            }
            state_->enqueue_namespace(std::move(op));
        }
    });
}

void FuseFrontend::chmod(std::string_view path, uint32_t mode) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested, mode](Clock::time_point deadline, std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available()) throw FsError(EAGAIN, "namespace queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            auto inode = state_->resolve_locked(requested);
            std::lock_guard inode_lock(inode->mutex);
            const auto sequence = state_->next_namespace_sequence++;
            const auto now = wall_time_ns();
            op.kind = State::NamespaceOp::Kind::chmod;
            op.sequence = sequence;
            op.from = requested;
            op.mode = mode;
            op.ctime_ns = now;
            op.affected = {inode->id};
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            inode->visible.mode = mode & 07777;
            inode->visible.ctime_ns = now;
            ++inode->visible.version;
            inode->namespace_sequence = sequence;
            state_->enqueue_namespace(std::move(op));
        }
    });
}

void FuseFrontend::chown(std::string_view path, uint32_t uid, uint32_t gid, bool set_uid,
                         bool set_gid) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested, uid, gid, set_uid, set_gid](Clock::time_point deadline,
                                                           std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available()) throw FsError(EAGAIN, "namespace queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            auto inode = state_->resolve_locked(requested);
            std::lock_guard inode_lock(inode->mutex);
            const auto sequence = state_->next_namespace_sequence++;
            const auto now = wall_time_ns();
            op.kind = State::NamespaceOp::Kind::chown;
            op.sequence = sequence;
            op.from = requested;
            op.uid = uid;
            op.gid = gid;
            op.set_uid = set_uid;
            op.set_gid = set_gid;
            op.ctime_ns = now;
            op.affected = {inode->id};
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            if (set_uid) inode->visible.uid = uid;
            if (set_gid) inode->visible.gid = gid;
            inode->visible.ctime_ns = now;
            ++inode->visible.version;
            inode->namespace_sequence = sequence;
            state_->enqueue_namespace(std::move(op));
        }
    });
}

void FuseFrontend::utimens(std::string_view path, int64_t mtime_ns) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested, mtime_ns](Clock::time_point deadline, std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available()) throw FsError(EAGAIN, "namespace queue saturated");
        State::NamespaceOp op;
        {
            std::lock_guard lock(state_->namespace_mutex);
            auto inode = state_->resolve_locked(requested);
            std::lock_guard inode_lock(inode->mutex);
            const auto sequence = state_->next_namespace_sequence++;
            const auto now = wall_time_ns();
            op.kind = State::NamespaceOp::Kind::utimens;
            op.sequence = sequence;
            op.from = requested;
            op.mtime_ns = mtime_ns;
            op.ctime_ns = now;
            op.affected = {inode->id};
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            inode->visible.mtime_ns = mtime_ns;
            inode->visible.ctime_ns = now;
            ++inode->visible.version;
            inode->namespace_sequence = sequence;
            state_->enqueue_namespace(std::move(op));
        }
    });
}

FuseOpenHandle FuseFrontend::open(std::string_view path, bool readable, bool writable, bool append,
                                  bool truncate_on_open) {
    const auto requested = std::string(path);
    return dispatch(FuseOperationClass::lifecycle,
                    [this, requested, readable, writable, append, truncate_on_open](
                        Clock::time_point deadline, std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        std::shared_ptr<State::Inode> inode;
        {
            std::lock_guard lock(state_->namespace_mutex);
            inode = state_->resolve_locked(requested);
        }
        if (truncate_on_open)
            state_->wait_for_inode_durability(inode, deadline, cancelled);
        std::lock_guard inode_lock(inode->mutex);
        if (inode->visible.type != EntryType::file)
            throw FsError(EISDIR, "directory");
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "asynchronous backend error");
        if (truncate_on_open) {
            const auto seq = inode->next_data_sequence;
            const auto now = wall_time_ns();
            State::DataOp op;
            op.kind = State::DataOp::Kind::truncate;
            op.sequence = seq;
            op.size = 0;
            op.mtime_ns = now;
            op.ctime_ns = now;
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_data_operation(inode->id, op);
            }
            inode->next_data_sequence = seq + 1;
            inode->data_ops.push_back(op);
            inode->durable_data_sequence = seq;
            inode->visible.size = 0;
            inode->visible.mtime_ns = now;
            inode->visible.ctime_ns = now;
            inode->admitted_size = 0;
        }
        ++inode->open_handles;
        if (writable)
            state_->open_writers.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<FuseReadSession> read_session;
        if (readable) {
            read_session = std::make_shared<FuseReadSession>();
            read_session->inode = inode->id;
        }
        return FuseOpenHandle{inode->id, readable, writable, append, std::move(read_session)};
    });
}

FuseOpenHandle FuseFrontend::create(std::string_view path, uint32_t mode, uint32_t uid,
                                    uint32_t gid, bool readable, bool writable, bool append) {
    const auto requested = canonical_path(path);
    return dispatch(FuseOperationClass::namespace_mutation,
                    [this, requested, mode, uid, gid, readable, writable, append](
                        Clock::time_point deadline, std::atomic_bool& cancelled) {
        std::lock_guard accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available())
            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
        State::NamespaceOp op;
        std::shared_ptr<State::Inode> inode;
        {
            std::lock_guard lock(state_->namespace_mutex);
            if (state_->paths.contains(requested))
                throw FsError(EEXIST, "exists");
            state_->require_parent_locked(requested);
            inode = std::make_shared<State::Inode>();
            inode->id = state_->next_inode++;
            inode->visible.type = EntryType::file;
            inode->visible.mode = mode & 07777;
            inode->visible.uid = uid;
            inode->visible.gid = gid;
            const auto now = wall_time_ns();
            inode->visible.ctime_ns = inode->visible.mtime_ns = now;
            inode->base = inode->visible;
            inode->admitted_size = inode->visible.size;
            inode->current_path = requested;
            inode->namespace_sequence = state_->next_namespace_sequence++;
            inode->open_handles = 1;

            op.kind = State::NamespaceOp::Kind::create;
            op.sequence = inode->namespace_sequence;
            op.from = requested;
            op.mode = mode;
            op.uid = uid;
            op.gid = gid;
            op.ctime_ns = now;
            op.affected = {inode->id};
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_namespace_operation(op);
            }
            if (writable)
                state_->open_writers.fetch_add(1, std::memory_order_relaxed);
            state_->paths[requested] = inode;
            state_->inodes[inode->id] = inode;
            state_->enqueue_namespace(std::move(op));
        }
        std::shared_ptr<FuseReadSession> read_session;
        if (readable) {
            read_session = std::make_shared<FuseReadSession>();
            read_session->inode = inode->id;
        }
        return FuseOpenHandle{inode->id, readable, writable, append, std::move(read_session)};
    });
}

size_t FuseFrontend::read_impl(uint64_t inode_id, const std::shared_ptr<FuseReadSession>& read_session,
                               uint64_t offset, std::span<uint8_t> output) {
    return dispatch(FuseOperationClass::read,
                    [this, inode_id, read_session, offset, output](Clock::time_point deadline,
                                                     std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        FsEntry base;
        FsEntry visible;
        std::vector<State::DataOp> operations;
        std::filesystem::path spool_path;
        std::string logical_path;
        {
            std::lock_guard lock(inode->mutex);
            if (inode->visible.type != EntryType::file)
                throw FsError(EISDIR, "directory");
            base = inode->base;
            visible = inode->visible;
            operations = inode->data_ops;
            spool_path = inode->spool_path;
            logical_path = inode->current_path.empty() ? std::string("<unlinked>") : inode->current_path;
        }
        if (offset >= visible.size || output.empty())
            return size_t{0};
        const auto count = static_cast<size_t>(std::min<uint64_t>(output.size(), visible.size - offset));
        std::fill_n(output.data(), count, uint8_t{0});

        uint64_t base_limit = base.size;
        for (const auto& op : operations)
            if (op.kind == State::DataOp::Kind::truncate)
                base_limit = std::min(base_limit, op.size);

        if (offset < base_limit) {
            const auto base_count = static_cast<size_t>(
                std::min<uint64_t>(count, base_limit - offset));
            std::shared_ptr<ReadHandle> reader;
            if (read_session) {
                std::lock_guard session_lock(read_session->mutex);
                if (read_session->inode != inode_id)
                    throw FsError(EBADF, "FUSE read session inode mismatch");
                if (!read_session->reader || read_session->base_version != base.version) {
                    read_session->reader =
                        state_->fs.open_read(base, logical_path, false, FrameType::read_ahead);
                    read_session->base_version = base.version;
                }
                reader = read_session->reader;
            } else {
                reader = state_->fs.open_read(base, logical_path, false, FrameType::read_ahead);
            }
            size_t done = 0;
            while (done < base_count) {
                check_deadline(deadline, cancelled);
                auto n = reader->read(offset + done, {output.data() + done, base_count - done},
                                      deadline, &cancelled);
                if (!n) break;
                done += n;
            }
            if (done < base_count)
                throw FsError(Clock::now() >= deadline ? ETIMEDOUT : EIO,
                              "FUSE read source unavailable");
        }

        uint64_t virtual_size = base.size;
        ScopedFd spool;
        for (const auto& op : operations) {
            check_deadline(deadline, cancelled);
            if (op.kind == State::DataOp::Kind::truncate) {
                if (op.size < virtual_size) {
                    const auto zero_begin = std::max<uint64_t>(offset, op.size);
                    const auto zero_end = std::min<uint64_t>(offset + count, virtual_size);
                    if (zero_begin < zero_end)
                        std::fill(output.begin() + static_cast<ptrdiff_t>(zero_begin - offset),
                                  output.begin() + static_cast<ptrdiff_t>(zero_end - offset), 0);
                }
                virtual_size = op.size;
                continue;
            }
            const auto write_begin = op.offset;
            const auto write_end = op.offset + op.length;
            const auto copy_begin = std::max<uint64_t>(offset, write_begin);
            const auto copy_end = std::min<uint64_t>(offset + count, write_end);
            if (copy_begin < copy_end) {
                if (spool.get() < 0) {
                    const int fd = ::open(spool_path.c_str(), O_RDONLY);
                    if (fd < 0)
                        throw FsError(errno, "cannot open FUSE write spool for read");
                    spool.reset(fd);
                }
                const auto n = static_cast<size_t>(copy_end - copy_begin);
                const auto spool_offset = op.spool_offset + (copy_begin - write_begin);
                if (pread_exact(spool.get(),
                                {output.data() + static_cast<size_t>(copy_begin - offset), n},
                                spool_offset) != n)
                    throw FsError(EIO, "short FUSE spool read");
            }
            virtual_size = std::max(virtual_size, write_end);
        }

        // Immediate kernel demand becomes a high-priority hint in the existing
        // cache architecture. The foreground read above still has its own hard
        // deadline; the hint is useful for read-ahead and retries.
        if (!base.extents.empty()) {
            size_t first = base.extents.size();
            size_t last = 0;
            for (size_t i = 0; i < base.extents.size(); ++i) {
                const auto& extent = base.extents[i];
                if (extent.offset + extent.length <= offset) continue;
                if (extent.offset >= offset + count) break;
                first = std::min(first, i);
                last = i;
            }
            if (first < base.extents.size()) {
                std::function<void()> wake;
                {
                    std::lock_guard hint_lock(state_->hint_mutex);
                    state_->hint_states[inode_id] =
                        State::HintState{base, first, last, Clock::now() + state_->config.hint_lifetime};
                    wake = state_->hint_wake_callback;
                }
                if (wake) wake();
            }
        }
        return count;
    });
}

size_t FuseFrontend::read(uint64_t inode_id, uint64_t offset, std::span<uint8_t> output) {
    return read_impl(inode_id, {}, offset, output);
}

size_t FuseFrontend::read(const FuseOpenHandle& handle, uint64_t offset,
                          std::span<uint8_t> output) {
    if (!handle.readable)
        throw FsError(EBADF, "FUSE handle is not readable");
    return read_impl(handle.inode, handle.read_session, offset, output);
}

size_t FuseFrontend::write(uint64_t inode_id, uint64_t offset, std::span<const uint8_t> data,
                           bool append) {
    Bytes owned(data.begin(), data.end());
    return dispatch(FuseOperationClass::write,
                    [this, inode_id, offset, append, owned = std::move(owned)](
                        Clock::time_point deadline, std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        state_->throw_if_durability_poisoned();
        check_deadline(deadline, cancelled);

        auto ticket = std::make_shared<State::DurabilityTicket>();
        ticket->inode = inode;
        uint64_t spool_offset = 0;
        int fd = -1;
        {
            std::lock_guard lock(inode->mutex);
            if (inode->visible.type != EntryType::file)
                throw FsError(EISDIR, "directory");
            if (inode->backend_error)
                throw FsError(*inode->backend_error, "asynchronous backend error");
            if (!inode->durability_pending)
                inode->admitted_size = inode->visible.size;

            const auto target = append ? inode->admitted_size : offset;
            fd = state_->ensure_spool_locked(inode);
            spool_offset = inode->spool_end;
            const auto seq = inode->next_data_sequence;
            const auto now = wall_time_ns();
            ticket->op.kind = State::DataOp::Kind::write;
            ticket->op.sequence = seq;
            ticket->op.offset = target;
            ticket->op.length = static_cast<uint64_t>(owned.size());
            ticket->op.spool_offset = spool_offset;
            ticket->op.mtime_ns = now;
            ticket->op.ctime_ns = now;

            // Keep the inode descriptor alive across the asynchronous gap between
            // payload admission and the group-committed data-op frame. The
            // admission count prevents journal compaction in that gap.
            {
                std::lock_guard journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_inflight_admissions.fetch_add(1, std::memory_order_relaxed);
            }

            try {
                if (pwrite_exact(fd, owned, spool_offset) != owned.size())
                    throw FsError(errno ? errno : EIO, "short FUSE spool write");
            } catch (...) {
                {
                    std::lock_guard journal_admission(state_->journal_admission_mutex);
                    const auto previous = state_->journal_inflight_admissions.fetch_sub(
                        1, std::memory_order_relaxed);
                    if (!previous)
                        state_->journal_inflight_admissions.store(0, std::memory_order_relaxed);
                }
                // No later reservation can exist while this inode lock is held,
                // so the failed, unacknowledged tail can be removed exactly.
                if (::ftruncate(fd, static_cast<off_t>(spool_offset)) == 0) {
                    try {
                        fsync_fd(fd, "cannot sync rolled-back FUSE spool");
                    } catch (const std::exception& e) {
                        Log::error("cannot sync rolled-back FUSE spool inode=" +
                                   std::to_string(inode->id) + " error=" + e.what());
                    }
                }
                throw;
            }

            inode->spool_end += owned.size();
            inode->admitted_size =
                std::max<uint64_t>(inode->admitted_size, target + owned.size());
            inode->next_data_sequence = seq + 1;

            // POSIX write() makes accepted bytes immediately visible to this
            // node, but does not imply stable storage. Keep the operation in the
            // local overlay now; the durability worker advances
            // durable_data_sequence only after spool fsync -> journal append ->
            // journal fsync. Distributed publication is clamped to that durable
            // prefix, so relaxing write acknowledgement cannot expose an
            // unstable generation to other nodes or metadata quorum.
            inode->data_ops.push_back(ticket->op);
            inode->visible.size =
                std::max<uint64_t>(inode->visible.size, target + owned.size());
            inode->visible.mtime_ns = now;
            inode->visible.ctime_ns = now;
            ++inode->durability_pending;

            // Queue while still holding the inode lock so per-inode journal
            // sequence order exactly follows spool reservation order.
            state_->enqueue_durability(ticket);
        }

        // Normal POSIX semantics: successful write() means the bytes have been
        // accepted by this filesystem instance, not that they have reached
        // stable storage. release()/close waits for local spool+journal
        // durability; fsync additionally waits for distributed publication.
        return owned.size();
    });
}

void FuseFrontend::truncate(uint64_t inode_id, uint64_t size) {
    dispatch(FuseOperationClass::write,
             [this, inode_id, size](Clock::time_point deadline, std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        state_->wait_for_inode_durability(inode, deadline, cancelled);
        std::lock_guard lock(inode->mutex);
        check_deadline(deadline, cancelled);
        if (inode->visible.type != EntryType::file)
            throw FsError(EISDIR, "directory");
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "asynchronous backend error");
        const auto seq = inode->next_data_sequence;
        const auto now = wall_time_ns();
        State::DataOp op;
        op.kind = State::DataOp::Kind::truncate;
        op.sequence = seq;
        op.size = size;
        op.mtime_ns = now;
        op.ctime_ns = now;
        {
            std::lock_guard journal_admission(state_->journal_admission_mutex);
            state_->journal_inode_locked(inode);
            state_->journal_data_operation(inode->id, op);
        }
        inode->next_data_sequence = seq + 1;
        inode->data_ops.push_back(op);
        inode->durable_data_sequence = seq;
        inode->visible.size = size;
        inode->visible.mtime_ns = now;
        inode->visible.ctime_ns = now;
        inode->admitted_size = size;
    });
}

void FuseFrontend::truncate(std::string_view path, uint64_t size) {
    auto inode = inode_for_path(path);
    if (!inode)
        throw FsError(ENOENT, "missing");
    truncate(*inode, size);
}

void FuseFrontend::flush(uint64_t inode_id) {
    dispatch(FuseOperationClass::sync,
             [this, inode_id](Clock::time_point deadline, std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        auto inode = state_->resolve_inode(inode_id);
        // POSIX flush is not a stable-storage barrier. Opportunistically publish
        // whatever prefix has already completed local durability and return;
        // release() is the close-time local durability boundary.
        state_->request_data_publication(inode);
        check_deadline(deadline, cancelled);
    });
}

void FuseFrontend::fsync(uint64_t inode_id) {
    dispatch(FuseOperationClass::sync,
             [this, inode_id](Clock::time_point deadline, std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);

        // First make every accepted local write recoverable. The durability
        // worker enforces payload fsync -> journal append -> journal fsync, so no
        // extra per-fd fsync is required here once the watermark is reached.
        state_->wait_for_inode_durability(inode, deadline, cancelled);
        const auto target = state_->durable_sequence(inode);
        check_deadline(deadline, cancelled);

        // Macha's fsync is deliberately stronger than merely syncing the local
        // staging file: require the durable prefix to finish its normal
        // DistributedStore + metadata commit before returning. This does not
        // weaken or bypass quorum policy; it waits for the existing publication
        // machinery to satisfy it.
        state_->request_data_publication(inode);
        state_->wait_for_inode_publication(inode, target, deadline, cancelled);
    });
}

void FuseFrontend::release(uint64_t inode_id, bool writable) {
    dispatch(FuseOperationClass::lifecycle,
             [this, inode_id, writable](Clock::time_point deadline, std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        auto inode = state_->resolve_inode(inode_id);
        if (writable)
            state_->wait_for_inode_durability(inode, deadline, cancelled);
        // flush()/fsync() are already write-handle-only in the adapter. Keep
        // release symmetric: closing a read-only descriptor must not publish
        // dirty data belonging to another writer on the same inode.
        if (writable)
            state_->request_data_publication(inode);
        bool closed = false;
        {
            std::lock_guard lock(inode->mutex);
            if (inode->open_handles) {
                --inode->open_handles;
                closed = true;
            }
        }
        if (writable && closed) {
            state_->open_writers.fetch_sub(1, std::memory_order_relaxed);
            state_->data_cv.notify_all();
        }
    });
}

std::pair<uint64_t, uint64_t> FuseFrontend::logical_capacity() const {
    return state_->fs.logical_capacity();
}

void FuseFrontend::note_interactive_activity(uint64_t bytes) {
    state_->fs.note_interactive_activity(bytes);
}

std::string FuseFrontend::path_for_inode(uint64_t id) const {
    auto inode = state_->resolve_inode(id);
    std::lock_guard lock(inode->mutex);
    return inode->current_path;
}

std::optional<uint64_t> FuseFrontend::inode_for_path(std::string_view path) {
    state_->refresh_namespace_if_stale();
    std::lock_guard lock(state_->namespace_mutex);
    auto found = state_->paths.find(canonical_path(path));
    if (found == state_->paths.end()) return {};
    return found->second->id;
}

std::vector<FuseDirtyRange> FuseFrontend::dirty_ranges(uint64_t id) const {
    auto inode = state_->resolve_inode(id);
    std::lock_guard lock(inode->mutex);
    std::vector<FuseDirtyRange> ranges;
    uint64_t size = inode->base.size;
    for (const auto& op : inode->data_ops) {
        if (op.kind == State::DataOp::Kind::truncate) {
            if (op.size < size)
                clip_ranges(ranges, op.size);
            else if (op.size > size)
                merge_range(ranges, size, op.size - size);
            size = op.size;
        } else {
            merge_range(ranges, op.offset, op.length);
            size = std::max(size, op.offset + op.length);
        }
    }
    return ranges;
}

FuseFrontendStatus FuseFrontend::status() const {
    FuseFrontendStatus out;
    out.broker_pending = state_->broker_pending.load();
    {
        std::lock_guard lock(state_->namespace_queue_mutex);
        out.pending_namespace = state_->namespace_pending_locked();
    }
    {
        std::lock_guard lock(state_->data_queue_mutex);
        out.pending_data = state_->data_queue.size();
    }
    // Deferred-but-not-admitted work is pending; active work is reported
    // separately and deliberately not double-counted in pending_data.
    {
        std::lock_guard lock(state_->namespace_mutex);
        for (const auto& [_, inode] : state_->inodes) {
            std::lock_guard inode_lock(inode->mutex);
            if ((inode->data_deferred || inode->unconfirmed_data_entry) &&
                !inode->data_queued && !inode->data_running)
                ++out.pending_data;
        }
    }
    out.active_data = state_->active_data.load();
    out.timed_out_requests = state_->timed_out_requests.load();
    out.merged_publications = state_->merged_publications.load();
    out.backend_failures = state_->backend_failures.load();
    out.durability_batches = state_->durability_batches.load();
    out.durability_writes = state_->durability_writes.load();
    return out;
}

bool FuseFrontend::wait_for_idle(std::chrono::milliseconds timeout) {
    const auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        auto current = status();
        if (!current.broker_pending && !current.pending_namespace && !current.pending_data &&
            !current.active_data)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto current = status();
    return !current.broker_pending && !current.pending_namespace && !current.pending_data &&
           !current.active_data;
}

std::vector<HydrationHint> FuseFrontend::hints() {
    std::vector<HydrationHint> out;
    std::lock_guard lock(state_->hint_mutex);
    const auto now = Clock::now();
    for (auto it = state_->hint_states.begin(); it != state_->hint_states.end();) {
        if (now >= it->second.expires) {
            it = state_->hint_states.erase(it);
            continue;
        }
        const auto& hint = it->second;
        HydrationHint result;
        result.run_id = "fuse:" + std::to_string(it->first);
        result.priority = state_->config.hydration_priority;
        result.reason = "fuse-demand";
        result.frame_type = FrameType::read_ahead;
        const auto end = std::min(hint.entry.extents.size(),
                                  hint.last + 1 + state_->config.read_ahead_extents);
        std::set<ObjectId> seen;
        for (size_t i = hint.first; i < end; ++i) {
            const auto& extent = hint.entry.extents[i];
            if (extent.hole || seen.contains(extent.id))
                continue;
            seen.insert(extent.id);
            result.objects.push_back(extent.id);
        }
        if (!result.objects.empty())
            out.push_back(std::move(result));
        ++it;
    }
    return out;
}

void FuseFrontend::set_wake_callback(std::function<void()> callback) {
    std::lock_guard lock(state_->hint_mutex);
    state_->hint_wake_callback = std::move(callback);
}

void FuseFrontend::stop() {
    if (state_)
        state_->stop();
}

} // namespace macha
