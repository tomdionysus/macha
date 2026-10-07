// SPDX-License-Identifier: GPL-3.0-or-later
#include "observation.hpp"
#include "fuse/fuse_frontend.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "fuse/fuse_journal.hpp"
#include "retry_policy.hpp"
#include "startup_progress.hpp"
#include "log.hpp"
#include "supervised.hpp"
#include "filesystem/macos_unicode.hpp"

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
        auto n =
            ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
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
        auto n =
            ::pwrite(fd, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
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
    do {
        rc = ::fsync(fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0)
        throw FsError(errno, what);
}

void sync_directory(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        throw FsError(errno, "cannot open FUSE spool directory for sync");
    int rc;
    do {
        rc = ::fsync(fd);
    } while (rc != 0 && errno == EINTR);
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
    return FuseEntryAttributes{entry.type, entry.mode,     entry.uid,      entry.gid,
                               entry.size, entry.ctime_ns, entry.mtime_ns, entry.version};
}

// Holds two distinct I/O mutexes, taken together without lock-order deadlock.
class MACHA_SCOPED_CAPABILITY PairLock {
    std::scoped_lock<std::mutex, std::mutex> lock_;

  public:
    PairLock(IoMutex& first, IoMutex& second) MACHA_ACQUIRE(first, second) MACHA_EXCLUDES(no_io)
        : lock_(first.native(), second.native()) {}
    ~PairLock() MACHA_RELEASE() {}
    PairLock(const PairLock&) = delete;
    PairLock& operator=(const PairLock&) = delete;
};

} // namespace

ViewerWeightedAdmission::ViewerWeightedAdmission(FileSystem& filesystem, const FuseConfig& config)
    : fs_(filesystem), quiet_(config.publication_quiet),
      share_(config.viewer_weight, config.loader_weight) {}

bool ViewerWeightedAdmission::viewer_active() const {
    return quiet_.count() > 0 && fs_.idle_for(WorkClass::viewer) < quiet_;
}

bool ViewerWeightedAdmission::can_start(TimePoint now) {
    return share_.can_start(now, viewer_active());
}

void ViewerWeightedAdmission::started(TimePoint now, bool begin_service) {
    share_.started(now, viewer_active(), begin_service);
}

void ViewerWeightedAdmission::service_started(TimePoint now) {
    share_.service_started(now, viewer_active());
}

bool ViewerWeightedAdmission::should_yield(TimePoint now) {
    return share_.should_yield(now, viewer_active());
}

void ViewerWeightedAdmission::finished(TimePoint now) {
    (void)share_.finished(now, viewer_active());
}

std::optional<std::chrono::milliseconds> ViewerWeightedAdmission::retry_after(TimePoint now) {
    // The earlier of the viewer window closing and the loader's cooldown.
    const auto idle = fs_.idle_for(WorkClass::viewer);
    if (quiet_.count() <= 0 || idle >= quiet_)
        return std::chrono::milliseconds(0);
    auto retry = quiet_ - idle;
    if (const auto cooldown = share_.wait_for(now, true); cooldown > std::chrono::milliseconds(0))
        retry = std::min(retry, cooldown);
    return retry;
}

class FuseReadSession {
  public:
    // Held across opening the read handle, which may fetch from peers.
    IoMutex mutex;
    // Set before the session is shared.
    uint64_t inode{};
    uint64_t base_version MACHA_GUARDED_BY(mutex){};
    std::shared_ptr<ReadHandle> reader MACHA_GUARDED_BY(mutex);
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

    int get() const noexcept {
        return fd_;
    }

    int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
};

struct FuseFrontend::State {
    static constexpr size_t spool_checksum_chunk_size = 256 * 1024;

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
        // One SHA-256 per spool_checksum_chunk_size chunk; empty means an
        // unchecksummed record, which still replays.
        std::vector<Hash256> spool_hashes;
        // Heap reserved before the op is accepted. DataSnapshot copies own no
        // second reservation: the estimate covers both copies and slack.
        uint64_t metadata_charge{};
        // Shared by history, durability ticket and publication snapshot;
        // released when the last of them goes.
        std::shared_ptr<RetainedMemoryLedger::Lease> process_memory;
    };

    struct DataSnapshot {
        uint64_t target_sequence{};
        uint64_t required_namespace_sequence{};
        std::vector<DataOp> operations;
        std::optional<std::string> published_path;
        std::filesystem::path spool_path;
    };

    // Read index over data_ops: non-overlapping ranges, each holding the newest
    // source for its interval, so reads need not replay the history. The
    // journal stays the recovery/publication authority.
    struct DataOverlayRange {
        uint64_t end{};
        uint64_t spool_offset{};
        bool zero{};
    };

    // In-memory cursor holding writer state between scheduling quanta; input
    // stays in the spool, so a crash just replays the same journal generation.
    // Used only by the data worker running its inode (data_running), or read
    // under data_queue_mutex while the inode is queued and no worker runs it.
    struct DataPublication {
        DataSnapshot snapshot;
        bool recovered{};
        bool initialized{};
        size_t operation_index{};
        uint64_t operation_offset{};
        uint64_t publication_bytes{};
        uint64_t spool_bytes_read{};
        // Spool bytes replayed since the last drained quantum; they earn
        // bootstrap admission credit only once drain_staging() succeeds.
        uint64_t unreported_spool_progress{};
        std::shared_ptr<PublicationWriter> writer;
        ScopedFd replay_spool;
    };

    struct Inode {
        // Held across spool file I/O, journal appends and namespace node reads.
        mutable IoMutex mutex;
        // Set before the inode is shared.
        uint64_t id{};
        FsEntry base MACHA_GUARDED_BY(mutex);
        FsEntry visible MACHA_GUARDED_BY(mutex);
        std::string current_path MACHA_GUARDED_BY(mutex);
        std::optional<std::string> published_path MACHA_GUARDED_BY(mutex);
        uint64_t namespace_sequence MACHA_GUARDED_BY(mutex){};
        uint64_t next_data_sequence MACHA_GUARDED_BY(mutex){1};
        uint64_t requested_data_sequence MACHA_GUARDED_BY(mutex){};
        // Highest data sequence past the local durability barrier. data_ops may
        // hold newer buffered writes: visible locally, not yet publishable.
        uint64_t durable_data_sequence MACHA_GUARDED_BY(mutex){};
        uint64_t published_data_sequence MACHA_GUARDED_BY(mutex){};
        // Highest data sequence recovered from the journal at startup;
        // publications covering it use the recovery concurrency budget.
        uint64_t recovery_data_sequence MACHA_GUARDED_BY(mutex){};
        uint64_t requested_namespace_sequence MACHA_GUARDED_BY(mutex){};
        std::vector<DataOp> data_ops MACHA_GUARDED_BY(mutex);
        std::map<uint64_t, DataOverlayRange> data_overlay MACHA_GUARDED_BY(mutex);
        int spool_fd MACHA_GUARDED_BY(mutex){-1};
        std::filesystem::path spool_path MACHA_GUARDED_BY(mutex);
        uint64_t spool_end MACHA_GUARDED_BY(mutex){};
        // A missing or truncated spool found at recovery; invalidates only this
        // inode's dirty generation, not the mount.
        std::optional<std::string> recovery_spool_error MACHA_GUARDED_BY(mutex);
        // Admitted writes share one payload+journal barrier; admitted_size
        // reserves O_APPEND offsets until they become visible.
        uint64_t admitted_size MACHA_GUARDED_BY(mutex){};
        size_t durability_pending MACHA_GUARDED_BY(mutex){};
        std::condition_variable_any durability_cv;
        size_t open_handles MACHA_GUARDED_BY(mutex){};
        size_t writable_handles MACHA_GUARDED_BY(mutex){};
        // Owning count of namespace ops referring to this inode that have not
        // reached `done`; moving an op between queues leaves it unchanged.
        std::atomic_size_t namespace_references{};
        bool data_queued MACHA_GUARDED_BY(mutex){};
        // Single enqueue owner across the inode-lock -> queue-lock handoff in
        // request_data_publication().
        bool data_enqueue_pending MACHA_GUARDED_BY(mutex){};
        bool data_running MACHA_GUARDED_BY(mutex){};
        bool data_deferred MACHA_GUARDED_BY(mutex){};
        std::shared_ptr<DataPublication> data_publication MACHA_GUARDED_BY(mutex);
        uint64_t accounted_data_operations MACHA_GUARDED_BY(mutex){};
        uint64_t accounted_data_operation_bytes MACHA_GUARDED_BY(mutex){};
        uint64_t accounted_overlay_ranges MACHA_GUARDED_BY(mutex){};
        uint64_t accounted_overlay_bytes MACHA_GUARDED_BY(mutex){};
        uint64_t accounted_publication_operations MACHA_GUARDED_BY(mutex){};
        uint64_t accounted_publication_operation_bytes MACHA_GUARDED_BY(mutex){};
        uint64_t accounted_operation_metadata_bytes MACHA_GUARDED_BY(mutex){};
        std::optional<int> backend_error MACHA_GUARDED_BY(mutex);
        // Discipline 2: publication retry state, and why it is parked once the
        // budget is spent. Parked blocks only publication; reads and writes
        // continue. Retry resets the budget, abandon retires the spool.
        RetryState publication_retry MACHA_GUARDED_BY(mutex);
        struct Parked {
            int error_code{};
            std::string error_message;
            Clock::time_point since{};
            // The target's reachability when parked: publication is tried
            // again once it differs.
            uint64_t reachability_epoch{};
        };
        std::optional<Parked> parked MACHA_GUARDED_BY(mutex);
        uint64_t journal_epoch MACHA_GUARDED_BY(mutex){};
        uint64_t unconfirmed_data_sequence MACHA_GUARDED_BY(mutex){};
        uint64_t unconfirmed_publication_bytes MACHA_GUARDED_BY(mutex){};
        std::optional<FsEntry> unconfirmed_data_entry MACHA_GUARDED_BY(mutex);

        ~Inode() {
            if (spool_fd >= 0)
                ::close(spool_fd);
        }
    };

    static bool overlay_ranges_mergeable(
        const std::pair<const uint64_t, DataOverlayRange>& left,
        const std::pair<const uint64_t, DataOverlayRange>& right) {
        return left.second.end == right.first && left.second.zero == right.second.zero &&
               (left.second.zero ||
                left.second.spool_offset + (left.second.end - left.first) ==
                    right.second.spool_offset);
    }

    static void assign_overlay_range_locked(Inode& inode, uint64_t begin, uint64_t end,
                                            bool zero, uint64_t spool_offset = 0)
        MACHA_REQUIRES(inode.mutex) {
        if (begin >= end)
            return;

        auto it = inode.data_overlay.lower_bound(begin);
        if (it != inode.data_overlay.begin()) {
            auto previous = std::prev(it);
            if (previous->second.end > begin)
                it = previous;
        }

        std::optional<std::pair<uint64_t, DataOverlayRange>> left;
        std::optional<std::pair<uint64_t, DataOverlayRange>> right;
        while (it != inode.data_overlay.end() && it->first < end) {
            const auto existing_begin = it->first;
            const auto existing = it->second;
            if (existing.end <= begin) {
                ++it;
                continue;
            }
            if (existing_begin < begin) {
                left = std::pair{existing_begin,
                                 DataOverlayRange{begin, existing.spool_offset,
                                                  existing.zero}};
            }
            if (existing.end > end) {
                right = std::pair{
                    end, DataOverlayRange{existing.end,
                                          existing.zero
                                              ? 0
                                              : existing.spool_offset + (end - existing_begin),
                                          existing.zero}};
            }
            it = inode.data_overlay.erase(it);
        }
        if (left)
            inode.data_overlay.insert_or_assign(left->first, left->second);
        if (right)
            inode.data_overlay.insert_or_assign(right->first, right->second);
        auto inserted = inode.data_overlay.insert_or_assign(
            begin, DataOverlayRange{end, spool_offset, zero}).first;

        if (inserted != inode.data_overlay.begin()) {
            auto previous = std::prev(inserted);
            if (overlay_ranges_mergeable(*previous, *inserted)) {
                previous->second.end = inserted->second.end;
                inode.data_overlay.erase(inserted);
                inserted = previous;
            }
        }
        auto next = std::next(inserted);
        if (next != inode.data_overlay.end() && overlay_ranges_mergeable(*inserted, *next)) {
            inserted->second.end = next->second.end;
            inode.data_overlay.erase(next);
        }
    }

    static void apply_data_overlay_locked(Inode& inode, const DataOp& op)
        MACHA_REQUIRES(inode.mutex) {
        const auto old_size = inode.visible.size;
        if (op.kind == DataOp::Kind::truncate) {
            assign_overlay_range_locked(inode, std::min(old_size, op.size),
                                        std::max(old_size, op.size), true);
            return;
        }
        if (op.offset > old_size)
            assign_overlay_range_locked(inode, old_size, op.offset, true);
        assign_overlay_range_locked(inode, op.offset, op.offset + op.length, false,
                                    op.spool_offset);
    }

    static void rebuild_data_overlay_locked(Inode& inode) MACHA_REQUIRES(inode.mutex) {
        inode.data_overlay.clear();
        inode.visible.size = inode.base.size;
        for (const auto& op : inode.data_ops) {
            apply_data_overlay_locked(inode, op);
            inode.visible.size = op.kind == DataOp::Kind::truncate
                                     ? op.size
                                     : std::max(inode.visible.size, op.offset + op.length);
        }
    }

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
        // Inodes displaced by this op (e.g. rename-over): still valid for open
        // handles, but own no published path.
        std::vector<uint64_t> removed;
        // Metadata generation seen once the backend durably applied this op
        // (in-memory only). A view at or past it holds the effect or whatever
        // superseded it, so confirmation tests the generation, not the effect.
        uint64_t published_generation{};
    };

    enum class JournalRecord : uint8_t {
        inode = 1,
        namespace_op = 2,
        data_op = 3,
        namespace_published = 4,
        namespace_done = 5,
        data_published = 6,
        data_done = 7,
        data_abandoned = 8,
        // Namespace batch about to publish as one atomic mutation, keyed in
        // the mutation-sequence clock by this node's FUSE origin and its
        // first op sequence.
        namespace_batch = 9,
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
        // First op sequence -> op count.
        std::map<uint64_t, uint32_t> namespace_batches;
        std::map<uint64_t, std::vector<DataOp>> data_ops;
        std::map<uint64_t, std::pair<uint64_t, FsEntry>> data_published;
        std::map<uint64_t, uint64_t> data_done;
        std::set<uint64_t> data_history_inodes;
        uint64_t max_inode{};
        uint64_t max_namespace_sequence{};
        size_t pending_operations{};
        // Discipline 3: frames that did not fit the state (skipped, never
        // fatal), and legitimate `done` records lacking a `published` record.
        size_t skipped_frames{};
        size_t done_without_published{};
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
        Mutex mutex;
        std::condition_variable_any cv;
        std::deque<BrokerTask> tasks MACHA_GUARDED_BY(mutex);
        // Started by start() and joined by stop(), on the lifecycle thread.
        std::vector<std::jthread> workers;
    };

    struct HintState {
        FsEntry entry;
        size_t first{};
        size_t last{};
        Clock::time_point expires{};
    };

    FileSystem& fs;
    RetainedMemoryLedger& retained_memory;
    // Completed by the constructor, then fixed.
    FuseConfig config;
    // Cancels the initial-namespace wait in start(); see the constructor.
    const std::stop_token startup_stop;
    // Decides when loader publication runs; owned.
    const std::unique_ptr<LoaderAdmission> admission;
    // Receives published file data; outlives the frontend.
    PublicationTarget& publication_target;
    const std::filesystem::path spool_dir;
    const std::filesystem::path journal_path;
    const std::filesystem::path journal_dir;
    // Serialises the descriptor+operation append against compaction, so a reset
    // cannot fall between seeing a descriptor present and appending the op.
    // Guards no state. Held across journal appends and compaction.
    IoMutex journal_admission_mutex;
    // Held across journal writes, fsyncs and compaction.
    IoMutex journal_mutex;
    int journal_fd MACHA_GUARDED_BY(journal_mutex){-1};
    bool journal_poisoned MACHA_GUARDED_BY(journal_mutex){};
    std::atomic_uint64_t journal_epoch{1};
    std::atomic_size_t durable_pending_operations{};
    // Keeps compaction from dropping a descriptor admitted ahead of its
    // group-committed data-op frame.
    std::atomic_size_t journal_inflight_admissions{};

    Mutex durability_mutex;
    std::condition_variable_any durability_cv;
    std::deque<std::shared_ptr<DurabilityTicket>> durability_queue
        MACHA_GUARDED_BY(durability_mutex);
    // Started by start() and joined by stop(), on the lifecycle thread.
    std::jthread durability_worker;
    bool durability_poisoned MACHA_GUARDED_BY(durability_mutex){};
    std::exception_ptr durability_error MACHA_GUARDED_BY(durability_mutex);
    std::atomic_uint64_t durability_batches{};
    std::atomic_uint64_t durability_writes{};
    std::atomic_uint64_t retained_durability_tickets{};
    // Bytes held in inode-*.spool files: reserved before pwrite, released only
    // on retirement, so concurrent inodes cannot exceed the cap.
    std::atomic_uint64_t spool_bytes{};
    // Spool admission backpressure: below half capacity writes run at disk
    // speed; above it they are paced down to the measured publication rate by
    // 90%; at the hard bound writers sleep until retirement frees room
    // (never ENOSPC).
    // Held across the spool directory's free-space check and logging.
    IoMutex spool_admission_mutex;
    std::condition_variable_any spool_admission_cv;
    Clock::time_point next_spool_admission MACHA_GUARDED_BY(spool_admission_mutex){};
    double spool_publish_rate_bytes_per_second MACHA_GUARDED_BY(spool_admission_mutex){};
    SpoolRetirementRateEstimator spool_retirement_rate MACHA_GUARDED_BY(spool_admission_mutex);
    // Until the first retirement gives a rate sample, each drained publication
    // quantum grants equal write credit, capped at the soft-to-hard headroom;
    // it never bypasses max_spool_bytes.
    uint64_t spool_progress_credit_bytes MACHA_GUARDED_BY(spool_admission_mutex){};
    uint64_t spool_admission_revision MACHA_GUARDED_BY(spool_admission_mutex){};
    std::atomic_bool spool_drain_requested{};
    std::atomic_uint64_t spool_publish_rate_diagnostic{};
    std::atomic_uint64_t spool_publish_rate_window_bytes{};
    std::atomic_uint64_t spool_publish_rate_window_ms{};
    std::atomic_uint64_t spool_throttle_waits{};
    std::atomic_uint64_t spool_throttle_wait_ns{};

    // Held across journal appends and namespace node reads.
    mutable IoMutex namespace_mutex;
    std::map<std::string, std::shared_ptr<Inode>, std::less<>> paths
        MACHA_GUARDED_BY(namespace_mutex);
    std::map<uint64_t, std::shared_ptr<Inode>> inodes MACHA_GUARDED_BY(namespace_mutex);
    uint64_t next_inode MACHA_GUARDED_BY(namespace_mutex){2};
    uint64_t next_namespace_sequence MACHA_GUARDED_BY(namespace_mutex){1};
    std::atomic_uint64_t inode_count{};
    std::atomic_uint64_t peak_inode_count{};
    std::atomic_uint64_t reclaimed_inode_count{};
    // Serialises namespace mutations; guards no state. Held across the
    // mutation's journal appends.
    IoMutex namespace_apply_mutex;

    // Held across namespace node reads while confirming published operations.
    IoMutex namespace_queue_mutex;
    std::condition_variable_any namespace_cv;
    std::deque<NamespaceOp> namespace_queue MACHA_GUARDED_BY(namespace_queue_mutex);
    std::deque<NamespaceOp> namespace_unconfirmed MACHA_GUARDED_BY(namespace_queue_mutex);
    bool namespace_inflight MACHA_GUARDED_BY(namespace_queue_mutex){};
    uint64_t namespace_inflight_sequence MACHA_GUARDED_BY(namespace_queue_mutex){};
    size_t namespace_inflight_operations MACHA_GUARDED_BY(namespace_queue_mutex){};
    // The namespace op the worker is stuck on with a non-retryable error; an
    // operator may name its sequence to abandon it (never automatic, see
    // namespace_loop()).
    std::optional<NamespaceOp> namespace_blocked_op MACHA_GUARDED_BY(namespace_queue_mutex);
    int namespace_blocked_error_code MACHA_GUARDED_BY(namespace_queue_mutex){};
    std::string namespace_blocked_error_message MACHA_GUARDED_BY(namespace_queue_mutex);
    Clock::time_point namespace_blocked_since MACHA_GUARDED_BY(namespace_queue_mutex);
    uint64_t namespace_skip_requested_sequence MACHA_GUARDED_BY(namespace_queue_mutex){};
    // Started by start() and joined by stop(), on the lifecycle thread.
    std::jthread namespace_worker;
    struct DataQueueItem {
        std::shared_ptr<Inode> inode;
        // Journal-restored; affects replay validation and caching, not priority.
        bool recovered{};
    };

    // Held while taking inode mutexes, which are held across I/O.
    IoMutex data_queue_mutex;
    std::condition_variable_any data_cv;
    std::deque<DataQueueItem> data_queue MACHA_GUARDED_BY(data_queue_mutex);
    // Started by start() and joined by stop(), on the lifecycle thread.
    std::vector<std::jthread> data_workers;
    std::atomic_size_t active_data{};
    std::atomic_size_t active_recovery_data{};
    // Each active worker reserves one quantum.
    uint64_t publication_inflight_bytes MACHA_GUARDED_BY(data_queue_mutex){};
    std::atomic_uint64_t publication_inflight_bytes_diagnostic{};
    // Open writable handles. Not a viewer signal: loaders keep writers open
    // and must not collapse publication to one worker.
    std::atomic_size_t open_writers{};
    // Inodes holding a provisional publication writer (and its extent leases).
    // Changed only by set_data_publication_locked(); bounded by
    // config.publication_max_open_writers so the ledger cannot self-deadlock.
    std::atomic_size_t open_publications{};
    std::atomic_size_t peak_open_publications{};
    // Writer slots reserved at selection but not yet opened. Counted against
    // the cap with open_publications so selection and creation under
    // different locks cannot overshoot it.
    size_t reserved_publications MACHA_GUARDED_BY(data_queue_mutex){};

    std::array<BrokerQueue, 6> broker;
    std::atomic_size_t broker_pending{};
    std::atomic_bool stopping{};
    // Set by interrupt_waits(): no wait on a publication may outlast it.
    std::atomic_bool waits_interrupted{};
    Mutex write_request_mutex;
    std::condition_variable_any write_request_cv;
    uint64_t pending_write_request_bytes MACHA_GUARDED_BY(write_request_mutex){};
    std::atomic_uint64_t pending_write_request_bytes_diagnostic{};
    std::atomic_uint64_t peak_pending_write_request_bytes{};

    struct WriteRequestLease {
        std::function<void()> release;
        explicit WriteRequestLease(std::function<void()> callback)
            : release(std::move(callback)) {}
        WriteRequestLease(const WriteRequestLease&) = delete;
        WriteRequestLease& operator=(const WriteRequestLease&) = delete;
        ~WriteRequestLease() { if (release) release(); }
    };

    mutable Mutex hint_mutex;
    std::map<uint64_t, HintState> hint_states MACHA_GUARDED_BY(hint_mutex);
    std::function<void()> hint_wake_callback MACHA_GUARDED_BY(hint_mutex);

    // Serialises namespace refreshes; guards no state. Held across journal
    // appends and namespace node reads.
    IoMutex refresh_mutex;
    std::atomic_uint64_t refreshed_namespace_revision{};
    // The tree `paths` was last brought into line with. A refresh applies
    // what differs between it and the new tree, unless the view may have
    // parted from it: an operation applied here and then retired without
    // being published. Then, and the first time, the whole tree is walked.
    std::optional<ObjectId> adopted_namespace_root MACHA_GUARDED_BY(namespace_mutex);
    std::atomic_bool namespace_view_diverged{true};
    // Last revision whose deferred adoption was logged; logs once per view.
    std::atomic_uint64_t namespace_refresh_deferred_logged_revision{};

    std::atomic_uint64_t timed_out_requests{};
    std::atomic_uint64_t merged_publications{};
    std::atomic_uint64_t data_publication_requests{};
    std::atomic_uint64_t data_publication_notifications_suppressed{};
    std::atomic_uint64_t spool_pressure_publication_sweeps{};
    std::atomic_uint64_t data_publication_coalesced_queued{};
    std::atomic_uint64_t data_publication_coalesced_running{};
    std::atomic_uint64_t data_publication_coalesced_unconfirmed{};
    std::atomic_uint64_t data_publications_started{};
    std::atomic_uint64_t data_publications_completed{};
    std::atomic_uint64_t data_publication_peak_active{};
    std::atomic_uint64_t data_publication_quanta{};
    std::atomic_uint64_t data_publication_yields{};
    std::atomic_uint64_t data_publication_peak_inflight_bytes{};
    std::atomic_uint64_t data_publication_peak_pipeline_extents{};
    std::atomic_uint64_t data_closed_priority_selections{};
    std::atomic_uint64_t data_retirement_priority_selections{};
    // Selections that preferred an inode already holding a writer because the
    // open-writer cap was reached. Rising with no completions means the open
    // set is stuck.
    std::atomic_uint64_t data_publication_selections_under_writer_cap{};
    std::atomic_uint64_t data_publication_bytes_read{};
    std::atomic_uint64_t data_publication_bytes_committed{};
    std::atomic_uint64_t data_publication_bytes_confirmed{};
    std::atomic_uint64_t data_publication_completed_spool_bytes_read{};
    std::atomic_uint64_t data_publication_completed_source_bytes_read{};
    std::atomic_uint64_t data_publication_completed_reused_extents{};
    std::atomic_uint64_t data_publication_completed_put_extents{};
    std::atomic_uint64_t data_overlay_read_queries{};
    std::atomic_uint64_t data_overlay_ranges_examined{};
    std::atomic_uint64_t data_overlay_descriptors_copied{};
    std::atomic_uint64_t retained_data_operations{};
    std::atomic_uint64_t retained_data_operation_bytes{};
    std::atomic_uint64_t retained_overlay_ranges{};
    std::atomic_uint64_t retained_overlay_bytes{};
    std::atomic_uint64_t retained_publication_operations{};
    std::atomic_uint64_t retained_publication_operation_bytes{};
    Mutex operation_metadata_mutex;
    std::condition_variable_any operation_metadata_cv;
    uint64_t operation_metadata_bytes MACHA_GUARDED_BY(operation_metadata_mutex){};
    std::atomic_uint64_t operation_metadata_bytes_diagnostic{};
    std::atomic_uint64_t peak_operation_metadata_bytes{};
    std::atomic_uint64_t operation_metadata_waits{};
    std::atomic_uint64_t backend_failures{};
    // Monotonic counts of durable work (not queue depth), so tests can assert
    // batching and replay amplification without timing.
    std::atomic_uint64_t namespace_operations_admitted{};
    std::atomic_uint64_t namespace_operations_recovered{};
    std::atomic_uint64_t namespace_publication_attempts{};
    std::atomic_uint64_t namespace_publication_batches{};
    std::atomic_uint64_t namespace_operations_batched{};
    std::atomic_uint64_t namespace_operations_published{};
    std::atomic_uint64_t namespace_operations_confirmed{};
    std::atomic_uint64_t parked_publications{};
    std::atomic_uint64_t publication_retries_backed_off{};
    // Inodes whose current failure run crossed the escalation threshold; a
    // persistently retrying file is operator-visible, not a DEBUG line.
    std::atomic_uint64_t publications_retrying_persistently{};
    // Admission wait slices (backpressure, not failure).
    std::atomic_uint64_t write_admission_waits{};
    std::atomic_uint64_t process_memory_admission_waits{};
    // Discipline 3: recovery resolves instead of refusing; these say how often.
    std::atomic_uint64_t journal_recovery_skipped_frames{};
    std::atomic_uint64_t journal_recovery_quarantined_bytes{};
    std::atomic_uint64_t recovery_dropped_operations{};
    std::atomic_uint64_t publications_abandoned{};
    // Earliest backed-off inode due time (steady-clock ns; 0 = none); the idle
    // data loop sleeps to it.
    std::atomic<int64_t> deferred_retry_due_ns{0};
    std::atomic_uint64_t journal_append_batches{};
    std::atomic_uint64_t journal_records_appended{};
    std::atomic_uint64_t journal_durability_barriers{};

    State(FileSystem& filesystem, RetainedMemoryLedger& memory, FuseConfig policy,
          std::unique_ptr<LoaderAdmission> loader_admission, PublicationTarget& target,
          std::stop_token startup_cancel = {})
        : fs(filesystem), retained_memory(memory), config(std::move(policy)), startup_stop(std::move(startup_cancel)),
          admission(std::move(loader_admission)), publication_target(target),
          spool_dir(config.spool_path.value_or(fs.config().state_path / "fuse-spool")),
          journal_path(config.operation_journal_path.value_or(spool_dir / "operations.log")),
          journal_dir(journal_path.parent_path().empty() ? std::filesystem::path(".")
                                                         : journal_path.parent_path()) {
        // An unvalidated FuseConfig gets the same defaults Config validation
        // applies; zero would mean serial publication.
        if (!config.publication_pipeline_bytes)
            config.publication_pipeline_bytes =
                std::min<uint64_t>(config.publication_quantum_bytes,
                                   static_cast<uint64_t>(fs.extent_size()) * 2);
        // Unbounded open writers could fill the durable-lower budget with
        // partial extents and deadlock the ledger.
        if (!config.publication_max_open_writers) {
            const auto per_writer =
                static_cast<uint64_t>(fs.extent_size()) + config.publication_pipeline_bytes;
            config.publication_max_open_writers = static_cast<size_t>(std::max<uint64_t>(
                config.commit_workers,
                fs.config().runtime.loader_memory_reserve_bytes / per_writer));
        }
        fuse_namespace_origin = derive_fuse_namespace_origin(fs.node_id());
        if (!admission)
            throw std::invalid_argument("FUSE frontend requires a loader admission");
        // A data or namespace loop may be sleeping with no timed wake.
        admission->set_wake_callback([this] {
            {
                Lock lock(data_queue_mutex);
            }
            data_cv.notify_all();
            {
                Lock lock(namespace_queue_mutex);
            }
            namespace_cv.notify_all();
        });
    }

    ~State() {
        if (journal_fd >= 0)
            ::close(journal_fd);
    }

    static uint64_t operation_bytes(const std::vector<DataOp>& operations) {
        uint64_t bytes = static_cast<uint64_t>(operations.capacity()) * sizeof(DataOp);
        for (const auto& op : operations)
            bytes += static_cast<uint64_t>(op.spool_hashes.capacity()) * sizeof(Hash256);
        return bytes;
    }

    static uint64_t operation_metadata_charge(size_t checksum_count) {
        // History plus one publication snapshot, each with up to 2x vector
        // capacity slack.
        constexpr uint64_t copies_with_capacity_slack = 4;
        const auto hashes = static_cast<uint64_t>(checksum_count) * sizeof(Hash256);
        if (hashes > std::numeric_limits<uint64_t>::max() / copies_with_capacity_slack -
                         sizeof(DataOp))
            throw FsError(E2BIG, "FUSE operation metadata charge overflow");
        return copies_with_capacity_slack * (sizeof(DataOp) + hashes);
    }

    void release_operation_metadata(uint64_t bytes) {
        if (!bytes)
            return;
        {
            Lock lock(operation_metadata_mutex);
            operation_metadata_bytes = bytes > operation_metadata_bytes
                                           ? 0
                                           : operation_metadata_bytes - bytes;
            operation_metadata_bytes_diagnostic.store(operation_metadata_bytes,
                                                       std::memory_order_relaxed);
        }
        operation_metadata_cv.notify_all();
    }

    // Admission waits are backpressure, not errors: a writer waits in slices
    // (so stop is noticed) until admitted, never returning EAGAIN, which a
    // blocking write(2) would surface to the application. The request
    // deadline starts after admission.
    static constexpr auto admission_slice = std::chrono::milliseconds(200);
    static constexpr auto admission_notice = std::chrono::seconds(5);

    void note_admission_wait(const char* what, Clock::time_point since, uint64_t bytes) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since);
        if (waited >= admission_notice && waited.count() % 5000 < admission_slice.count())
            Log::debug(std::string("FUSE ") + what + " admission waiting bytes=" +
                       std::to_string(bytes) + " waited_ms=" + std::to_string(waited.count()) +
                       "; backpressure from publication, not an error");
    }

    std::unique_ptr<WriteRequestLease> reserve_operation_metadata(
        uint64_t bytes, Clock::time_point, const std::function<void()>& request_progress) {
        if (bytes > config.max_operation_metadata_bytes)
            throw FsError(E2BIG, "single FUSE operation exceeds metadata byte limit");
        Lock lock(operation_metadata_mutex);
        const auto since = Clock::now();
        while (operation_metadata_bytes > config.max_operation_metadata_bytes - bytes) {
            operation_metadata_waits.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            request_progress();
            lock.lock();
            if (stopping.load(std::memory_order_relaxed))
                throw FsError(EINTR, "FUSE operation metadata admission stopping");
            (void)operation_metadata_cv.wait_for(lock.native(), admission_slice);
            note_admission_wait("operation metadata", since, bytes);
        }
        operation_metadata_bytes += bytes;
        operation_metadata_bytes_diagnostic.store(operation_metadata_bytes,
                                                   std::memory_order_relaxed);
        auto peak = peak_operation_metadata_bytes.load(std::memory_order_relaxed);
        while (peak < operation_metadata_bytes &&
               !peak_operation_metadata_bytes.compare_exchange_weak(
                   peak, operation_metadata_bytes, std::memory_order_relaxed)) {
        }
        return std::make_unique<WriteRequestLease>([this, bytes] {
            release_operation_metadata(bytes);
        });
    }

    std::shared_ptr<RetainedMemoryLedger::Lease> reserve_process_memory(
        WorkClass memory_class, MemoryOwner owner, uint64_t bytes, Clock::time_point) {
        const auto since = Clock::now();
        for (;;) {
            auto lease = retained_memory.acquire(memory_class, owner, bytes,
                                                             Clock::now() + admission_slice);
            if (lease)
                return std::make_shared<RetainedMemoryLedger::Lease>(std::move(*lease));
            if (stopping.load(std::memory_order_relaxed))
                throw FsError(EINTR, "process retained-memory admission stopping");
            process_memory_admission_waits.fetch_add(1, std::memory_order_relaxed);
            note_admission_wait("process memory", since, bytes);
        }
    }

    static void replace_accounted(std::atomic_uint64_t& total, uint64_t& accounted,
                                  uint64_t current) {
        if (current >= accounted)
            total.fetch_add(current - accounted, std::memory_order_relaxed);
        else
            total.fetch_sub(accounted - current, std::memory_order_relaxed);
        accounted = current;
    }

    // Sole mutator of Inode::data_publication, keeping open_publications exact.
    void set_data_publication_locked(Inode& inode, std::shared_ptr<DataPublication> publication)
        MACHA_REQUIRES(inode.mutex) {
        const bool was_open = inode.data_publication != nullptr;
        const bool now_open = publication != nullptr;
        inode.data_publication = std::move(publication);
        if (was_open == now_open)
            return;
        if (!now_open) {
            open_publications.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        const auto open = open_publications.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = peak_open_publications.load(std::memory_order_relaxed);
        while (peak < open &&
               !peak_open_publications.compare_exchange_weak(peak, open,
                                                             std::memory_order_relaxed)) {
        }
    }

    bool writer_cap_reached() const MACHA_REQUIRES(data_queue_mutex) {
        return config.publication_max_open_writers &&
               open_publications.load(std::memory_order_relaxed) + reserved_publications >=
                   config.publication_max_open_writers;
    }

    void refresh_retained_owners_locked(Inode& inode) MACHA_REQUIRES(inode.mutex) {
        const auto operation_count = static_cast<uint64_t>(inode.data_ops.size());
        const auto operation_memory = operation_bytes(inode.data_ops);
        const auto overlay_count = static_cast<uint64_t>(inode.data_overlay.size());
        const auto overlay_memory = overlay_count *
            (sizeof(std::pair<const uint64_t, DataOverlayRange>) + 3 * sizeof(void*));
        uint64_t publication_count = 0;
        uint64_t publication_memory = 0;
        if (inode.data_publication) {
            publication_count = inode.data_publication->snapshot.operations.size();
            publication_memory = operation_bytes(inode.data_publication->snapshot.operations);
        }
        replace_accounted(retained_data_operations, inode.accounted_data_operations,
                          operation_count);
        replace_accounted(retained_data_operation_bytes, inode.accounted_data_operation_bytes,
                          operation_memory);
        replace_accounted(retained_overlay_ranges, inode.accounted_overlay_ranges, overlay_count);
        replace_accounted(retained_overlay_bytes, inode.accounted_overlay_bytes, overlay_memory);
        replace_accounted(retained_publication_operations,
                          inode.accounted_publication_operations, publication_count);
        replace_accounted(retained_publication_operation_bytes,
                          inode.accounted_publication_operation_bytes, publication_memory);
        uint64_t metadata_memory = 0;
        for (const auto& op : inode.data_ops)
            metadata_memory += op.metadata_charge;
        if (metadata_memory < inode.accounted_operation_metadata_bytes)
            release_operation_metadata(inode.accounted_operation_metadata_bytes -
                                       metadata_memory);
        else if (metadata_memory > inode.accounted_operation_metadata_bytes) {
            // Recovery restores acknowledged ownership unconditionally; above
            // a lowered limit it blocks new work until publication drains it.
            const auto added = metadata_memory - inode.accounted_operation_metadata_bytes;
            Lock metadata_lock(operation_metadata_mutex);
            operation_metadata_bytes += added;
            operation_metadata_bytes_diagnostic.store(operation_metadata_bytes,
                                                       std::memory_order_relaxed);
            auto peak = peak_operation_metadata_bytes.load(std::memory_order_relaxed);
            while (peak < operation_metadata_bytes &&
                   !peak_operation_metadata_bytes.compare_exchange_weak(
                       peak, operation_metadata_bytes, std::memory_order_relaxed)) {
            }
        }
        inode.accounted_operation_metadata_bytes = metadata_memory;
    }

    void release_retained_owners_locked(Inode& inode) MACHA_REQUIRES(inode.mutex) {
        replace_accounted(retained_data_operations, inode.accounted_data_operations, 0);
        replace_accounted(retained_data_operation_bytes, inode.accounted_data_operation_bytes, 0);
        replace_accounted(retained_overlay_ranges, inode.accounted_overlay_ranges, 0);
        replace_accounted(retained_overlay_bytes, inode.accounted_overlay_bytes, 0);
        replace_accounted(retained_publication_operations,
                          inode.accounted_publication_operations, 0);
        replace_accounted(retained_publication_operation_bytes,
                          inode.accounted_publication_operation_bytes, 0);
        release_operation_metadata(inode.accounted_operation_metadata_bytes);
        inode.accounted_operation_metadata_bytes = 0;
    }

    std::shared_ptr<WriteRequestLease> reserve_write_request_bytes(uint64_t bytes,
                                                                   Clock::time_point) {
        if (bytes > config.max_pending_write_bytes)
            throw FsError(E2BIG, "single FUSE write exceeds pending byte limit");
        Lock lock(write_request_mutex);
        const auto since = Clock::now();
        while (pending_write_request_bytes > config.max_pending_write_bytes - bytes) {
            if (stopping.load())
                throw FsError(EINTR, "FUSE write admission stopping");
            write_admission_waits.fetch_add(1, std::memory_order_relaxed);
            (void)write_request_cv.wait_for(lock.native(), admission_slice);
            note_admission_wait("write byte", since, bytes);
        }
        // Stop may free capacity and wake this waiter; admit nothing after stop.
        if (stopping.load())
            throw FsError(EINTR, "FUSE write admission stopping");
        pending_write_request_bytes += bytes;
        pending_write_request_bytes_diagnostic.store(pending_write_request_bytes,
                                                     std::memory_order_relaxed);
        auto peak = peak_pending_write_request_bytes.load(std::memory_order_relaxed);
        while (peak < pending_write_request_bytes &&
               !peak_pending_write_request_bytes.compare_exchange_weak(
                   peak, pending_write_request_bytes, std::memory_order_relaxed)) {
        }
        return std::make_shared<WriteRequestLease>([this, bytes] {
            {
                Lock release_lock(write_request_mutex);
                pending_write_request_bytes -= bytes;
                pending_write_request_bytes_diagnostic.store(
                    pending_write_request_bytes, std::memory_order_relaxed);
            }
            write_request_cv.notify_all();
        });
    }

    uint64_t spool_throttle_start() const {
        return std::max<uint64_t>(1, config.max_spool_bytes / 2);
    }

    bool spool_under_pressure() const {
        return spool_bytes.load(std::memory_order_acquire) >= spool_throttle_start();
    }

    uint64_t spool_progress_credit_limit() const {
        return config.max_spool_bytes - spool_throttle_start();
    }

    void note_spool_publication_progress(uint64_t bytes) {
        if (!bytes)
            return;
        Lock lock(spool_admission_mutex);
        const auto current = spool_bytes.load(std::memory_order_relaxed);
        if (current < spool_throttle_start() &&
            !spool_drain_requested.load(std::memory_order_acquire))
            return;
        const auto limit = spool_progress_credit_limit();
        spool_progress_credit_bytes =
            bytes > limit - std::min(limit, spool_progress_credit_bytes)
                ? limit
                : spool_progress_credit_bytes + bytes;
        ++spool_admission_revision;
        spool_admission_cv.notify_all();
    }

    void check_spool_physical_space(uint64_t bytes) {
        if (!bytes)
            return;
        std::filesystem::create_directories(spool_dir);
        std::error_code space_error;
        const auto space = std::filesystem::space(spool_dir, space_error);
        if (space_error)
            throw FsError(EIO, "cannot inspect FUSE spool free space");
        if (bytes > space.available || config.spool_reserve_free > space.available - bytes)
            throw FsError(ENOSPC, "FUSE spool physical reserve reached");
    }

    void reserve_spool_bytes(uint64_t bytes) {
        if (!bytes)
            return;
        if (bytes > config.max_spool_bytes)
            throw FsError(EFBIG, "single FUSE write exceeds the spool byte limit");

        const auto wait_started = Clock::now();
        bool waited = false;
        Lock lock(spool_admission_mutex);
        for (;;) {
            if (stopping.load(std::memory_order_acquire))
                throw FsError(EINTR, "FUSE spool admission stopping");

            const auto current = spool_bytes.load(std::memory_order_relaxed);
            const bool capacity_available =
                current <= config.max_spool_bytes && bytes <= config.max_spool_bytes - current;
            const auto throttle_start = spool_throttle_start();
            const auto now = Clock::now();

            bool rate_admitted = current < throttle_start;
            bool progress_admitted = false;
            Clock::time_point wake_at = Clock::time_point::max();
            if (capacity_available && !rate_admitted && bytes <= spool_progress_credit_bytes) {
                // Drained quanta admit writes even when a retirement sample
                // exists: a large file publishing quantum by quantum retires
                // nothing, so a stale small sample would otherwise set the pace.
                rate_admitted = true;
                progress_admitted = true;
            } else if (capacity_available && !rate_admitted &&
                       spool_publish_rate_bytes_per_second > 0.0) {
                const auto full_rate_at = std::max<uint64_t>(
                    throttle_start + 1,
                    config.max_spool_bytes - config.max_spool_bytes / 10);
                const auto pressure_span = full_rate_at - throttle_start;
                const auto pressure_bytes =
                    std::min(current - throttle_start, pressure_span);
                // From up to 8x the drain rate, converging to 1x at 90% occupancy.
                const double pressure = std::max(
                    0.125, static_cast<double>(pressure_bytes) /
                               static_cast<double>(std::max<uint64_t>(1, pressure_span)));
                const double admission_rate = spool_publish_rate_bytes_per_second / pressure;
                if (next_spool_admission <= now) {
                    rate_admitted = true;
                    const auto seconds = static_cast<double>(bytes) / admission_rate;
                    const auto delay = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(seconds));
                    next_spool_admission = now + std::max(delay, Clock::duration{1});
                } else {
                    wake_at = next_spool_admission;
                }
            }

            if (capacity_available && rate_admitted) {
                // Physical check precedes the reservation so a failure reserves
                // nothing.
                check_spool_physical_space(bytes);
                if (progress_admitted)
                    spool_progress_credit_bytes -= bytes;
                spool_bytes.store(current + bytes, std::memory_order_release);
                if (waited) {
                    spool_throttle_wait_ns.fetch_add(
                        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  Clock::now() - wait_started)
                                                  .count()),
                        std::memory_order_relaxed);
                }
                return;
            }

            if (!waited) {
                waited = true;
                spool_throttle_waits.fetch_add(1, std::memory_order_relaxed);
            }
            const auto revision = spool_admission_revision;
            // Only the transition into drain demand sweeps inodes; later
            // writes are notified at their durability-batch boundary rather
            // than rescanning every inode per blocked write.
            if (!spool_drain_requested.exchange(true, std::memory_order_acq_rel)) {
                spool_pressure_publication_sweeps.fetch_add(1, std::memory_order_relaxed);
                lock.unlock();
                request_spool_pressure_publications();
                lock.lock();
                // A release or rate sample may have raced the sweep.
                if (spool_admission_revision != revision)
                    continue;
            }
            const auto admission_changed = [&]() MACHA_REQUIRES(spool_admission_mutex) {
                return stopping.load(std::memory_order_acquire) ||
                       spool_admission_revision != revision;
            };
            if (wake_at == Clock::time_point::max())
                spool_admission_cv.wait(lock.native(), admission_changed);
            else
                spool_admission_cv.wait_until(lock.native(), wake_at, admission_changed);
        }
    }

    void recover_spool_bytes(uint64_t bytes) {
        if (!bytes)
            return;
        check_spool_physical_space(0);
        Lock lock(spool_admission_mutex);
        const auto current = spool_bytes.load(std::memory_order_relaxed);
        if (current > config.max_spool_bytes || bytes > config.max_spool_bytes - current)
            throw FsError(ENOSPC, "recovered FUSE spool exceeds configured byte limit");
        spool_bytes.store(current + bytes, std::memory_order_release);
    }

    void note_spool_publication_started() {
        Lock lock(spool_admission_mutex);
        spool_retirement_rate.start(Clock::now());
    }

    void release_spool_bytes(uint64_t bytes, bool retired = false) {
        if (!bytes)
            return;
        Lock lock(spool_admission_mutex);
        if (retired) {
            if (const auto sample = spool_retirement_rate.retire(bytes, Clock::now())) {
                spool_publish_rate_bytes_per_second = sample->bytes_per_second;
                spool_publish_rate_diagnostic.store(
                    static_cast<uint64_t>(spool_publish_rate_bytes_per_second),
                    std::memory_order_relaxed);
                spool_publish_rate_window_bytes.store(sample->bytes,
                                                      std::memory_order_relaxed);
                spool_publish_rate_window_ms.store(
                    static_cast<uint64_t>(sample->elapsed.count()),
                    std::memory_order_relaxed);
            }
        }
        const auto before = spool_bytes.load(std::memory_order_relaxed);
        if (before < bytes) {
            spool_bytes.store(0, std::memory_order_release);
            Log::error("FUSE spool accounting underflow");
        } else {
            spool_bytes.store(before - bytes, std::memory_order_release);
        }
        if (spool_bytes.load(std::memory_order_relaxed) < spool_throttle_start()) {
            next_spool_admission = {};
            spool_progress_credit_bytes = 0;
            spool_drain_requested.store(false, std::memory_order_release);
            spool_retirement_rate.reset();
        }
        ++spool_admission_revision;
        spool_admission_cv.notify_all();
    }

    struct OrphanFile {
        std::filesystem::path path;
        uint64_t size{};
        std::filesystem::file_time_type time{};
    };

    uint64_t make_orphan_room(uint64_t incoming) {
        std::filesystem::create_directories(spool_dir);
        std::vector<OrphanFile> orphans;
        uint64_t total = 0;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(spool_dir, error)) {
            if (error)
                throw FsError(EIO, "cannot enumerate FUSE orphan spool files");
            if (!entry.is_regular_file())
                continue;
            const auto name = entry.path().filename().string();
            if (name.find(".orphan.") == std::string::npos)
                continue;
            std::error_code size_error;
            const auto size = entry.file_size(size_error);
            if (size_error)
                continue;
            std::error_code time_error;
            const auto time = entry.last_write_time(time_error);
            const auto safe_time = time_error ? std::filesystem::file_time_type::min() : time;
            orphans.push_back({entry.path(), size, safe_time});
            total = size > std::numeric_limits<uint64_t>::max() - total
                        ? std::numeric_limits<uint64_t>::max()
                        : total + size;
        }
        std::sort(orphans.begin(), orphans.end(), [](const OrphanFile& a, const OrphanFile& b) {
            if (a.time != b.time)
                return a.time < b.time;
            return a.path.string() < b.path.string();
        });
        bool changed = false;
        for (const auto& orphan : orphans) {
            if (incoming <= config.max_orphan_bytes && total <= config.max_orphan_bytes - incoming)
                break;
            std::error_code remove_error;
            if (std::filesystem::remove(orphan.path, remove_error) && !remove_error) {
                total = orphan.size > total ? 0 : total - orphan.size;
                changed = true;
            }
        }
        if (changed)
            sync_directory(spool_dir);
        return total;
    }

    static constexpr std::array<uint8_t, 8> journal_magic{'M', 'A', 'C', 'H', 'F', 'U', 'S', '1'};

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

    static size_t encoded_namespace_op_size(const NamespaceOp& op) {
        Writer encoded;
        encode_namespace_op(encoded, op);
        return encoded.data().size();
    }

    static bool namespace_batch_compatible(std::span<const NamespaceOp> current,
                                           const NamespaceOp& candidate) {
        // Any run of ops batches: a batch publishes as one atomic mutation
        // identified in the mutation-sequence clock (journal_namespace_batch +
        // fuse_namespace_origin), so crash recovery asks the clock whether it
        // took effect rather than re-deriving each op's effect.
        (void)current;
        (void)candidate;
        return true;
    }

    [[maybe_unused]] static bool namespace_batch_compatible_legacy(
        std::span<const NamespaceOp> current, const NamespaceOp& candidate) {
        if (current.empty())
            return true;
        const auto is_delete = [](NamespaceOp::Kind kind) {
            return kind == NamespaceOp::Kind::unlink || kind == NamespaceOp::Kind::rmdir;
        };
        const auto is_create = [](NamespaceOp::Kind kind) {
            return kind == NamespaceOp::Kind::mkdir || kind == NamespaceOp::Kind::create;
        };
        const auto first = current.front().kind;
        if (is_delete(first) && is_delete(candidate.kind))
            return true;
        if (is_create(first) && is_create(candidate.kind))
            return std::none_of(current.begin(), current.end(), [&](const NamespaceOp& op) {
                return canonical_path(op.from) == canonical_path(candidate.from);
            });
        if ((first == NamespaceOp::Kind::chmod || first == NamespaceOp::Kind::chown ||
             first == NamespaceOp::Kind::utimens) &&
            candidate.kind == first) {
            return std::none_of(current.begin(), current.end(), [&](const NamespaceOp& op) {
                return canonical_path(op.from) == canonical_path(candidate.from);
            });
        }
        // Rename and mixed kinds stay singleton: their effects cannot be
        // proven from the final snapshot without a batch identity.
        return false;
    }

    static FilesystemNamespaceMutation filesystem_namespace_mutation(const NamespaceOp& op) {
        FilesystemNamespaceMutation out;
        switch (op.kind) {
        case NamespaceOp::Kind::mkdir:
            out.kind = FilesystemNamespaceMutation::Kind::mkdir;
            break;
        case NamespaceOp::Kind::create:
            out.kind = FilesystemNamespaceMutation::Kind::create;
            break;
        case NamespaceOp::Kind::rmdir:
            out.kind = FilesystemNamespaceMutation::Kind::rmdir;
            break;
        case NamespaceOp::Kind::unlink:
            out.kind = FilesystemNamespaceMutation::Kind::unlink;
            break;
        case NamespaceOp::Kind::rename:
            out.kind = FilesystemNamespaceMutation::Kind::rename;
            break;
        case NamespaceOp::Kind::chmod:
            out.kind = FilesystemNamespaceMutation::Kind::chmod;
            break;
        case NamespaceOp::Kind::chown:
            out.kind = FilesystemNamespaceMutation::Kind::chown;
            break;
        case NamespaceOp::Kind::utimens:
            out.kind = FilesystemNamespaceMutation::Kind::utimens;
            break;
        }
        out.from = op.from;
        out.to = op.to;
        out.noreplace = op.noreplace;
        out.mode = op.mode;
        out.uid = op.uid;
        out.gid = op.gid;
        out.set_uid = op.set_uid;
        out.set_gid = op.set_gid;
        out.mtime_ns = op.mtime_ns;
        return out;
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
        if (op.spool_hashes.size() > UINT32_MAX)
            throw FsError(EFBIG, "too many FUSE spool checksum chunks");
        writer.u32(static_cast<uint32_t>(op.spool_hashes.size()));
        for (const auto& hash : op.spool_hashes)
            writer.fixed(hash.bytes);
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
        if (reader.remaining()) {
            const auto count = reader.u32();
            const auto maximum =
                op.kind == DataOp::Kind::write
                    ? (op.length + spool_checksum_chunk_size - 1) / spool_checksum_chunk_size
                    : 0;
            if (count > maximum)
                throw DecodeError("FUSE data journal checksum count is invalid");
            op.spool_hashes.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Hash256 hash;
                hash.bytes = reader.fixed<32>();
                op.spool_hashes.push_back(hash);
            }
            if (op.kind == DataOp::Kind::write && count != maximum)
                throw DecodeError("FUSE data journal checksum coverage is incomplete");
        }
        op.metadata_charge = operation_metadata_charge(op.spool_hashes.size());
        return {inode, op};
    }

    void install_empty_journal_locked() MACHA_REQUIRES(journal_mutex) {
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

    void open_journal_append_locked() MACHA_REQUIRES(journal_mutex) {
        if (journal_fd >= 0)
            ::close(journal_fd);
        journal_fd = ::open(journal_path.c_str(), O_WRONLY | O_APPEND);
        if (journal_fd < 0)
            throw FsError(errno, "cannot open FUSE operation journal");
        journal_poisoned = false;
    }

    // `sync` false leaves the records to the next append's barrier: for a
    // record that means nothing until the one written straight after it.
    void append_journal_records_locked(const std::vector<Bytes>& payloads,
                                       bool new_admission = false, bool sync = true)
        MACHA_REQUIRES(journal_mutex) {
        if (journal_poisoned)
            throw FsError(EIO,
                          "FUSE operation journal is unavailable after a previous write failure");
        if (payloads.empty())
            return;
        const auto start = ::lseek(journal_fd, 0, SEEK_END);
        if (start < 0)
            throw FsError(errno, "cannot seek FUSE operation journal");

        uint64_t appended_bytes = 0;
        for (const auto& payload : payloads) {
            if (payload.size() > fuse_journal_max_record)
                throw FsError(EFBIG, "FUSE operation journal record too large");
            const uint64_t frame_size = 4ULL + payload.size() + 32ULL;
            if (frame_size > std::numeric_limits<uint64_t>::max() - appended_bytes)
                throw FsError(EFBIG, "FUSE operation journal batch size overflow");
            appended_bytes += frame_size;
        }
        if (new_admission &&
            (static_cast<uint64_t>(start) > config.max_operation_journal_bytes ||
             appended_bytes > config.max_operation_journal_bytes -
                                  std::min<uint64_t>(static_cast<uint64_t>(start),
                                                     config.max_operation_journal_bytes)))
            throw FsError(ENOSPC, "FUSE operation journal admission limit reached");

        try {
            for (const auto& payload : payloads) {
                auto frame = fuse_journal_frame(payload);
                write_exact(journal_fd, frame);
            }
            if (sync) {
                const auto sync_started = Clock::now();
                fsync_fd(journal_fd, "cannot sync FUSE operation journal");
                observations().record("fuse.journal_sync_us", elapsed_us(sync_started));
                journal_durability_barriers.fetch_add(1, std::memory_order_relaxed);
            }
            journal_append_batches.fetch_add(1, std::memory_order_relaxed);
            journal_records_appended.fetch_add(payloads.size(), std::memory_order_relaxed);
        } catch (...) {
            // A batch is all-or-nothing to recovery: roll back partial records,
            // or poison the journal if that fails.
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

    void append_journal_record_locked(std::span<const uint8_t> payload,
                                      bool new_admission = false, bool sync = true)
        MACHA_REQUIRES(journal_mutex) {
        std::vector<Bytes> payloads;
        payloads.emplace_back(payload.begin(), payload.end());
        append_journal_records_locked(payloads, new_admission, sync);
    }

    void reset_journal_locked() MACHA_REQUIRES(journal_admission_mutex, journal_mutex) {
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
        Lock admission_lock(journal_admission_mutex);
        Lock journal_lock(journal_mutex);
        reset_journal_locked();
    }

    // `sync` false when the operation that needs this descriptor is journalled
    // straight after it: that append's barrier makes both durable.
    void journal_inode_locked(const std::shared_ptr<Inode>& inode, bool sync = true)
        MACHA_REQUIRES(inode->mutex, journal_admission_mutex) {
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
        Lock journal_lock(journal_mutex);
        append_journal_record_locked(payload.data(), true, sync);
        inode->journal_epoch = epoch;
    }

    // Journals a rename's descriptors and operation under every inode's mutex,
    // taken in id order. Exempt from the analysis, which cannot follow a lock
    // set sized at run time.
    void journal_rename(const std::vector<std::shared_ptr<Inode>>& affected_inodes,
                        const std::shared_ptr<Inode>& displaced_inode, NamespaceOp& op)
        MACHA_REQUIRES(namespace_mutex) MACHA_NO_THREAD_SAFETY_ANALYSIS {
        std::vector<std::shared_ptr<Inode>> journal_inodes = affected_inodes;
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
            inode_locks.emplace_back(inode->mutex.native());

        // Lock order: inode locks before journal_admission_mutex.
        Lock journal_admission(journal_admission_mutex);
        for (const auto& inode : affected_inodes) {
            journal_inode_locked(inode, false);
            op.affected.push_back(inode->id);
        }
        if (displaced_inode) {
            journal_inode_locked(displaced_inode, false);
            op.removed.push_back(displaced_inode->id);
        }
        journal_namespace_operation(op);
    }

    void journal_namespace_operation(const NamespaceOp& op) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::namespace_op));
        encode_namespace_op(payload, op);
        Lock lock(journal_mutex);
        append_journal_record_locked(payload.data(), true);
        durable_pending_operations.fetch_add(1, std::memory_order_relaxed);
        namespace_operations_admitted.fetch_add(1, std::memory_order_relaxed);
    }

    void journal_data_operation(uint64_t inode, const DataOp& op) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_op));
        encode_data_op(payload, inode, op);
        Lock lock(journal_mutex);
        append_journal_record_locked(payload.data(), true);
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

        Lock admission_lock(journal_admission_mutex);
        {
            Lock journal_lock(journal_mutex);
            append_journal_records_locked(payloads, true);
        }
        durable_pending_operations.fetch_add(batch.size(), std::memory_order_relaxed);
        const auto previous =
            journal_inflight_admissions.fetch_sub(batch.size(), std::memory_order_relaxed);
        if (previous < batch.size()) {
            journal_inflight_admissions.store(0, std::memory_order_relaxed);
            Log::error("FUSE journal admission accounting underflow after durable batch");
        }
    }

    void journal_namespace_batch(uint64_t first_sequence, size_t count) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::namespace_batch));
        payload.u64(first_sequence);
        payload.u32(static_cast<uint32_t>(count));
        Lock lock(journal_mutex);
        append_journal_record_locked(payload.data());
    }

    void journal_namespace_published(std::span<const NamespaceOp> operations) {
        std::vector<Bytes> payloads;
        payloads.reserve(operations.size());
        for (const auto& op : operations) {
            Writer payload;
            payload.u8(static_cast<uint8_t>(JournalRecord::namespace_published));
            payload.u64(op.sequence);
            payloads.push_back(payload.data());
        }
        Lock lock(journal_mutex);
        append_journal_records_locked(payloads);
    }

    void journal_namespace_done(std::span<const NamespaceOp> operations) {
        if (operations.empty())
            return;
        Lock admission_lock(journal_admission_mutex);
        std::vector<Bytes> payloads;
        payloads.reserve(operations.size());
        for (const auto& op : operations) {
            Writer payload;
            payload.u8(static_cast<uint8_t>(JournalRecord::namespace_done));
            payload.u64(op.sequence);
            payloads.push_back(payload.data());
        }
        Lock lock(journal_mutex);
        append_journal_records_locked(payloads);
        const auto previous =
            durable_pending_operations.fetch_sub(operations.size(), std::memory_order_relaxed);
        if (previous < operations.size())
            throw std::logic_error("FUSE journal namespace completion underflow");
        if (previous == operations.size())
            reset_journal_locked();
    }

    void journal_data_published(uint64_t inode, uint64_t sequence, const FsEntry& entry) {
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_published));
        payload.u64(inode);
        payload.u64(sequence);
        encode_fuse_entry(payload, entry);
        Lock lock(journal_mutex);
        append_journal_record_locked(payload.data());
    }

    bool journal_data_done(uint64_t inode, uint64_t sequence, size_t retired) {
        Lock admission_lock(journal_admission_mutex);
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_done));
        payload.u64(inode);
        payload.u64(sequence);
        Lock lock(journal_mutex);
        append_journal_record_locked(payload.data());
        const auto previous =
            durable_pending_operations.fetch_sub(retired, std::memory_order_relaxed);
        if (previous < retired)
            throw std::logic_error("FUSE journal data completion underflow");
        // The caller compacts only after spool cleanup succeeds, so no spool
        // is ever left without a journal record explaining it.
        return previous == retired;
    }

    bool journal_data_abandoned(uint64_t inode, uint64_t sequence, size_t retired) {
        Lock admission_lock(journal_admission_mutex);
        Writer payload;
        payload.u8(static_cast<uint8_t>(JournalRecord::data_abandoned));
        payload.u64(inode);
        payload.u64(sequence);
        Lock lock(journal_mutex);
        append_journal_record_locked(payload.data());
        const auto previous =
            durable_pending_operations.fetch_sub(retired, std::memory_order_relaxed);
        if (previous < retired)
            throw std::logic_error("FUSE journal data abandonment underflow");
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

    void parse_journal_record(JournalRecovery& recovery, std::span<const uint8_t> payload,
                              size_t frame_offset) {
        note_startup_progress();
        Reader reader(payload);
        const auto raw_type = reader.u8();
        if (raw_type < static_cast<uint8_t>(JournalRecord::inode) ||
            raw_type > static_cast<uint8_t>(JournalRecord::namespace_batch))
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
            // Recovery re-journals a descriptor when it changes an inode's
            // paths (path-collision loser); the newest wins.
            recovery.inodes.insert_or_assign(inode.id, std::move(inode));
            break;
        }
        case JournalRecord::namespace_op: {
            auto op = decode_namespace_op(reader);
            if (!op.sequence)
                throw DecodeError("invalid FUSE namespace journal sequence");
            recovery.max_namespace_sequence =
                std::max(recovery.max_namespace_sequence, op.sequence);
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
            // Done need not follow published: an operator abandon
            // (skip_blocked_namespace_operation()) retires an op unpublished.
            if (!recovery.namespace_ops.contains(sequence) ||
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

            // data_done is the retirement watermark, written only after the
            // generation is committed and observed, and before spool
            // reclamation. It is trusted without data_published, but only if
            // the journal holds the exact op it retires.
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
                // Legitimate and repeated every boot until the journal resets:
                // DEBUG plus a count in the recovery summary.
                ++recovery.done_without_published;
                Log::debug("FUSE journal recovery accepted data completion without published "
                           "prefix inode=" +
                           std::to_string(inode) + " sequence=" + std::to_string(sequence) +
                           " frame_offset=" + std::to_string(frame_offset));
            }
            recovery.data_done[inode] = sequence;
            break;
        }
        case JournalRecord::namespace_batch: {
            const auto first = reader.u64();
            const auto count = reader.u32();
            if (!first || !count || count > 65536)
                throw DecodeError("invalid FUSE namespace batch record");
            recovery.namespace_batches[first] = count; // a re-attempt re-journals; last wins
            break;
        }
        case JournalRecord::data_abandoned: {
            const auto inode = reader.u64();
            const auto sequence = reader.u64();
            auto operations = recovery.data_ops.find(inode);
            const bool has_operation =
                operations != recovery.data_ops.end() &&
                std::any_of(operations->second.begin(), operations->second.end(),
                            [&](const DataOp& op) { return op.sequence == sequence; });
            if (!has_operation)
                throw DecodeError("FUSE data abandonment marker has no operation prefix");
            auto done = recovery.data_done.find(inode);
            if (done != recovery.data_done.end() && done->second >= sequence)
                throw DecodeError("non-monotonic FUSE data abandonment marker");
            recovery.data_done[inode] = sequence;
            break;
        }
        }
        reader.finish();
    }

    JournalRecovery load_journal() {
        std::filesystem::create_directories(spool_dir);
        std::filesystem::create_directories(journal_dir);
        Lock lock(journal_mutex);
        if (!std::filesystem::exists(journal_path))
            install_empty_journal_locked();

        int fd = ::open(journal_path.c_str(), O_RDWR);
        if (fd < 0)
            throw FsError(errno, "cannot open FUSE operation journal for recovery");
        try {
            struct stat statbuf{};
            if (::fstat(fd, &statbuf) != 0)
                throw FsError(errno, "cannot stat FUSE operation journal");
            if (statbuf.st_size < static_cast<off_t>(journal_magic.size()))
                throw std::runtime_error("FUSE operation journal header is missing");
            const uint64_t file_size = static_cast<uint64_t>(statbuf.st_size);
            auto pread_exact = [&](std::span<uint8_t> out, uint64_t offset) {
                size_t done = 0;
                while (done < out.size()) {
                    const auto n = ::pread(fd, out.data() + done, out.size() - done,
                                           static_cast<off_t>(offset + done));
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0)
                        return false;
                    done += static_cast<size_t>(n);
                }
                return true;
            };

            std::array<uint8_t, journal_magic.size()> magic{};
            if (!pread_exact(magic, 0) ||
                !std::equal(journal_magic.begin(), journal_magic.end(), magic.begin()))
                throw std::runtime_error("unsupported or corrupt FUSE operation journal header");

            JournalRecovery recovery;
            uint64_t position = journal_magic.size();
            uint64_t last_good = position;
            // Discipline 3: a frame that does not fit the state so far is
            // skipped and counted, never fatal. Each parse_journal_record()
            // throw is deterministic (duplicate marker, marker for a dropped
            // op, unknown record type), so refusing to start would only loop.
            std::map<std::string, std::pair<size_t, uint64_t>> skipped_reasons;
            std::optional<uint64_t> corrupt_at;
            while (position < file_size) {
                if (file_size - position < 4)
                    break;
                std::array<uint8_t, 4> length_bytes{};
                if (!pread_exact(length_bytes, position))
                    break;
                Reader length_reader(length_bytes);
                const auto length = static_cast<uint64_t>(length_reader.u32());
                length_reader.finish();
                if (length > fuse_journal_max_record)
                    throw std::runtime_error("FUSE operation journal record length is corrupt");
                const uint64_t frame_size = 4ULL + length + 32ULL;
                if (file_size - position < frame_size)
                    break;

                Bytes payload(static_cast<size_t>(length));
                std::array<uint8_t, 32> checksum{};
                if ((length && !pread_exact(payload, position + 4)) ||
                    !pread_exact(checksum, position + 4 + length))
                    break;
                Hash256 expected;
                expected.bytes = checksum;
                if (sha256(payload) != expected) {
                    if (position + frame_size == file_size)
                        break;
                    // Mid-journal corruption: quarantine the tail and start
                    // from the good prefix. Spool bytes whose ops were in the
                    // tail become orphans in validate_recovery_spools().
                    corrupt_at = position;
                    break;
                }

                try {
                    parse_journal_record(recovery, payload, static_cast<size_t>(position));
                } catch (const std::exception& error) {
                    auto& entry = skipped_reasons[error.what()];
                    if (!entry.first)
                        entry.second = position;
                    ++entry.first;
                    ++recovery.skipped_frames;
                }
                position += frame_size;
                last_good = position;
            }
            for (const auto& [reason, detail] : skipped_reasons)
                Log::warn("FUSE journal recovery skipped frames reason=\"" + reason +
                          "\" count=" + std::to_string(detail.first) +
                          " first_offset=" + std::to_string(detail.second));
            journal_recovery_skipped_frames.fetch_add(recovery.skipped_frames,
                                                      std::memory_order_relaxed);
            if (corrupt_at) {
                const auto quarantine = journal_path.string() + ".corrupt." +
                                        std::to_string(unix_ms()) + "." + std::to_string(getpid());
                ScopedFd target(::open(quarantine.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
                if (target.get() < 0)
                    throw FsError(errno, "cannot quarantine corrupt FUSE operation journal tail");
                Bytes buffer(256 * 1024);
                for (uint64_t offset = *corrupt_at; offset < file_size;) {
                    const auto chunk =
                        static_cast<size_t>(std::min<uint64_t>(buffer.size(), file_size - offset));
                    if (!pread_exact({buffer.data(), chunk}, offset))
                        throw FsError(EIO, "cannot read corrupt FUSE operation journal tail");
                    write_exact(target.get(), {buffer.data(), chunk});
                    offset += chunk;
                }
                fsync_fd(target.get(), "cannot sync quarantined FUSE operation journal tail");
                journal_recovery_quarantined_bytes.fetch_add(file_size - *corrupt_at,
                                                             std::memory_order_relaxed);
                Log::warn("FUSE operation journal checksum mismatch before EOF; quarantined the "
                          "tail and recovering the prefix offset=" +
                          std::to_string(*corrupt_at) +
                          " bytes=" + std::to_string(file_size - *corrupt_at) +
                          " quarantine=" + quarantine);
            }
            if (last_good != file_size) {
                if (::ftruncate(fd, static_cast<off_t>(last_good)) != 0)
                    throw FsError(errno, "cannot trim torn FUSE operation journal tail");
                fsync_fd(fd, "cannot sync trimmed FUSE operation journal");
                sync_directory(journal_dir);
                if (!corrupt_at)
                    Log::warn("trimmed incomplete FUSE operation journal tail bytes=" +
                              std::to_string(file_size - last_good));
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
                pending += static_cast<size_t>(
                    std::count_if(operations.begin(), operations.end(),
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
        Lock lock(durability_mutex);
        if (!durability_poisoned)
            return;
        if (durability_error)
            std::rethrow_exception(durability_error);
        throw FsError(EIO, "FUSE durability coordinator is unavailable");
    }

    void wait_for_inode_durability(const std::shared_ptr<Inode>& inode, Clock::time_point deadline,
                                   std::atomic_bool& cancelled) {
        Lock lock(inode->mutex);
        while (inode->durability_pending && !inode->backend_error) {
            if (deadline == Clock::time_point::max()) {
                inode->durability_cv.wait(lock.native());
            } else if (inode->durability_cv.wait_until(lock.native(), deadline) ==
                       std::cv_status::timeout) {
                check_deadline(deadline, cancelled);
            }
        }
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "FUSE inode durability error");
        check_deadline(deadline, cancelled);
    }

    uint64_t durable_sequence(const std::shared_ptr<Inode>& inode) const {
        Lock lock(inode->mutex);
        return inode->durable_data_sequence;
    }

    void wait_for_inode_publication(const std::shared_ptr<Inode>& inode, uint64_t target,
                                    Clock::time_point deadline, std::atomic_bool& cancelled) {
        if (!target)
            return;
        Lock queue_lock(data_queue_mutex);
        while (true) {
            {
                Lock inode_lock(inode->mutex);
                if (inode->backend_error)
                    throw FsError(*inode->backend_error, "FUSE inode publication error");
                // Advanced only after PublicationWriter::commit(): cluster-durable at
                // this watermark even if local confirmation lags.
                if (inode->published_data_sequence >= target)
                    return;
            }
            // interrupt_waits() sets this under data_queue_mutex, so no missed
            // notify.
            if (waits_interrupted.load(std::memory_order_acquire))
                throw FsError(EIO, "FUSE frontend stopping; the data is journalled and "
                                   "publishes after the restart");
            check_deadline(deadline, cancelled);
            if (deadline == Clock::time_point::max()) {
                data_cv.wait(queue_lock.native());
            } else if (data_cv.wait_until(queue_lock.native(), deadline) ==
                       std::cv_status::timeout) {
                check_deadline(deadline, cancelled);
            }
        }
    }

    void enqueue_durability(const std::shared_ptr<DurabilityTicket>& ticket) {
        {
            Lock lock(durability_mutex);
            // Queue even if poisoned: the coordinator owns failure accounting.
            durability_queue.push_back(ticket);
            retained_durability_tickets.fetch_add(1, std::memory_order_relaxed);
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

    static void close_idle_spool_locked(Inode& inode) MACHA_REQUIRES(inode.mutex) {
        // The fd only bridges admission to the durability barrier; publication
        // and reads reopen spool_path, so holding it would make a backlog cost
        // one fd per dirty inode.
        if (inode.durability_pending || inode.spool_fd < 0)
            return;
        ::close(inode.spool_fd);
        inode.spool_fd = -1;
    }

    void fail_durability_batch(const std::vector<std::shared_ptr<DurabilityTicket>>& batch,
                               const std::exception_ptr& error) {
        {
            Lock admission_lock(journal_admission_mutex);
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
                Lock lock(ticket->inode->mutex);
                ticket->inode->backend_error = code;
                if (ticket->inode->durability_pending)
                    --ticket->inode->durability_pending;
                if (!ticket->inode->durability_pending) {
                    ticket->inode->admitted_size = ticket->inode->visible.size;
                    close_idle_spool_locked(*ticket->inode);
                }
            }
            ticket->inode->durability_cv.notify_all();
        }
    }

    void finish_durability_batch(const std::vector<std::shared_ptr<DurabilityTicket>>& batch) {
        for (const auto& ticket : batch) {
            auto inode = ticket->inode;
            {
                Lock lock(inode->mutex);
                // One FIFO worker, so per-inode sequences become durable in
                // admission order.
                inode->durable_data_sequence =
                    std::max(inode->durable_data_sequence, ticket->op.sequence);
                if (inode->durability_pending)
                    --inode->durability_pending;
                if (!inode->durability_pending) {
                    inode->admitted_size = inode->visible.size;
                    close_idle_spool_locked(*inode);
                }
            }
            inode->durability_cv.notify_all();
        }
        // Wake metadata-bound writers to enqueue the new durable prefix.
        operation_metadata_cv.notify_all();
    }

    void durability_loop(std::stop_token stop) {
        while (true) {
            std::vector<std::shared_ptr<DurabilityTicket>> batch;
            {
                Lock lock(durability_mutex);
                durability_cv.wait(lock.native(), [&]() MACHA_REQUIRES(durability_mutex) {
                    return stop.stop_requested() || !durability_queue.empty();
                });
                if (durability_queue.empty() && stop.stop_requested())
                    break;

                // Brief coalescing window to amortise the spool+journal fsync
                // pair; close/release and fsync wait for the watermark.
                if (!stop.stop_requested())
                    durability_cv.wait_for(
                        lock.native(), std::chrono::milliseconds(25),
                        [&]() MACHA_REQUIRES(durability_mutex) { return stop.stop_requested(); });
                batch.assign(durability_queue.begin(), durability_queue.end());
                durability_queue.clear();
                retained_durability_tickets.fetch_sub(batch.size(), std::memory_order_relaxed);
                if (durability_poisoned) {
                    auto error = durability_error
                                     ? durability_error
                                     : std::make_exception_ptr(FsError(
                                           EIO, "FUSE durability coordinator is unavailable"));
                    lock.unlock();
                    fail_durability_batch(batch, error);
                    continue;
                }
            }

            const auto started = Clock::now();
            try {
                std::set<int> spool_fds;
                for (const auto& ticket : batch) {
                    Lock lock(ticket->inode->mutex);
                    if (ticket->inode->spool_fd < 0)
                        throw FsError(EIO, "missing FUSE write spool during durability commit");
                    spool_fds.insert(ticket->inode->spool_fd);
                }
                for (auto fd : spool_fds)
                    fsync_fd(fd, "FUSE local spool group sync failed");

                // Payload is durable before its descriptor, so recovery never
                // sees a data-op whose spool bytes were not synced.
                journal_data_batch(batch);
                finish_durability_batch(batch);
                if (spool_under_pressure() ||
                    spool_drain_requested.load(std::memory_order_acquire)) {
                    std::set<std::shared_ptr<Inode>> pressure_inodes;
                    for (const auto& ticket : batch)
                        pressure_inodes.insert(ticket->inode);
                    for (const auto& inode : pressure_inodes)
                        request_data_publication(inode);
                }
                durability_batches.fetch_add(1, std::memory_order_relaxed);
                durability_writes.fetch_add(batch.size(), std::memory_order_relaxed);

                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
                        .count();
                if (Log::enabled(LogLevel::debug) && elapsed >= 100)
                    Log::debug("DIAG FUSE durability batch writes=" + std::to_string(batch.size()) +
                               " spool_fds=" + std::to_string(spool_fds.size()) +
                               " elapsed_ms=" + std::to_string(elapsed));
            } catch (...) {
                auto error = std::current_exception();
                {
                    Lock lock(durability_mutex);
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

    std::shared_ptr<Inode> resolve_locked(std::string_view path) const
        MACHA_REQUIRES(namespace_mutex) {
        auto found = paths.find(canonical_path(path));
        if (found == paths.end())
            throw FsError(ENOENT, "missing");
        return found->second;
    }

    // The inodes whose paths lie directly inside a directory, in key order;
    // `each` returns false to stop. `paths` is ordered by canonical path, so
    // a directory's contents are one key range, and a child directory's own
    // contents are stepped over.
    template <typename Each>
    void for_each_child_locked(std::string_view directory, Each&& each) const
        MACHA_REQUIRES(namespace_mutex) {
        const auto key = canonical_path(directory);
        const auto prefix = key == "/" ? std::string("/") : key + "/";
        for (auto it = paths.lower_bound(prefix); it != paths.end();) {
            const std::string_view path = it->first;
            if (!path.starts_with(prefix))
                break;
            const auto rest = path.substr(prefix.size());
            const auto slash = rest.find('/');
            if (rest.empty()) {
                ++it;
            } else if (slash == std::string_view::npos) {
                if (!each(it->second))
                    return;
                ++it;
            } else {
                // '0' follows '/': the first key past this child's contents.
                it = paths.lower_bound(prefix + std::string(rest.substr(0, slash)) + "0");
            }
        }
    }

    // The inodes at or beneath a path.
    template <typename Each>
    void for_each_under_locked(std::string_view root, Each&& each) const
        MACHA_REQUIRES(namespace_mutex) {
        const auto key = canonical_path(root);
        if (key == "/") {
            for (const auto& [_, inode] : paths)
                each(inode);
            return;
        }
        if (const auto self = paths.find(key); self != paths.end())
            each(self->second);
        const auto prefix = key + "/";
        for (auto it = paths.lower_bound(prefix);
             it != paths.end() && std::string_view(it->first).starts_with(prefix); ++it)
            each(it->second);
    }

    std::shared_ptr<Inode> resolve_inode(uint64_t id) const {
        Lock lock(namespace_mutex);
        auto found = inodes.find(id);
        if (found == inodes.end())
            throw FsError(EBADF, "unknown FUSE inode");
        return found->second;
    }

    void require_parent_locked(const std::string& path) const MACHA_REQUIRES(namespace_mutex) {
        auto parent = parent_path(path);
        auto found = paths.find(canonical_path(parent));
        if (found == paths.end())
            throw FsError(ENOENT, "parent missing");
        Lock inode_lock(found->second->mutex);
        if (found->second->visible.type != EntryType::directory)
            throw FsError(ENOTDIR, "parent not directory");
    }

    // Pure questions about a snapshot; `nodes` supplies tree nodes.
    static bool snapshot_has_path(const MetadataSnapshot& snapshot,
                                  const NamespaceNodeStore& nodes, std::string_view path) {
        return namespace_contains(snapshot, &nodes, normalize_path(std::string(path)));
    }

    // `with_extents` false for stat-only callers: skips reading or copying the
    // extent list.
    static std::optional<FsEntry> snapshot_entry(const MetadataSnapshot& snapshot,
                                                 const NamespaceNodeStore& nodes,
                                                 std::string_view path, bool with_extents = true) {
        return namespace_entry(snapshot, &nodes, normalize_path(std::string(path)), with_extents);
    }

    // Stat fields only, so every read skips extents.
    static bool namespace_effect_confirmed(const NamespaceOp& op, const MetadataSnapshot& snapshot,
                                           const NamespaceNodeStore& nodes) {
        switch (op.kind) {
        case NamespaceOp::Kind::mkdir: {
            auto entry = snapshot_entry(snapshot, nodes, op.from, false);
            return entry && entry->type == EntryType::directory &&
                   entry->mode == (op.mode & 07777) && entry->uid == op.uid && entry->gid == op.gid;
        }
        case NamespaceOp::Kind::create: {
            auto entry = snapshot_entry(snapshot, nodes, op.from, false);
            return entry && entry->type == EntryType::file && entry->mode == (op.mode & 07777) &&
                   entry->uid == op.uid && entry->gid == op.gid;
        }
        case NamespaceOp::Kind::rmdir:
        case NamespaceOp::Kind::unlink:
            return !snapshot_has_path(snapshot, nodes, op.from);
        case NamespaceOp::Kind::rename:
            return !snapshot_has_path(snapshot, nodes, op.from) &&
                   snapshot_has_path(snapshot, nodes, op.to);
        case NamespaceOp::Kind::chmod: {
            auto entry = snapshot_entry(snapshot, nodes, op.from, false);
            return entry && entry->mode == (op.mode & 07777);
        }
        case NamespaceOp::Kind::chown: {
            auto entry = snapshot_entry(snapshot, nodes, op.from, false);
            return entry && (!op.set_uid || entry->uid == op.uid) &&
                   (!op.set_gid || entry->gid == op.gid);
        }
        case NamespaceOp::Kind::utimens: {
            auto entry = snapshot_entry(snapshot, nodes, op.from, false);
            return entry && entry->mtime_ns == op.mtime_ns;
        }
        }
        return false;
    }

    // Confirmed once a view at or past the op's commit generation exists (it
    // holds the effect or its successor); effect visibility also confirms a
    // view that lags the commit.
    static bool namespace_op_confirmed(const NamespaceOp& op, const MetadataSnapshotView& view,
                                       const NamespaceNodeStore& nodes) {
        if (op.published_generation && view.generation >= op.published_generation)
            return true;
        return namespace_effect_confirmed(op, *view.snapshot, nodes);
    }

    static std::string_view namespace_op_kind_name(NamespaceOp::Kind kind) {
        switch (kind) {
        case NamespaceOp::Kind::mkdir: return "mkdir";
        case NamespaceOp::Kind::create: return "create";
        case NamespaceOp::Kind::rmdir: return "rmdir";
        case NamespaceOp::Kind::unlink: return "unlink";
        case NamespaceOp::Kind::rename: return "rename";
        case NamespaceOp::Kind::chmod: return "chmod";
        case NamespaceOp::Kind::chown: return "chown";
        case NamespaceOp::Kind::utimens: return "utimens";
        }
        return "unknown";
    }

    static bool contains_inode(const std::vector<uint64_t>& ids, uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    static std::vector<uint64_t> namespace_operation_inodes(const NamespaceOp& op) {
        auto ids = op.affected;
        ids.insert(ids.end(), op.removed.begin(), op.removed.end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }

    void note_inode_inserted_locked() MACHA_REQUIRES(namespace_mutex) {
        const auto current = inode_count.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = peak_inode_count.load(std::memory_order_relaxed);
        while (peak < current && !peak_inode_count.compare_exchange_weak(
                                     peak, current, std::memory_order_relaxed)) {
        }
    }

    // Call holding neither namespace_mutex nor inode.mutex. A shared_ptr may
    // outlive table removal, but no new lookup can reach a reclaimed inode.
    void reclaim_inode_if_quiescent(uint64_t id) {
        Lock namespace_lock(namespace_mutex);
        auto found = inodes.find(id);
        if (found == inodes.end() || id == 1)
            return;
        const auto& inode = found->second;
        Lock inode_lock(inode->mutex);
        const bool detached = !inode->published_path;
        const bool no_handles = !inode->open_handles && !inode->writable_handles;
        const bool no_namespace_owner =
            !inode->namespace_references.load(std::memory_order_relaxed);
        const bool no_durability_owner = !inode->durability_pending;
        const bool no_data_owner = inode->data_ops.empty() && !inode->data_enqueue_pending &&
                                   !inode->data_queued && !inode->data_running &&
                                   !inode->data_deferred && !inode->data_publication &&
                                   !inode->unconfirmed_data_entry &&
                                   !inode->unconfirmed_data_sequence;
        const bool no_spool_owner = inode->spool_fd < 0 && inode->spool_path.empty() &&
                                    !inode->spool_end;
        if (!detached || !no_handles || !no_namespace_owner || !no_durability_owner ||
            !no_data_owner || !no_spool_owner)
            return;

        // A path edge is an owner; check the index as well as published_path.
        // An inode stands in the index under its own current path and
        // nowhere else.
        if (const auto edge = paths.find(canonical_path(inode->current_path));
            edge != paths.end() && edge->second.get() == inode.get())
            return;
        release_retained_owners_locked(*inode);
        inodes.erase(found);
        inode_count.fetch_sub(1, std::memory_order_relaxed);
        reclaimed_inode_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Install before the op's path edges stop owning their inodes.
    void retain_namespace_references_locked(const NamespaceOp& op)
        MACHA_REQUIRES(namespace_mutex) {
        for (auto id : namespace_operation_inodes(op)) {
            auto found = inodes.find(id);
            if (found == inodes.end())
                throw std::logic_error("durable namespace operation references unknown inode");
            found->second->namespace_references.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release_namespace_references(std::span<const NamespaceOp> operations) {
        std::vector<uint64_t> candidates;
        {
            Lock namespace_lock(namespace_mutex);
            for (const auto& op : operations) {
                for (auto id : namespace_operation_inodes(op)) {
                    auto found = inodes.find(id);
                    if (found == inodes.end())
                        continue;
                    const auto previous = found->second->namespace_references.fetch_sub(
                        1, std::memory_order_relaxed);
                    if (!previous) {
                        found->second->namespace_references.fetch_add(
                            1, std::memory_order_relaxed);
                        throw std::logic_error("FUSE namespace ownership underflow");
                    }
                    candidates.push_back(id);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (auto id : candidates)
            reclaim_inode_if_quiescent(id);
    }

    static bool snapshot_path_shadowed(std::string_view path, const JournalRecovery& recovery) {
        const auto key = canonical_path(path);
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (recovery.namespace_done.contains(sequence))
                continue;
            switch (op.kind) {
            case NamespaceOp::Kind::rename:
                // Locally the source subtree and any displaced destination are
                // gone; a stale snapshot must not resurrect either.
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

    size_t namespace_pending_locked() const MACHA_REQUIRES(namespace_queue_mutex) {
        return namespace_queue.size() + namespace_unconfirmed.size() +
               namespace_inflight_operations;
    }

    bool namespace_capacity_available() {
        Lock lock(namespace_queue_mutex);
        return namespace_pending_locked() < config.max_pending_operations;
    }

    // The caller holds namespace_mutex across journal admission, local
    // mutation and this enqueue.
    void enqueue_namespace(NamespaceOp operation) MACHA_REQUIRES(namespace_mutex) {
        retain_namespace_references_locked(operation);
        {
            Lock lock(namespace_queue_mutex);
            // Capacity was checked at admission. Never reject here: the op is
            // already durable, and the worker may transiently count one op
            // twice (inflight and unconfirmed).
            namespace_queue.push_back(std::move(operation));
        }
        namespace_cv.notify_one();
    }

    int ensure_spool_locked(const std::shared_ptr<Inode>& inode) MACHA_REQUIRES(inode->mutex) {
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
            // A new spool's directory entry is durable before any journal
            // record refers to it.
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

    bool retire_spool_locked(Inode& inode) MACHA_REQUIRES(inode.mutex) {
        if (inode.spool_path.empty()) {
            if (inode.spool_fd >= 0) {
                ::close(inode.spool_fd);
                inode.spool_fd = -1;
            }
            const auto retired_bytes = inode.spool_end;
            inode.spool_end = 0;
            release_spool_bytes(retired_bytes, true);
            return true;
        }

        // data_done/data_abandoned is already durable, so unlink without a
        // directory fsync: a lost unlink leaves a stale spool that startup
        // removes.
        if (inode.spool_fd >= 0) {
            ::close(inode.spool_fd);
            inode.spool_fd = -1;
        }

        const auto retired_path = inode.spool_path;
        const auto retired_bytes = inode.spool_end;
        if (::unlink(retired_path.c_str()) != 0 && errno != ENOENT) {
            Log::warn("cannot unlink retired FUSE spool inode=" + std::to_string(inode.id) +
                      " error=" + std::strerror(errno));
            return false;
        }
        inode.spool_end = 0;
        inode.spool_path.clear();
        release_spool_bytes(retired_bytes, true);
        return true;
    }

    void request_data_publication(const std::shared_ptr<Inode>& inode) {
        bool enqueue = false;
        {
            Lock inode_lock(inode->mutex);
            // A terminal backend error holds until a namespace op or operator
            // clears it; re-admitting the same generation would only loop.
            if (inode->backend_error || inode->data_ops.empty() ||
                inode->durable_data_sequence <= inode->published_data_sequence)
                return;
            const bool watermark_advanced =
                inode->durable_data_sequence > inode->requested_data_sequence ||
                inode->namespace_sequence > inode->requested_namespace_sequence;
            // A repeat of the same prefix with an existing owner is not a
            // request and must not touch the shared queue.
            if (!watermark_advanced &&
                (inode->unconfirmed_data_entry || inode->data_enqueue_pending ||
                 inode->data_queued || inode->data_running || inode->data_deferred)) {
                data_publication_notifications_suppressed.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            data_publication_requests.fetch_add(1, std::memory_order_relaxed);
            // Only the durable prefix is publishable; later writes stay local.
            inode->requested_data_sequence =
                std::max(inode->requested_data_sequence, inode->durable_data_sequence);
            inode->requested_namespace_sequence =
                std::max(inode->requested_namespace_sequence, inode->namespace_sequence);
            // At most one committed-but-unobserved generation per inode, so an
            // older metadata view never becomes the base.
            if (inode->unconfirmed_data_entry) {
                inode->data_deferred = true;
                ++merged_publications;
                data_publication_coalesced_unconfirmed.fetch_add(1,
                                                                 std::memory_order_relaxed);
                return;
            }
            if (inode->data_enqueue_pending || inode->data_queued) {
                ++merged_publications;
                data_publication_coalesced_queued.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (inode->data_running) {
                ++merged_publications;
                data_publication_coalesced_running.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            inode->data_enqueue_pending = true;
            enqueue = true;
        }
        if (!enqueue)
            return;

        Lock queue_lock(data_queue_mutex);
        Lock inode_lock(inode->mutex);
        if (!inode->data_enqueue_pending)
            return;
        inode->data_enqueue_pending = false;
        if (inode->data_queued || inode->data_running || inode->unconfirmed_data_entry)
            return;
        if (data_queue.size() >= config.max_pending_operations) {
            inode->data_deferred = true;
            return;
        }
        inode->data_queued = true;
        inode->data_deferred = false;
        const bool recovered = inode->published_data_sequence < inode->recovery_data_sequence;
        data_queue.push_back({inode, recovered});
        data_cv.notify_one();
    }

    void request_spool_pressure_publications() {
        std::vector<std::shared_ptr<Inode>> candidates;
        {
            Lock lock(namespace_mutex);
            candidates.reserve(inodes.size());
            for (const auto& [_, inode] : inodes) {
                Lock inode_lock(inode->mutex);
                if (!inode->data_ops.empty() &&
                    inode->durable_data_sequence > inode->published_data_sequence)
                    candidates.push_back(inode);
            }
        }
        for (const auto& inode : candidates)
            request_data_publication(inode);
    }

    // The idle data loop sleeps to the earliest due time and re-admits.
    void note_deferred_due(Clock::time_point due) {
        const auto ns = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count());
        auto current = deferred_retry_due_ns.load(std::memory_order_relaxed);
        while ((current == 0 || ns < current) &&
               !deferred_retry_due_ns.compare_exchange_weak(current, ns,
                                                            std::memory_order_acq_rel)) {
        }
        data_cv.notify_all();
    }

    // A parked publication is tried again once the nodes or storage it
    // depends on have changed since it was parked.
    void readmit_parked_on_change() {
        if (!parked_publications.load(std::memory_order_relaxed))
            return;
        const auto epoch = publication_target.reachability_epoch();
        std::vector<std::shared_ptr<Inode>> candidates;
        {
            Lock lock(namespace_mutex);
            candidates.reserve(inodes.size());
            for (const auto& [_, inode] : inodes)
                candidates.push_back(inode);
        }
        size_t readmitted = 0;
        for (const auto& inode : candidates) {
            Lock inode_lock(inode->mutex);
            if (!inode->parked || inode->parked->reachability_epoch == epoch)
                continue;
            inode->parked.reset();
            inode->publication_retry.reset();
            inode->data_deferred = true;
            ++readmitted;
        }
        if (!readmitted)
            return;
        parked_publications.fetch_sub(readmitted, std::memory_order_relaxed);
        Log::info("FUSE data publication retry after a membership or storage change parked=" +
                  std::to_string(readmitted));
        admit_deferred();
    }

    void admit_deferred() {
        std::vector<std::shared_ptr<Inode>> candidates;
        {
            Lock lock(namespace_mutex);
            candidates.reserve(inodes.size());
            for (const auto& [_, inode] : inodes)
                candidates.push_back(inode);
        }
        std::optional<Clock::time_point> earliest_due;
        const auto now = Clock::now();
        {
            Lock queue_lock(data_queue_mutex);
            for (const auto& inode : candidates) {
                if (data_queue.size() >= config.max_pending_operations)
                    break;
                Lock inode_lock(inode->mutex);
                if (inode->backend_error || inode->parked) {
                    inode->data_deferred = false;
                    continue;
                }
                if (!inode->data_deferred || inode->data_enqueue_pending || inode->data_queued ||
                    inode->data_running || inode->unconfirmed_data_entry)
                    continue;
                if (!inode->publication_retry.due(now)) {
                    const auto due = inode->publication_retry.due_at();
                    if (!earliest_due || due < *earliest_due)
                        earliest_due = due;
                    continue;
                }
                inode->data_deferred = false;
                inode->data_queued = true;
                const bool recovered =
                    inode->published_data_sequence < inode->recovery_data_sequence;
                data_queue.push_back({inode, recovered});
            }
            data_cv.notify_all();
        }
        if (earliest_due)
            note_deferred_due(*earliest_due);
    }

    void mark_backend_error(const std::vector<uint64_t>& affected, int error) {
        Lock lock(namespace_mutex);
        for (auto id : affected) {
            auto found = inodes.find(id);
            if (found == inodes.end())
                continue;
            Lock inode_lock(found->second->mutex);
            found->second->backend_error = error;
        }
    }

    FilesystemNamespaceBatchResult
    apply_namespace_backend(std::span<const NamespaceOp> operations,
                            std::optional<MetadataMutationIdentity> identity = {},
                            bool atomic = false) {
        std::vector<FilesystemNamespaceMutation> mutations;
        mutations.reserve(operations.size());
        for (const auto& op : operations)
            mutations.push_back(filesystem_namespace_mutation(op));
        return fs.apply_namespace_batch(mutations, identity, atomic);
    }

    // Idempotency key derived from this node's id; its clock in
    // MetadataSnapshot::mutation_sequences advances to a batch's first op
    // sequence when the batch commits. Set by the constructor.
    NodeId fuse_namespace_origin{};

    static NodeId derive_fuse_namespace_origin(const NodeId& node) {
        Writer w;
        w.raw(std::array<uint8_t, 16>{'m', 'a', 'c', 'h', 'a', '-', 'f', 'u', 's', 'e', '-', 'n',
                                      's', 'o', 'p', '1'});
        w.fixed(node.bytes);
        const auto digest = sha256(w.data());
        NodeId out;
        std::copy_n(digest.bytes.begin(), out.bytes.size(), out.bytes.begin());
        return out;
    }

    uint64_t fuse_namespace_clock(const MetadataSnapshot& snapshot) const {
        auto found = snapshot.mutation_sequences.find(fuse_namespace_origin);
        return found == snapshot.mutation_sequences.end() ? 0 : found->second;
    }

    void namespace_success(const NamespaceOp& op, const MetadataSnapshot* snapshot = nullptr) {
        Lock lock(namespace_mutex);
        for (auto id : op.affected) {
            auto found = inodes.find(id);
            if (found == inodes.end())
                continue;
            Lock inode_lock(found->second->mutex);
            found->second->backend_error.reset();
        }
        if (op.kind == NamespaceOp::Kind::rename) {
            for (auto id : op.removed) {
                auto found = inodes.find(id);
                if (found == inodes.end())
                    continue;
                Lock inode_lock(found->second->mutex);
                found->second->published_path.reset();
            }
            for (auto id : op.affected) {
                auto found = inodes.find(id);
                if (found == inodes.end())
                    continue;
                Lock inode_lock(found->second->mutex);
                if (found->second->published_path &&
                    under_path(*found->second->published_path, op.from))
                    found->second->published_path =
                        op.to + found->second->published_path->substr(op.from.size());
            }
        } else if (op.kind == NamespaceOp::Kind::unlink || op.kind == NamespaceOp::Kind::rmdir) {
            for (auto id : op.affected) {
                auto found = inodes.find(id);
                if (found == inodes.end())
                    continue;
                Lock inode_lock(found->second->mutex);
                found->second->published_path.reset();
            }
        } else if (op.kind == NamespaceOp::Kind::mkdir || op.kind == NamespaceOp::Kind::create) {
            if (!op.affected.empty()) {
                auto found = inodes.find(op.affected.front());
                if (found != inodes.end()) {
                    Lock inode_lock(found->second->mutex);
                    found->second->published_path = op.from;
                    if (snapshot) {
                        if (auto entry = snapshot_entry(*snapshot, fs.namespace_nodes(), op.from))
                            found->second->base = *entry;
                    }
                }
            }
        }
    }

    void namespace_loop(std::stop_token stop) {
        while (!stop.stop_requested() && !stopping.load()) {
            std::vector<NamespaceOp> batch;
            {
                Lock lock(namespace_queue_mutex);
                namespace_cv.wait(lock.native(), stop, [&]() MACHA_REQUIRES(namespace_queue_mutex) {
                    return !namespace_queue.empty() || stopping.load();
                });
                if (stop.stop_requested() || stopping.load())
                    break;
                batch.push_back(namespace_queue.front());
                namespace_queue.pop_front();
                size_t encoded_bytes = encoded_namespace_op_size(batch.front());
                while (!namespace_queue.empty() &&
                       batch.size() < config.namespace_batch_operations &&
                       namespace_batch_compatible(batch, namespace_queue.front())) {
                    const auto candidate_bytes = encoded_namespace_op_size(namespace_queue.front());
                    if (encoded_bytes > config.namespace_batch_bytes ||
                        candidate_bytes > config.namespace_batch_bytes - encoded_bytes)
                        break;
                    encoded_bytes += candidate_bytes;
                    batch.push_back(namespace_queue.front());
                    namespace_queue.pop_front();
                }
                namespace_inflight = true;
                namespace_inflight_sequence = batch.front().sequence;
                namespace_inflight_operations = batch.size();
            }

            size_t published_prefix = 0;
            std::optional<int> prefix_failure_code;
            std::string prefix_failure_message;
            std::shared_ptr<const MetadataSnapshot> published_snapshot;
            RetryState retry;
            bool budget_reported = false;
            // Batches publish under their identity; one reduced to a singleton
            // by a refused op uses per-op effect checks.
            bool identity_batch = true;
            bool batch_journaled = false;
            while (!published_prefix && !stop.stop_requested() && !stopping.load()) {
                bool skip_requested = false;
                {
                    Lock lock(namespace_queue_mutex);
                    if (namespace_skip_requested_sequence &&
                        namespace_skip_requested_sequence == batch.front().sequence) {
                        namespace_skip_requested_sequence = 0;
                        namespace_blocked_op.reset();
                        skip_requested = true;
                    }
                }
                if (skip_requested) {
                    // Operator abandon: retire without claiming the effect
                    // (inodes keep backend_error) and continue the batch.
                    const NamespaceOp abandoned = batch.front();
                    const std::array<NamespaceOp, 1> abandoned_span{abandoned};
                    journal_namespace_done(abandoned_span);
                    release_namespace_references(abandoned_span);
                    namespace_view_diverged.store(true, std::memory_order_release);
                    Log::warn("FUSE async namespace publication operator-skipped seq=" +
                             std::to_string(abandoned.sequence));
                    batch.erase(batch.begin());
                    if (batch.empty())
                        break;
                    retry.reset();
                    budget_reported = false;
                    continue;
                }
                try {
                    // A crash may leave an effect without its local marker: an
                    // identity batch asks the snapshot clock, a singleton
                    // checks the effect.
                    auto before = fs.local_snapshot_view();
                    const MetadataMutationIdentity identity{fuse_namespace_origin,
                                                            batch.front().sequence};
                    // Otherwise retire a leading run of ops whose effect is
                    // already visible; safe for every kind as nothing re-applies.
                    if (identity_batch && fuse_namespace_clock(*before.snapshot) >= identity.sequence)
                        published_prefix = batch.size();
                    while (published_prefix < batch.size() &&
                           namespace_effect_confirmed(batch[published_prefix], *before.snapshot,
                                                      fs.namespace_nodes()))
                        ++published_prefix;
                    uint64_t published_generation = before.generation;

                    if (published_prefix < batch.size()) {
                        namespace_publication_attempts.fetch_add(1, std::memory_order_relaxed);
                        const auto remaining =
                            std::span<const NamespaceOp>(batch).subspan(published_prefix);
                        // Two or more ops go as one atomic mutation under the
                        // batch identity; a single op needs no identity.
                        const bool atomic = identity_batch && remaining.size() > 1;
                        FilesystemNamespaceBatchResult result;
                        try {
                            wait_for_namespace_admission(stop);
                            if (atomic) {
                                if (!batch_journaled) {
                                    journal_namespace_batch(identity.sequence, batch.size());
                                    batch_journaled = true;
                                }
                                result = apply_namespace_backend(remaining, identity, true);
                            } else {
                                result = apply_namespace_backend(remaining);
                            }
                        } catch (const FsError& error) {
                            if (atomic && !retryable_backend_error(error)) {
                                // An op was refused and nothing committed:
                                // publish the head alone to name the culprit,
                                // and requeue the rest in order.
                                {
                                    Lock lock(namespace_queue_mutex);
                                    for (size_t i = batch.size(); i-- > published_prefix + 1;)
                                        namespace_queue.push_front(batch[i]);
                                    namespace_inflight_operations = published_prefix + 1;
                                }
                                batch.resize(published_prefix + 1);
                                identity_batch = false;
                                Log::debug("FUSE namespace batch refused atomically; retrying "
                                           "head op alone seq=" +
                                           std::to_string(batch.back().sequence) +
                                           " error=" + error.what());
                                published_prefix = 0;
                                continue;
                            }
                            if (!published_prefix)
                                throw;
                            prefix_failure_code = error.code();
                            prefix_failure_message = error.what();
                            published_snapshot = std::move(before.snapshot);
                        }
                        if (result.applied) {
                            namespace_publication_batches.fetch_add(1, std::memory_order_relaxed);
                            namespace_operations_batched.fetch_add(result.applied,
                                                                   std::memory_order_relaxed);
                            published_prefix += result.applied;
                            prefix_failure_code = result.failure_code;
                            prefix_failure_message = std::move(result.failure_message);
                            const auto after = fs.local_snapshot_view();
                            published_snapshot = after.snapshot;
                            published_generation = after.generation;
                        }
                    } else {
                        published_snapshot = std::move(before.snapshot);
                    }

                    if (!published_prefix)
                        throw std::logic_error("namespace batch made no progress");
                    for (size_t i = 0; i < published_prefix; ++i)
                        batch[i].published_generation = published_generation;
                    const auto prefix = std::span<const NamespaceOp>(batch).first(published_prefix);
                    for (const auto& op : prefix)
                        namespace_success(op, published_snapshot.get());
                    journal_namespace_published(prefix);
                    namespace_operations_published.fetch_add(published_prefix,
                                                             std::memory_order_relaxed);
                } catch (const std::exception& e) {
                    published_prefix = 0;
                    prefix_failure_code.reset();
                    prefix_failure_message.clear();
                    published_snapshot.reset();
                    ++backend_failures;
                    const bool retryable = retryable_backend_error(e);
                    if (!retryable) {
                        int error = EIO;
                        if (const auto* fs_error = dynamic_cast<const FsError*>(&e))
                            error = fs_error->code();
                        const auto& blocked = batch.front();
                        auto errored = blocked.affected;
                        errored.insert(errored.end(), blocked.removed.begin(),
                                       blocked.removed.end());
                        mark_backend_error(errored, error);
                        {
                            Lock lock(namespace_queue_mutex);
                            if (!namespace_blocked_op ||
                                namespace_blocked_op->sequence != blocked.sequence)
                                namespace_blocked_since = Clock::now();
                            namespace_blocked_op = blocked;
                            namespace_blocked_error_code = error;
                            namespace_blocked_error_message = e.what();
                        }
                        Log::error("FUSE async namespace publication blocked seq=" +
                                   std::to_string(blocked.sequence) + " error=" + e.what());
                    } else {
                        Log::debug("FUSE async namespace publication retry seq=" +
                                   std::to_string(batch.front().sequence) + " error=" + e.what());
                    }
                    // A durable namespace op is never skipped automatically:
                    // there is no conflict resolver, so keep order and retry;
                    // only an operator may abandon it
                    // (skip_blocked_namespace_operation()).
                    //
                    // Discipline 2: cadence is the shared RetryPolicy. A
                    // retryable error past its budget is reported blocked but
                    // keeps retrying at the ceiling, so it can still clear.
                    auto delay = retry.failed(config.namespace_retry);
                    if (!delay) {
                        delay = config.namespace_retry.max_backoff;
                        if (retryable && !budget_reported) {
                            budget_reported = true;
                            const auto& blocked = batch.front();
                            {
                                Lock lock(namespace_queue_mutex);
                                if (!namespace_blocked_op ||
                                    namespace_blocked_op->sequence != blocked.sequence)
                                    namespace_blocked_since = Clock::now();
                                namespace_blocked_op = blocked;
                                namespace_blocked_error_code = EAGAIN;
                                namespace_blocked_error_message =
                                    "retry budget exhausted: " + std::string(e.what());
                            }
                            Log::error("FUSE async namespace publication blocked after " +
                                       std::to_string(retry.total_failures()) +
                                       " retryable failures seq=" +
                                       std::to_string(blocked.sequence) + " error=" + e.what() +
                                       "; still retrying every " +
                                       std::to_string(delay->count()) + " ms");
                        }
                    }
                    Lock wait_lock(namespace_queue_mutex);
                    namespace_cv.wait_for(
                        wait_lock.native(), stop, *delay,
                        [&]() MACHA_REQUIRES(namespace_queue_mutex) {
                            return stopping.load() ||
                                   namespace_skip_requested_sequence == batch.front().sequence;
                        });
                }
            }

            if (published_prefix) {
                const auto prefix = std::span<const NamespaceOp>(batch).first(published_prefix);
                bool confirmed = false;
                if (auto available = fs.available_snapshot_view()) {
                    confirmed =
                        std::all_of(prefix.begin(), prefix.end(), [&](const NamespaceOp& op) {
                            return namespace_op_confirmed(op, *available, fs.namespace_nodes());
                        });
                }
                if (confirmed) {
                    journal_namespace_done(prefix);
                    namespace_operations_confirmed.fetch_add(published_prefix,
                                                             std::memory_order_relaxed);
                    release_namespace_references(prefix);
                } else {
                    Lock lock(namespace_queue_mutex);
                    namespace_unconfirmed.insert(namespace_unconfirmed.end(), prefix.begin(),
                                                 prefix.end());
                }

                if (prefix_failure_code && published_prefix < batch.size()) {
                    ++backend_failures;
                    const auto& blocked = batch[published_prefix];
                    auto errored = blocked.affected;
                    errored.insert(errored.end(), blocked.removed.begin(), blocked.removed.end());
                    mark_backend_error(errored, *prefix_failure_code);
                    Log::error("FUSE async namespace batch committed prefix first_seq=" +
                               std::to_string(batch.front().sequence) +
                               " operations=" + std::to_string(published_prefix) +
                               " blocked_seq=" + std::to_string(blocked.sequence) +
                               " error=" + prefix_failure_message);
                }
            } else if (!stopping.load()) {
                // When stopping, the durable ops are left for startup replay.
                Lock lock(namespace_queue_mutex);
                for (auto i = batch.rbegin(); i != batch.rend(); ++i)
                    namespace_queue.push_front(*i);
            }
            if (published_prefix && published_prefix < batch.size() && !stopping.load()) {
                Lock lock(namespace_queue_mutex);
                for (size_t i = batch.size(); i > published_prefix; --i)
                    namespace_queue.push_front(batch[i - 1]);
            }
            {
                Lock lock(namespace_queue_mutex);
                namespace_inflight = false;
                namespace_inflight_sequence = 0;
                namespace_inflight_operations = 0;
            }
            namespace_cv.notify_all();
        }
    }

    DataSnapshot snapshot_data(const std::shared_ptr<Inode>& inode) {
        Lock lock(inode->mutex);
        DataSnapshot snapshot;
        if (inode->unconfirmed_data_entry)
            return snapshot;
        snapshot.target_sequence = inode->requested_data_sequence;
        // Wait through the latest namespace sequence so the writer never
        // commits through a path a queued rename/unlink has superseded.
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

    bool loader_should_yield() {
        return admission->should_yield(Clock::now());
    }

    void wait_for_loader_admission(std::stop_token stop) {
        for (;;) {
            if (stop.stop_requested() || stopping.load(std::memory_order_relaxed))
                throw FsError(EINTR, "FUSE publication stopping");
            const auto now = Clock::now();
            if (admission->can_start(now)) {
                admission->started(now, true);
                return;
            }
            const auto retry = admission->retry_after(now);
            Lock lock(data_queue_mutex);
            const auto admitted_or_stopping = [&]() MACHA_REQUIRES(data_queue_mutex) {
                return stopping.load(std::memory_order_relaxed) ||
                       admission->can_start(Clock::now());
            };
            if (retry)
                data_cv.wait_for(lock.native(), stop,
                                 std::max(*retry, std::chrono::milliseconds(1)),
                                 admitted_or_stopping);
            else
                data_cv.wait(lock.native(), stop, admitted_or_stopping);
        }
    }

    void wait_for_namespace_admission(std::stop_token stop) {
        Lock lock(namespace_queue_mutex);
        namespace_cv.wait(lock.native(), stop, [&]() MACHA_REQUIRES(namespace_queue_mutex) {
            return stopping.load(std::memory_order_relaxed) ||
                   admission->namespace_can_start(Clock::now());
        });
        if (stop.stop_requested() || stopping.load(std::memory_order_relaxed))
            throw FsError(EINTR, "FUSE publication stopping");
    }

    void finish_loader_admission() {
        admission->finished(Clock::now());
        data_cv.notify_all();
    }

    void abandon_data(const std::shared_ptr<Inode>& inode, std::string_view reason) {
        uint64_t target = 0;
        size_t retired = 0;
        {
            Lock lock(inode->mutex);
            // Wait for pending writes to reach the barrier, then a retry
            // abandons the whole dirty generation.
            if (inode->durability_pending)
                throw FsError(EAGAIN, "FUSE spool corruption raced pending write durability");
            target = inode->durable_data_sequence;
            retired = static_cast<size_t>(
                std::count_if(inode->data_ops.begin(), inode->data_ops.end(),
                              [&](const DataOp& op) { return op.sequence <= target; }));
        }
        if (!retired)
            return;

        // Journaled before reclaiming the spool, so a crash cannot resurrect it.
        const bool journal_idle = journal_data_abandoned(inode->id, target, retired);
        bool spool_clean = true;
        {
            Lock lock(inode->mutex);
            inode->data_ops.erase(
                std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                               [&](const DataOp& op) { return op.sequence <= target; }),
                inode->data_ops.end());
            if (inode->data_ops.empty())
                std::vector<DataOp>{}.swap(inode->data_ops);
            inode->published_data_sequence = std::max(inode->published_data_sequence, target);
            inode->requested_data_sequence = inode->published_data_sequence;
            inode->recovery_data_sequence = inode->published_data_sequence;
            inode->unconfirmed_data_sequence = 0;
            inode->unconfirmed_data_entry.reset();
            // Drop only the dirty overlay; the published generation stands.
            inode->visible.size = inode->base.size;
            inode->visible.mtime_ns = inode->base.mtime_ns;
            inode->visible.ctime_ns = inode->base.ctime_ns;
            rebuild_data_overlay_locked(*inode);
            refresh_retained_owners_locked(*inode);
            if (inode->data_ops.empty())
                spool_clean = retire_spool_locked(*inode);
        }
        if (journal_idle && spool_clean)
            reset_journal_if_idle();
        Log::warn("dropped FUSE spool generation inode=" + std::to_string(inode->id) +
                  " sequence=" + std::to_string(target) + " reason=" + std::string(reason));
        data_cv.notify_all();
    }

    // ENOENT on publication is either a race with a namespace mutation not yet
    // reflected in published_path, or the file is gone cluster-side. Retry
    // while namespace work is moving or the path is in the decoded view;
    // otherwise it is terminal.
    bool publication_path_may_still_appear(const std::shared_ptr<Inode>& inode) {
        {
            Lock queue_lock(namespace_queue_mutex);
            if (namespace_inflight || !namespace_queue.empty() || !namespace_unconfirmed.empty())
                return true;
        }
        std::optional<std::string> path;
        {
            Lock lock(inode->mutex);
            path = inode->published_path;
        }
        if (!path)
            return true; // retired by the next attempt, never terminal.
        if (auto available = fs.available_snapshot_view())
            return snapshot_has_path(*available->snapshot, fs.namespace_nodes(), *path);
        return true;
    }

    bool replay_data_quantum(const std::shared_ptr<Inode>& inode,
                             const std::shared_ptr<DataPublication>& publication) {
        auto& snapshot = publication->snapshot;
        if (snapshot.operations.empty())
            return true;

        if (!publication->initialized && publication->recovered) {
            std::optional<std::string> spool_error;
            {
                Lock lock(inode->mutex);
                spool_error = inode->recovery_spool_error;
            }
            if (spool_error) {
                abandon_data(inode, *spool_error);
                return true;
            }
        }

        if (!publication->initialized) {
            // Wait only for unpublished namespace mutations it depends on.
            {
                Lock namespace_lock(namespace_queue_mutex);
                const auto dependencies_published = [&]() MACHA_REQUIRES(namespace_queue_mutex) {
                    if (stopping.load())
                        return true;
                    if (namespace_inflight &&
                        namespace_inflight_sequence <= snapshot.required_namespace_sequence)
                        return false;
                    return namespace_queue.empty() || namespace_queue.front().sequence >
                                                          snapshot.required_namespace_sequence;
                };
                namespace_cv.wait(namespace_lock.native(), dependencies_published);
            }
            if (stopping.load())
                throw FsError(EINTR, "FUSE publication stopping");

            {
                Lock lock(inode->mutex);
                snapshot.published_path = inode->published_path;
                if (inode->backend_error)
                    throw FsError(*inode->backend_error,
                                  "FUSE inode has asynchronous backend error");
            }
            if (!snapshot.published_path) {
                const auto retired = snapshot.operations.size();
                const bool journal_idle =
                    journal_data_done(inode->id, snapshot.target_sequence, retired);
                bool spool_clean = true;
                {
                    Lock lock(inode->mutex);
                    inode->data_ops.erase(
                        std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                                       [&](const DataOp& op) {
                                           return op.sequence <= snapshot.target_sequence;
                                       }),
                        inode->data_ops.end());
                    if (inode->data_ops.empty())
                        std::vector<DataOp>{}.swap(inode->data_ops);
                    rebuild_data_overlay_locked(*inode);
                    refresh_retained_owners_locked(*inode);
                    inode->published_data_sequence =
                        std::max(inode->published_data_sequence, snapshot.target_sequence);
                    if (inode->data_ops.empty() && !inode->durability_pending)
                        spool_clean = retire_spool_locked(*inode);
                }
                if (journal_idle && spool_clean)
                    reset_journal_if_idle();
                return true;
            }

            for (const auto& op : snapshot.operations) {
                if (op.kind == DataOp::Kind::write)
                    publication->publication_bytes =
                        op.length > std::numeric_limits<uint64_t>::max() -
                                        publication->publication_bytes
                            ? std::numeric_limits<uint64_t>::max()
                            : publication->publication_bytes + op.length;
            }
            note_spool_publication_started();
            data_publications_started.fetch_add(1, std::memory_order_relaxed);
            // The spool+journal is the WAL, so extents are staged provisionally
            // and group-synced once before metadata publication. Recovery
            // replay bypasses the cache so a backlog cannot evict the working set.
            try {
                publication->writer = publication_target.open_publication(
                    *snapshot.published_path,
                    config.write_through_cache && !publication->recovered,
                    config.publication_pipeline_bytes,
                    DataWorkContext(FrameType::loader, config.publication_quantum_bytes, {},
                                    nullptr, &fs.write_progress(),
                                    config.publication_no_progress_deadline));
            } catch (const FsError& e) {
                if (e.code() == ENOENT && publication_path_may_still_appear(inode))
                    throw FsError(EAGAIN, "FUSE namespace advanced before data publication opened");
                throw;
            }
            publication->initialized = true;
        }

        // Writer setup is not charged as loader service; otherwise it could
        // spend the slice and yield with no progress.
        admission->service_started(Clock::now());

        uint64_t served = 0;
        constexpr size_t chunk_size = spool_checksum_chunk_size;
        Bytes buffer(chunk_size);
        try {
            const auto note_pipeline_peak = [&] {
                const auto observed = publication->writer->diagnostics().peak_pending_extent_puts;
                auto peak = data_publication_peak_pipeline_extents.load(std::memory_order_relaxed);
                while (peak < observed &&
                       !data_publication_peak_pipeline_extents.compare_exchange_weak(
                           peak, observed, std::memory_order_relaxed)) {
                }
            };
            const auto yield_quantum = [&] {
                // A quantum holds its byte reservation until its provisional
                // extents retire, bounding loader I/O ahead of viewer demand.
                publication->writer->drain_staging();
                note_spool_publication_progress(publication->unreported_spool_progress);
                publication->unreported_spool_progress = 0;
                note_pipeline_peak();
                return false;
            };
            while (publication->operation_index < snapshot.operations.size()) {
                if (stopping.load())
                    throw FsError(EINTR, "FUSE publication stopping");
                if (loader_should_yield()) {
                    return yield_quantum();
                }
                const auto& op = snapshot.operations[publication->operation_index];
                if (op.kind == DataOp::Kind::truncate) {
                    // Charged one chunk so truncate streams respect the quantum.
                    if (served > config.publication_quantum_bytes - chunk_size) {
                        return yield_quantum();
                    }
                    publication->writer->truncate(op.size);
                    served += chunk_size;
                    ++publication->operation_index;
                    publication->operation_offset = 0;
                    continue;
                }
                while (publication->operation_offset < op.length &&
                       served < config.publication_quantum_bytes) {
                    // Input is already durable, so yielding between chunks is
                    // safe.
                    if (loader_should_yield()) {
                        return yield_quantum();
                    }
                    const auto write_offset = op.offset + publication->operation_offset;
                    const auto remaining_quantum = config.publication_quantum_bytes - served;
                    // An overlapping write may first need the old generation
                    // staged; do that under the same grant, keeping both
                    // cursors across a yield.
                    if (remaining_quantum < fs.extent_size())
                        return yield_quantum();
                    const auto preparation = publication->writer->prepare_write(
                        write_offset, remaining_quantum);
                    served += preparation.bytes_processed;
                    if (!preparation.ready || served >= config.publication_quantum_bytes)
                        return yield_quantum();
                    const auto chunk =
                        static_cast<size_t>(std::min<uint64_t>(
                            {buffer.size(), op.length - publication->operation_offset,
                             config.publication_quantum_bytes - served}));
                    if (publication->replay_spool.get() < 0) {
                        const int fd = ::open(snapshot.spool_path.c_str(), O_RDONLY);
                        if (fd < 0)
                            throw FsError(errno, "cannot open FUSE write spool for publication");
                        publication->replay_spool.reset(fd);
                    }
                    if (pread_exact(publication->replay_spool.get(), {buffer.data(), chunk},
                                    op.spool_offset + publication->operation_offset) != chunk) {
                        if (publication->recovered) {
                            abandon_data(inode, "short read from FUSE write spool");
                            return true;
                        }
                        throw FsError(EIO, "short read from FUSE write spool");
                    }
                    data_publication_bytes_read.fetch_add(chunk, std::memory_order_relaxed);
                    publication->spool_bytes_read += chunk;
                    if (!op.spool_hashes.empty()) {
                        const auto checksum_index =
                            static_cast<size_t>(publication->operation_offset /
                                                State::spool_checksum_chunk_size);
                        if (checksum_index >= op.spool_hashes.size() ||
                            sha256(std::span<const uint8_t>{buffer.data(), chunk}) !=
                                op.spool_hashes[checksum_index]) {
                            abandon_data(inode, "payload checksum mismatch");
                            return true;
                        }
                    }
                    if (publication->writer->write(write_offset,
                                                   {buffer.data(), chunk}) != chunk)
                        throw FsError(EIO, "short replay into Macha write handle");
                    publication->operation_offset += chunk;
                    publication->unreported_spool_progress += chunk;
                    served += chunk;
                }
                if (publication->operation_offset == op.length) {
                    ++publication->operation_index;
                    publication->operation_offset = 0;
                }
                if (served >= config.publication_quantum_bytes &&
                    publication->operation_index < snapshot.operations.size()) {
                    return yield_quantum();
                }
            }
            if (loader_should_yield())
                return yield_quantum();
            // Commit preparation starts on a fresh grant so arbitrary write
            // lengths do not fragment the rebuilt manifest.
            if (served)
                return yield_quantum();
            const auto remaining_quantum = config.publication_quantum_bytes - served;
            if (remaining_quantum < fs.extent_size())
                return yield_quantum();
            const auto commit_preparation =
                publication->writer->prepare_commit(remaining_quantum);
            served += commit_preparation.bytes_processed;
            if (!commit_preparation.ready || served >= config.publication_quantum_bytes)
                return yield_quantum();
            {
                // Publish the visible mtime, including any later utimens.
                Lock lock(inode->mutex);
                publication->writer->set_committed_mtime(inode->visible.mtime_ns);
            }
            publication->writer->commit();
            const auto completed_diagnostics = publication->writer->diagnostics();
            note_spool_publication_progress(publication->unreported_spool_progress);
            publication->unreported_spool_progress = 0;
            note_pipeline_peak();
            data_publications_completed.fetch_add(1, std::memory_order_relaxed);
            data_publication_bytes_committed.fetch_add(publication->publication_bytes,
                                                       std::memory_order_relaxed);
            data_publication_completed_spool_bytes_read.fetch_add(
                publication->spool_bytes_read, std::memory_order_relaxed);
            data_publication_completed_source_bytes_read.fetch_add(
                completed_diagnostics.materialize_source_bytes +
                    completed_diagnostics.rebuild_source_bytes,
                std::memory_order_relaxed);
            data_publication_completed_reused_extents.fetch_add(
                completed_diagnostics.rebuild_reused_extents, std::memory_order_relaxed);
            data_publication_completed_put_extents.fetch_add(
                completed_diagnostics.new_extent_puts + completed_diagnostics.rebuild_put_extents,
                std::memory_order_relaxed);
        } catch (const FsError& e) {
            // Only ENOENT is re-derived; other errors keep their own message.
            if (e.code() == ENOENT && publication_path_may_still_appear(inode))
                throw FsError(EAGAIN, "FUSE namespace advanced during data publication");
            throw;
        }
        auto committed = publication->writer->committed_entry();

        // Journaled before the in-memory watermark moves; a crash before it
        // replays idempotently.
        journal_data_published(inode->id, snapshot.target_sequence, committed);
        {
            Lock lock(inode->mutex);
            inode->base = committed;
            inode->published_data_sequence =
                std::max(inode->published_data_sequence, snapshot.target_sequence);
            inode->unconfirmed_data_sequence = snapshot.target_sequence;
            inode->unconfirmed_publication_bytes = publication->publication_bytes;
            inode->unconfirmed_data_entry = committed;
        }

        // The overlay retires only once the decoded view shows the commit; if
        // it lags, a later request confirms.
        if (auto available = fs.available_snapshot_view())
            (void)confirm_data_from_snapshot(inode, *available->snapshot);
        return true;
    }

    bool data_global_slot_available() MACHA_REQUIRES(data_queue_mutex) {
        // Viewers get the dominant share, not exclusion: loader bursts run
        // after a proportional cooldown and take all capacity when idle.
        if (!admission->can_start(Clock::now()))
            return false;
        return active_data.load(std::memory_order_relaxed) < config.commit_workers &&
               publication_inflight_bytes <=
                   config.publication_inflight_bytes - config.publication_quantum_bytes;
    }

    static bool admissible(bool bounded, const Inode& inode) MACHA_REQUIRES(inode.mutex) {
        return !bounded || inode.data_publication != nullptr;
    }

    auto runnable_data_locked() MACHA_REQUIRES(data_queue_mutex) {
        if (!data_global_slot_available())
            return data_queue.end();

        // At the open-writer cap only inodes already holding a writer are
        // admissible: they finish on their existing leases. Depth-first here
        // means a publication waiting on retained memory waits on control or
        // viewer work, never on another publication.
        const bool bounded = writer_cap_reached();

        // Closed files first, so a large open import cannot hide complete
        // files; recovery provenance does not demote them. Handle state is
        // read now, since release() may close an already-queued inode.
        auto loader = std::find_if(data_queue.begin(), data_queue.end(), [&](const DataQueueItem& item) {
            Lock inode_lock(item.inode->mutex);
            return item.inode->writable_handles == 0 && admissible(bounded, *item.inode);
        });
        if (loader != data_queue.end() && spool_under_pressure()) {
            struct RetirementScore {
                uint64_t retirement_bytes{};
                uint64_t remaining_bytes{1};
            };
            const auto score = [](const DataQueueItem& item) {
                const auto& inode = *item.inode;
                Lock inode_lock(inode.mutex);
                uint64_t target_sequence = inode.requested_data_sequence;
                uint64_t total_bytes = 0;
                uint64_t processed_bytes = 0;
                if (inode.data_publication) {
                    target_sequence = inode.data_publication->snapshot.target_sequence;
                    total_bytes = inode.data_publication->publication_bytes;
                    processed_bytes = inode.data_publication->spool_bytes_read;
                }
                if (!total_bytes) {
                    for (const auto& op : inode.data_ops) {
                        if (op.kind != DataOp::Kind::write ||
                            op.sequence <= inode.published_data_sequence ||
                            op.sequence > target_sequence)
                            continue;
                        total_bytes =
                            op.length > std::numeric_limits<uint64_t>::max() - total_bytes
                                ? std::numeric_limits<uint64_t>::max()
                                : total_bytes + op.length;
                    }
                }
                const bool later_generation =
                    inode.durability_pending ||
                    std::any_of(inode.data_ops.begin(), inode.data_ops.end(),
                                [&](const DataOp& op) { return op.sequence > target_sequence; });
                return RetirementScore{
                    later_generation ? 0 : inode.spool_end,
                    std::max<uint64_t>(1, total_bytes > processed_bytes
                                              ? total_bytes - processed_bytes
                                              : 1)};
            };
            const auto better = [](const RetirementScore& candidate,
                                   const RetirementScore& current) {
                const auto candidate_return =
                    static_cast<long double>(candidate.retirement_bytes) *
                    static_cast<long double>(current.remaining_bytes);
                const auto current_return =
                    static_cast<long double>(current.retirement_bytes) *
                    static_cast<long double>(candidate.remaining_bytes);
                if (candidate_return != current_return)
                    return candidate_return > current_return;
                if (candidate.remaining_bytes != current.remaining_bytes)
                    return candidate.remaining_bytes < current.remaining_bytes;
                return candidate.retirement_bytes > current.retirement_bytes;
            };

            auto selected = loader;
            auto selected_score = score(*selected);
            for (auto candidate = std::next(loader); candidate != data_queue.end(); ++candidate) {
                RetirementScore candidate_score;
                {
                    Lock inode_lock(candidate->inode->mutex);
                    if (candidate->inode->writable_handles != 0 ||
                        !admissible(bounded, *candidate->inode))
                        continue;
                }
                candidate_score = score(*candidate);
                if (better(candidate_score, selected_score)) {
                    selected = candidate;
                    selected_score = candidate_score;
                }
            }
            if (selected != loader)
                loader = selected;
        }
        if (loader != data_queue.end())
            return loader;

        // Then files still open for writing.
        if (!bounded)
            return data_queue.begin();
        return std::find_if(data_queue.begin(), data_queue.end(),
                            [](const DataQueueItem& item) {
                                Lock inode_lock(item.inode->mutex);
                                return item.inode->data_publication != nullptr;
                            });
    }

    bool runnable_data_available_locked() MACHA_REQUIRES(data_queue_mutex) {
        return runnable_data_locked() != data_queue.end();
    }

    void data_loop(std::stop_token stop) {
        while (!stop.stop_requested() && !stopping.load()) {
            std::shared_ptr<Inode> inode;
            bool recovered = false;
            // Held until this turn opens the publication or ends.
            bool reserved = false;
            {
                Lock lock(data_queue_mutex);
                while (!stop.stop_requested() && !stopping.load()) {
                    // Backed-off inodes are off the queue; nothing notifies
                    // their due time.
                    const auto due_ns = deferred_retry_due_ns.load(std::memory_order_acquire);
                    std::optional<Clock::time_point> due;
                    if (due_ns)
                        due = Clock::time_point{std::chrono::nanoseconds(due_ns)};
                    if (due && Clock::now() >= *due) {
                        lock.unlock();
                        deferred_retry_due_ns.store(0, std::memory_order_release);
                        admit_deferred();
                        lock.lock();
                        continue;
                    }
                    if (data_queue.empty()) {
                        const auto arrived = [&]() MACHA_REQUIRES(data_queue_mutex) {
                            return stopping.load() || !data_queue.empty() ||
                                   deferred_retry_due_ns.load(std::memory_order_acquire) != due_ns;
                        };
                        if (due) {
                            data_cv.wait_until(lock.native(), stop, *due, arrived);
                        } else if (parked_publications.load(std::memory_order_relaxed)) {
                            // Nothing notifies a membership or storage change.
                            data_cv.wait_for(lock.native(), stop, config.parked_recheck,
                                             arrived);
                            lock.unlock();
                            readmit_parked_on_change();
                            lock.lock();
                        } else {
                            data_cv.wait(lock.native(), stop, arrived);
                        }
                        continue;
                    }
                    if (runnable_data_available_locked())
                        break;

                    // Sleep to the earliest of cooldown expiry, viewer-idle
                    // expiry and the deferred due time; other events notify.
                    // The due time matters with a non-empty queue too: at the
                    // writer cap every queued inode may be inadmissible while
                    // the writers that could free a slot are backed off.
                    auto wake_at = due;
                    const auto now = Clock::now();
                    if (const auto retry = admission->retry_after(now);
                        retry && *retry > std::chrono::milliseconds(0))
                        wake_at = wake_at ? std::min(*wake_at, now + *retry) : now + *retry;
                    const auto changed = [&]() MACHA_REQUIRES(data_queue_mutex) {
                        return stopping.load() || data_queue.empty() ||
                               runnable_data_available_locked() ||
                               deferred_retry_due_ns.load(std::memory_order_acquire) != due_ns;
                    };
                    if (wake_at)
                        data_cv.wait_until(lock.native(), stop, *wake_at, changed);
                    else
                        data_cv.wait(lock.native(), stop, changed);
                }
                if (stop.stop_requested() || stopping.load())
                    break;
                auto selected = runnable_data_locked();
                if (selected == data_queue.end())
                    continue;
                bool selected_closed_ahead_of_open = false;
                bool selected_retirement_ahead_of_closed = false;
                {
                    Lock selected_inode_lock(selected->inode->mutex);
                    if (selected->inode->writable_handles == 0) {
                        selected_closed_ahead_of_open =
                            std::any_of(data_queue.begin(), selected,
                                        [](const DataQueueItem& item) {
                                            Lock inode_lock(item.inode->mutex);
                                            return item.inode->writable_handles > 0;
                                        });
                        if (spool_under_pressure()) {
                            const auto first_closed = std::find_if(
                                data_queue.begin(), selected,
                                [](const DataQueueItem& item) {
                                    Lock inode_lock(item.inode->mutex);
                                    return item.inode->writable_handles == 0;
                                });
                            selected_retirement_ahead_of_closed =
                                first_closed != selected;
                        }
                    }
                }
                if (selected_closed_ahead_of_open)
                    data_closed_priority_selections.fetch_add(1, std::memory_order_relaxed);
                if (selected_retirement_ahead_of_closed)
                    data_retirement_priority_selections.fetch_add(1,
                                                                   std::memory_order_relaxed);
                if (writer_cap_reached())
                    data_publication_selections_under_writer_cap.fetch_add(
                        1, std::memory_order_relaxed);
                inode = selected->inode;
                recovered = selected->recovered;
                {
                    Lock inode_lock(inode->mutex);
                    reserved = inode->data_publication == nullptr;
                }
                if (reserved)
                    ++reserved_publications;
                data_queue.erase(selected);
                admission->started(Clock::now(), false);
                publication_inflight_bytes += config.publication_quantum_bytes;
                publication_inflight_bytes_diagnostic.store(publication_inflight_bytes,
                                                             std::memory_order_relaxed);
                data_publication_quanta.fetch_add(1, std::memory_order_relaxed);
                auto byte_peak =
                    data_publication_peak_inflight_bytes.load(std::memory_order_relaxed);
                while (byte_peak < publication_inflight_bytes &&
                       !data_publication_peak_inflight_bytes.compare_exchange_weak(
                           byte_peak, publication_inflight_bytes, std::memory_order_relaxed)) {
                }
                const auto active_now = active_data.fetch_add(1, std::memory_order_relaxed) + 1;
                auto peak = data_publication_peak_active.load(std::memory_order_relaxed);
                while (peak < active_now &&
                       !data_publication_peak_active.compare_exchange_weak(
                           peak, active_now, std::memory_order_relaxed)) {
                }
                if (recovered)
                    ++active_recovery_data;
            }
            {
                Lock lock(inode->mutex);
                inode->data_queued = false;
                inode->data_running = true;
            }

            bool retry = false;
            bool completed = false;
            try {
                std::shared_ptr<DataPublication> publication;
                {
                    Lock lock(inode->mutex);
                    publication = inode->data_publication;
                }
                if (!publication) {
                    auto created = std::make_shared<DataPublication>();
                    created->snapshot = snapshot_data(inode);
                    created->recovered = recovered;
                    {
                        Lock lock(inode->mutex);
                        set_data_publication_locked(*inode, created);
                        refresh_retained_owners_locked(*inode);
                    }
                    if (reserved) {
                        Lock queue_lock(data_queue_mutex);
                        --reserved_publications;
                        reserved = false;
                    }
                    publication = std::move(created);
                }
                completed = replay_data_quantum(inode, publication);
                if (!completed)
                    data_publication_yields.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                ++backend_failures;
                // ESTALE: the backend lost an extent the writer cannot re-put;
                // drop the writer so the next attempt replays from the spool.
                const auto* fs_error = dynamic_cast<const FsError*>(&e);
                const int code = fs_error ? fs_error->code() : EIO;
                const bool replay = fs_error && code == ESTALE;
                retry = replay || retryable_backend_error(e);
                // Discipline 3: a terminal ENOENT means the bytes have no
                // destination; abandon them as for a corrupt spool rather than
                // poison the inode and keep the journal from resetting.
                bool abandoned = false;
                if (!retry && fs_error && code == ENOENT) {
                    try {
                        std::string path;
                        uint64_t bytes = 0;
                        {
                            Lock lock(inode->mutex);
                            path = inode->published_path.value_or(inode->current_path);
                            for (const auto& op : inode->data_ops)
                                if (op.kind == DataOp::Kind::write)
                                    bytes += op.length;
                        }
                        abandon_data(inode, "file is no longer in the namespace");
                        abandoned = true;
                        publications_abandoned.fetch_add(1, std::memory_order_relaxed);
                        Log::warn("FUSE data publication abandoned inode=" +
                                  std::to_string(inode->id) + " last_path=" + path +
                                  " unpublished_bytes=" + std::to_string(bytes) +
                                  " reason=file is no longer in the namespace");
                    } catch (const FsError&) {
                        // Writes still reaching the spool; retry later.
                        retry = true;
                    }
                }
                const auto line = std::string("FUSE async data publication ") +
                                  (replay ? "replay" : retry ? "retry" : "failed") +
                                  " inode=" + std::to_string(inode->id) + " error=" + e.what();
                if (replay) {
                    Lock lock(inode->mutex);
                    set_data_publication_locked(*inode, nullptr);
                }
                if (abandoned) {
                } else if (retry) {
                    // Discipline 2: per-inode backoff and budget; past the
                    // budget the file is parked for an operator.
                    std::optional<std::chrono::milliseconds> delay;
                    std::string path;
                    size_t attempts = 0;
                    size_t consecutive = 0;
                    std::chrono::milliseconds failing_for{};
                    std::chrono::milliseconds run_for{};
                    {
                        Lock lock(inode->mutex);
                        const auto now = Clock::now();
                        delay = inode->publication_retry.failed(config.publication_retry, now);
                        attempts = inode->publication_retry.total_failures();
                        consecutive = inode->publication_retry.consecutive_failures();
                        failing_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - inode->publication_retry.first_failure());
                        run_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - inode->publication_retry.failing_since());
                        path = inode->current_path;
                        if (!delay) {
                            inode->parked = Inode::Parked{
                                code, e.what(), now, publication_target.reachability_epoch()};
                            set_data_publication_locked(*inode, nullptr);
                        }
                    }
                    if (delay) {
                        publication_retries_backed_off.fetch_add(1, std::memory_order_relaxed);
                        note_deferred_due(Clock::now() + *delay);
                        // Warn at the threshold, then periodically.
                        constexpr size_t escalate_at = 10;
                        constexpr size_t repeat_every = 20;
                        const bool crossed = consecutive == escalate_at;
                        if (crossed)
                            publications_retrying_persistently.fetch_add(
                                1, std::memory_order_relaxed);
                        if (crossed || (consecutive > escalate_at &&
                                        (consecutive - escalate_at) % repeat_every == 0)) {
                            Log::warn("FUSE data publication failing repeatedly inode=" +
                                      std::to_string(inode->id) + " path=" + path +
                                      " consecutive=" + std::to_string(consecutive) +
                                      " failing_for_ms=" + std::to_string(run_for.count()) +
                                      " retry_in_ms=" + std::to_string(delay->count()) +
                                      " error=" + e.what());
                        } else {
                            Log::debug(line + " retry_in_ms=" + std::to_string(delay->count()) +
                                       " attempts=" + std::to_string(attempts));
                        }
                    } else {
                        parked_publications.fetch_add(1, std::memory_order_relaxed);
                        retry = false; // not readmitted; not poisoned either.
                        Log::warn("FUSE data publication parked inode=" +
                                  std::to_string(inode->id) + " path=" + path +
                                  " attempts=" + std::to_string(attempts) +
                                  " failing_for_ms=" + std::to_string(failing_for.count()) +
                                  " error=" + e.what() +
                                  "; retried when membership or storage changes, or via "
                                  "manage/filesystem/parked-publications");
                    }
                } else {
                    // Terminal: poisons the inode until an operator acts.
                    Log::warn(line);
                }
                bool parked_now = false;
                {
                    Lock lock(inode->mutex);
                    parked_now = inode->parked.has_value();
                }
                if (!retry && !parked_now && !abandoned) {
                    int code = EIO;
                    if (const auto* fs_error = dynamic_cast<const FsError*>(&e))
                        code = fs_error->code();
                    Lock lock(inode->mutex);
                    inode->backend_error = code;
                }
                if (abandoned)
                    completed = true; // the generation is gone; nothing to run.
            }

            {
                Lock lock(inode->mutex);
                inode->data_running = false;
                // Yields and retryable failures keep the writer and cursor (a
                // failed extent stays at the queue head, so retry leaves no
                // hole); completion and terminal errors discard them.
                if (completed)
                    inode->publication_retry.succeeded();
                if (completed || inode->backend_error || inode->parked)
                    set_data_publication_locked(*inode, nullptr);
                refresh_retained_owners_locked(*inode);
                const bool still_requested =
                    inode->requested_data_sequence > inode->published_data_sequence;
                if (inode->backend_error || inode->parked) {
                    // Not runnable; admit_deferred() must not requeue it.
                    inode->data_deferred = false;
                } else if ((!completed || retry || still_requested) &&
                           !inode->unconfirmed_data_entry) {
                    inode->data_deferred = true;
                }
            }
            {
                Lock lock(data_queue_mutex);
                if (reserved)
                    --reserved_publications;
                publication_inflight_bytes -= config.publication_quantum_bytes;
                publication_inflight_bytes_diagnostic.store(publication_inflight_bytes,
                                                             std::memory_order_relaxed);
                (void)active_data.fetch_sub(1, std::memory_order_relaxed);
                if (recovered)
                    --active_recovery_data;
                admission->finished(Clock::now());
            }
            data_cv.notify_all();
            admit_deferred();
            reclaim_inode_if_quiescent(inode->id);
        }
    }

    void broker_loop(size_t index, std::stop_token stop) {
        auto& queue = broker[index];
        while (!stop.stop_requested() && !stopping.load()) {
            BrokerTask task;
            {
                Lock lock(queue.mutex);
                queue.cv.wait(lock.native(), stop, [&]() MACHA_REQUIRES(queue.mutex) {
                    return !queue.tasks.empty() || stopping.load();
                });
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
                auto published =
                    transformed_path(result.published.value_or(std::string{}), inode, op);
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
        return static_cast<size_t>(
            std::count_if(found->second.begin(), found->second.end(), [&](const DataOp& op) {
                return op.sequence > completed && op.sequence <= through;
            }));
    }

    void reconcile_recovery(JournalRecovery& recovery, const MetadataSnapshot& snapshot) {
        // A "published" marker means the backend made the mutation durable at
        // the write floor, which includes this node's replica; so retire it
        // even if a later change has overwritten its effect. Leaving it
        // unconfirmed would make refresh_namespace_if_stale() refuse every
        // newer view.
        std::vector<NamespaceOp> confirmed_namespace;
        for (const auto& [sequence, op] : recovery.namespace_ops) {
            if (recovery.namespace_done.contains(sequence) ||
                !recovery.namespace_published.contains(sequence))
                continue;
            if (!namespace_effect_confirmed(op, snapshot, fs.namespace_nodes()))
                Log::info("FUSE recovery retiring published namespace op whose effect is no "
                          "longer visible seq=" + std::to_string(op.sequence) +
                          " kind=" + std::string(namespace_op_kind_name(op.kind)) +
                          " from=" + op.from + (op.to.empty() ? "" : " to=" + op.to));
            confirmed_namespace.push_back(op);
        }
        if (!confirmed_namespace.empty()) {
            journal_namespace_done(confirmed_namespace);
            for (const auto& op : confirmed_namespace)
                recovery.namespace_done.insert(op.sequence);
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
            auto entry = snapshot_entry(snapshot, fs.namespace_nodes(), *paths.published);
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
                                                 const JournalRecovery& recovery)
        MACHA_REQUIRES(inode.mutex) {
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
                if (op.set_uid)
                    inode.visible.uid = op.uid;
                if (op.set_gid)
                    inode.visible.gid = op.gid;
                inode.visible.ctime_ns = op.ctime_ns;
                ++inode.visible.version;
                break;
            case NamespaceOp::Kind::utimens:
                // Data ops were applied first; utimens wins only if admitted
                // after the last write.
                if (op.ctime_ns >= inode.visible.ctime_ns) {
                    inode.visible.mtime_ns = op.mtime_ns;
                    inode.visible.ctime_ns = op.ctime_ns;
                    ++inode.visible.version;
                }
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

    static void apply_pending_data_metadata(Inode& inode, const std::vector<DataOp>& operations)
        MACHA_REQUIRES(inode.mutex) {
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
        const auto orphan_bytes = size - keep;
        const auto existing_orphans = make_orphan_room(orphan_bytes);
        if (orphan_bytes > config.max_orphan_bytes ||
            existing_orphans > config.max_orphan_bytes - orphan_bytes) {
            ScopedFd original(::open(path.c_str(), O_RDWR));
            if (original.get() < 0)
                throw FsError(errno, "cannot trim over-budget FUSE orphan tail");
            if (::ftruncate(original.get(), static_cast<off_t>(keep)) != 0)
                throw FsError(errno, "cannot trim over-budget FUSE orphan tail");
            fsync_fd(original.get(), "cannot sync trimmed over-budget FUSE spool");
            sync_directory(spool_dir);
            Log::warn("discarded over-budget orphaned FUSE spool tail path=" + path.string() +
                      " bytes=" + std::to_string(orphan_bytes));
            return;
        }
        const auto orphan = path.string() + ".orphan." + std::to_string(unix_ms());
        ScopedFd source(::open(path.c_str(), O_RDONLY));
        if (source.get() < 0)
            throw FsError(errno, "cannot open FUSE spool for orphan recovery");
        ScopedFd target(::open(orphan.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
        if (target.get() < 0)
            throw FsError(errno, "cannot preserve orphaned FUSE spool bytes");
        try {
            Bytes buffer(256 * 1024);
            uint64_t offset = keep;
            while (offset < size) {
                const auto chunk =
                    static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - offset));
                if (pread_exact(source.get(), {buffer.data(), chunk}, offset) != chunk)
                    throw FsError(EIO, "cannot read orphaned FUSE spool bytes");
                write_exact(target.get(), {buffer.data(), chunk});
                offset += chunk;
            }
            fsync_fd(target.get(), "cannot sync preserved orphaned FUSE spool bytes");
            source.reset();
            if (::close(target.release()) != 0)
                throw FsError(errno, "cannot close preserved orphaned FUSE spool bytes");
            ScopedFd original(::open(path.c_str(), O_RDWR));
            if (original.get() < 0)
                throw FsError(errno, "cannot trim FUSE spool after orphan preservation");
            if (::ftruncate(original.get(), static_cast<off_t>(keep)) != 0)
                throw FsError(errno, "cannot trim FUSE spool after orphan preservation");
            fsync_fd(original.get(), "cannot sync trimmed FUSE spool");
            sync_directory(spool_dir);
            Log::warn("preserved orphaned FUSE spool tail path=" + path.string() +
                      " bytes=" + std::to_string(size - keep) + " as=" + orphan);
        } catch (...) {
            throw;
        }
    }

    void recover_spool(const std::shared_ptr<Inode>& inode, const std::vector<DataOp>& operations)
        MACHA_REQUIRES(inode->mutex) {
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
        if (fd < 0) {
            if (errno == ENOENT) {
                inode->recovery_spool_error = "durable journal references missing spool";
                inode->spool_end = 0;
                return;
            }
            throw FsError(errno, "cannot open recovered FUSE spool");
        }
        struct stat statbuf{};
        if (::fstat(fd, &statbuf) != 0) {
            const int saved = errno;
            ::close(fd);
            throw FsError(saved, "cannot stat recovered FUSE spool");
        }
        const auto size = static_cast<uint64_t>(statbuf.st_size);
        if (size < required) {
            ::close(fd);
            recover_spool_bytes(size);
            inode->recovery_spool_error = "spool is shorter than its durable operation journal";
            inode->spool_end = size;
            return;
        }
        ::close(fd);
        if (size > required)
            quarantine_spool_tail(inode->spool_path, required, size);
        recover_spool_bytes(required);
        // No fd retained; replay and reads open the spool lazily.
        inode->spool_fd = -1;
        inode->spool_end = required;
    }

    void preserve_unreferenced_spool(const std::filesystem::path& path, uint64_t size,
                                     std::string_view reason) {
        const auto existing_orphans = make_orphan_room(size);
        if (size > config.max_orphan_bytes || existing_orphans > config.max_orphan_bytes - size) {
            if (::unlink(path.c_str()) != 0 && errno != ENOENT)
                throw FsError(errno, "cannot discard over-budget unreferenced FUSE spool");
            sync_directory(spool_dir);
            Log::warn("discarded over-budget unreferenced FUSE spool path=" + path.string() +
                      " bytes=" + std::to_string(size) + " reason=" + std::string(reason));
            return;
        }
        const auto preserved =
            path.string() + ".orphan." + std::to_string(unix_ms()) + "." + std::to_string(getpid());
        if (::rename(path.c_str(), preserved.c_str()) != 0)
            throw FsError(errno, "cannot preserve unreferenced FUSE spool");
        sync_directory(spool_dir);
        Log::warn("preserved unreferenced FUSE spool path=" + path.string() + " bytes=" +
                  std::to_string(size) + " as=" + preserved + " reason=" + std::string(reason) +
                  "; no durable journal attribution exists, so bytes were not replayed");
    }

    void validate_recovery_spools(const JournalRecovery& recovery) {
        (void)make_orphan_room(0);
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
                // Retired by data_done; its unlink was lost in a crash.
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

    // Bounded and cancellable: it runs in the constructor on a supervised
    // lifecycle thread and must not hold Service::stop(). Both exits throw, so
    // the supervisor treats them as a construction fault (backoff, then
    // `disabled`).
    MetadataSnapshotView wait_for_initial_namespace() {
        bool announced = false;
        const auto started = Clock::now();
        for (;;) {
            try {
                return fs.local_snapshot_view();
            } catch (const MetadataNotReady& error) {
                if (startup_stop.stop_requested())
                    throw FsError(EINTR,
                                  "FUSE startup cancelled while waiting for initial metadata");
                if (config.initial_namespace_timeout.count() > 0 &&
                    Clock::now() - started >= config.initial_namespace_timeout)
                    throw FsError(ETIMEDOUT,
                                  "FUSE saw no metadata within " +
                                      std::to_string(config.initial_namespace_timeout.count()) +
                                      "ms: " + std::string(error.what()));
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

        // Discipline 3: ops on an undescribed inode cannot replay. Retire them
        // with journaled markers and count them; validate_recovery_spools()
        // keeps any spool bytes as an orphan.
        {
            std::vector<uint64_t> undescribed;
            for (auto id : needed)
                if (!recovery.inodes.contains(id))
                    undescribed.push_back(id);
            for (auto id : undescribed) {
                size_t dropped = 0;
                std::vector<NamespaceOp> namespace_dropped;
                for (auto& [sequence, op] : recovery.namespace_ops) {
                    if (recovery.namespace_done.contains(sequence))
                        continue;
                    const bool references =
                        std::find(op.affected.begin(), op.affected.end(), id) !=
                            op.affected.end() ||
                        std::find(op.removed.begin(), op.removed.end(), id) != op.removed.end();
                    if (!references)
                        continue;
                    namespace_dropped.push_back(op);
                    recovery.namespace_done.insert(sequence);
                }
                if (!namespace_dropped.empty()) {
                    journal_namespace_done(namespace_dropped);
                    dropped += namespace_dropped.size();
                }
                auto operations = recovery.data_ops.find(id);
                if (operations != recovery.data_ops.end()) {
                    const auto done = recovery.data_done[id];
                    const auto pending = static_cast<size_t>(std::count_if(
                        operations->second.begin(), operations->second.end(),
                        [&](const DataOp& op) { return op.sequence > done; }));
                    if (pending) {
                        (void)journal_data_abandoned(id, operations->second.back().sequence,
                                                     pending);
                        dropped += pending;
                    }
                    recovery.data_ops.erase(operations);
                    recovery.data_done.erase(id);
                    recovery.data_history_inodes.erase(id);
                }
                needed.erase(id);
                recovery_dropped_operations.fetch_add(dropped, std::memory_order_relaxed);
                Log::warn("FUSE journal recovery dropped operations for an inode without a "
                          "descriptor inode=" +
                          std::to_string(id) + " operations=" + std::to_string(dropped) +
                          "; any spool bytes are preserved as an orphan");
            }
            // Dropped ops may have been all that named other inodes.
            if (!undescribed.empty()) {
                std::set<uint64_t> still_needed;
                for (const auto& [sequence, op] : recovery.namespace_ops) {
                    if (recovery.namespace_done.contains(sequence))
                        continue;
                    still_needed.insert(op.affected.begin(), op.affected.end());
                    still_needed.insert(op.removed.begin(), op.removed.end());
                }
                for (const auto& [inode, operations] : recovery.data_ops) {
                    const auto done = recovery.data_done[inode];
                    if (std::any_of(operations.begin(), operations.end(),
                                    [&](const DataOp& op) { return op.sequence > done; }))
                        still_needed.insert(inode);
                }
                needed = std::move(still_needed);
                validate_recovery_spools(recovery);
            }
        }

        std::map<std::string, uint64_t, std::less<>> descriptor_by_published_path;
        std::map<uint64_t, RecoveryPaths> recovered_paths;
        for (auto id : needed) {
            auto descriptor = recovery.inodes.find(id);
            if (descriptor == recovery.inodes.end())
                throw std::runtime_error(
                    "FUSE operation journal references an inode without descriptor");
            auto state = recovery_paths(id, descriptor->second, recovery);
            if (state.published)
                descriptor_by_published_path[canonical_path(*state.published)] = id;
            recovered_paths[id] = std::move(state);
        }

        auto recovery_nodes = fs.namespace_nodes();
        Lock lock(namespace_mutex);
        next_inode = std::max<uint64_t>(2, recovery.max_inode + 1);
        next_namespace_sequence = std::max<uint64_t>(1, recovery.max_namespace_sequence + 1);
        // With extents: the base entry serves reads and writes.
        for_each_namespace_entry(snapshot, &recovery_nodes,
                                 [&](const std::string& path, const FsEntry& entry)
                                     MACHA_REQUIRES(namespace_mutex) {
            const auto key = canonical_path(path);
            if (snapshot_path_shadowed(key, recovery))
                return;
            auto mapped = descriptor_by_published_path.find(key);
            if (mapped != descriptor_by_published_path.end())
                return;
            auto inode = std::make_shared<Inode>();
            Lock inode_lock(inode->mutex);
            inode->id = path == "/" ? 1 : next_inode++;
            inode->base = entry;
            inode->visible = entry;
            inode->current_path = path;
            inode->published_path = path;
            paths[key] = inode;
            inodes[inode->id] = std::move(inode);
            note_inode_inserted_locked();
        });

        for (auto id : needed) {
            const auto& descriptor = recovery.inodes.at(id);
            const auto& rpaths = recovered_paths.at(id);
            auto inode = std::make_shared<Inode>();
            std::string current_path;
            uint64_t namespace_sequence = 0;
            {
                Lock inode_lock(inode->mutex);
                inode->id = id;
                inode->journal_epoch = journal_epoch.load(std::memory_order_relaxed);
                inode->namespace_sequence = rpaths.namespace_sequence;
                inode->current_path = rpaths.current;
                inode->published_path = rpaths.published;
                inode->next_data_sequence = descriptor.next_data_sequence;

                std::optional<FsEntry> committed;
                if (rpaths.published)
                    committed = snapshot_entry(snapshot, fs.namespace_nodes(), *rpaths.published);
                inode->base = committed ? *committed : descriptor.base;
                inode->visible = inode->base;
                if (!committed && descriptor.visible.type == inode->base.type)
                    inode->visible = descriptor.visible;
                auto operations = recovery.data_ops.find(id);
                const auto done = recovery.data_done[id];
                if (operations != recovery.data_ops.end()) {
                    for (const auto& op : operations->second) {
                        inode->next_data_sequence =
                            std::max(inode->next_data_sequence, op.sequence + 1);
                        if (op.sequence > done) {
                            auto recovered_op = op;
                            auto memory = retained_memory.restore(
                                WorkClass::loader, MemoryOwner::fuse_operation,
                                recovered_op.metadata_charge);
                            recovered_op.process_memory =
                                std::make_shared<RetainedMemoryLedger::Lease>(std::move(memory));
                            inode->data_ops.push_back(std::move(recovered_op));
                        }
                    }
                }
                apply_pending_data_metadata(*inode, inode->data_ops);
                // After the data ops, so a utimens admitted after the last write
                // sets the mtime shown and published.
                apply_pending_namespace_metadata(*inode, id, recovery);
                rebuild_data_overlay_locked(*inode);
                refresh_retained_owners_locked(*inode);
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
                inode->recovery_data_sequence = inode->durable_data_sequence;
                inode->requested_namespace_sequence = inode->namespace_sequence;
                recover_spool(inode, inode->data_ops);
                current_path = inode->current_path;
                namespace_sequence = inode->namespace_sequence;
            }
            if (!current_path.empty()) {
                const auto key = canonical_path(current_path);
                auto existing = paths.find(key);
                if (existing != paths.end() && existing->second->id != inode->id) {
                    // Two inodes on one path: resolve as a rename-over would.
                    // A journaled inode beats a snapshot-seeded one; between
                    // journaled ones the later namespace sequence wins. The
                    // loser keeps its data, detached, and is re-journaled
                    // without the path. Deterministic, never fatal.
                    auto holder = existing->second;
                    uint64_t holder_sequence = 0;
                    {
                        Lock holder_lock(holder->mutex);
                        holder_sequence = holder->namespace_sequence;
                    }
                    const bool holder_journaled = recovery.inodes.contains(holder->id);
                    const bool inode_wins =
                        !holder_journaled || namespace_sequence > holder_sequence;
                    auto winner = inode_wins ? inode : holder;
                    auto loser = inode_wins ? holder : inode;
                    Log::warn("FUSE journal recovery: two inodes resolve to one path; keeping "
                              "the newer path=" + key + " kept_inode=" +
                              std::to_string(winner->id) + " kept_seq=" +
                              std::to_string(inode_wins ? namespace_sequence : holder_sequence) +
                              " detached_inode=" + std::to_string(loser->id) + " detached_seq=" +
                              std::to_string(inode_wins ? holder_sequence : namespace_sequence) +
                              " detached_journaled=" +
                              (recovery.inodes.contains(loser->id) ? "yes" : "no"));
                    {
                        Lock loser_lock(loser->mutex);
                        loser->current_path.clear();
                        loser->published_path.reset();
                        if (recovery.inodes.contains(loser->id)) {
                            // It carries the current epoch, which would make
                            // journal_inode_locked() a no-op; force it.
                            loser->journal_epoch = std::numeric_limits<uint64_t>::max();
                            Lock admission(journal_admission_mutex);
                            journal_inode_locked(loser);
                        }
                    }
                    // The loser stays registered by id, without a path.
                    paths[key] = winner;
                } else {
                    paths[key] = inode;
                }
            }
            inodes[id] = inode;
            note_inode_inserted_locked();
        }

        if (!paths.contains(canonical_path("/")))
            throw std::runtime_error("FUSE frontend cannot initialise without namespace root");

        // A journaled batch covered by the snapshot clock committed atomically
        // even if per-op markers were lost; never re-apply it (create + rename
        // re-applied would overwrite the final file).
        const auto fuse_clock = fuse_namespace_clock(snapshot);
        const auto committed_in_batch = [&](uint64_t sequence) {
            auto batch = recovery.namespace_batches.upper_bound(sequence);
            if (batch == recovery.namespace_batches.begin())
                return false;
            --batch;
            return sequence < batch->first + batch->second && fuse_clock >= batch->first;
        };
        size_t recovered_namespace_operations = 0;
        size_t recovered_by_identity = 0;
        size_t namespace_pending = 0;
        {
            Lock queue_lock(namespace_queue_mutex);
            for (const auto& [sequence, op_const] : recovery.namespace_ops) {
                if (recovery.namespace_done.contains(sequence))
                    continue;
                auto op = op_const;
                retain_namespace_references_locked(op);
                ++recovered_namespace_operations;
                if (recovery.namespace_published.contains(sequence)) {
                    namespace_unconfirmed.push_back(op);
                } else if (committed_in_batch(sequence)) {
                    op.published_generation = view.generation;
                    namespace_unconfirmed.push_back(op);
                    ++recovered_by_identity;
                } else {
                    namespace_queue.push_back(op);
                }
            }
            namespace_pending = namespace_queue.size() + namespace_unconfirmed.size();
        }
        if (recovered_by_identity)
            Log::info("FUSE journal recovery: " + std::to_string(recovered_by_identity) +
                      " namespace operations confirmed committed by batch identity clock=" +
                      std::to_string(fuse_clock));
        // Else a lost journal with a surviving clock would make every new
        // batch look already committed.
        next_namespace_sequence = std::max<uint64_t>(next_namespace_sequence, fuse_clock + 1);
        namespace_operations_recovered.fetch_add(recovered_namespace_operations,
                                                 std::memory_order_relaxed);
        refreshed_namespace_revision.store(view.namespace_revision, std::memory_order_release);
        if (recovery.pending_operations) {
            Log::warn("recovered durable FUSE operations pending=" +
                      std::to_string(durable_pending_operations.load(std::memory_order_relaxed)) +
                      " namespace=" + std::to_string(namespace_pending) +
                      " inodes=" + std::to_string(needed.size()) +
                      " skipped_frames=" + std::to_string(recovery.skipped_frames) +
                      " done_without_published=" +
                      std::to_string(recovery.done_without_published));
        }
        reset_journal_if_idle();
    }

    void resume_recovered_data() {
        std::vector<std::shared_ptr<Inode>> recovered;
        {
            Lock lock(namespace_mutex);
            for (const auto& [_, inode] : inodes) {
                Lock inode_lock(inode->mutex);
                if (!inode->data_ops.empty() &&
                    inode->requested_data_sequence > inode->published_data_sequence &&
                    !inode->unconfirmed_data_entry)
                    recovered.push_back(inode);
            }
        }
        for (const auto& inode : recovered)
            request_data_publication(inode);
    }

    std::string describe_unconfirmed_head() {
        Lock queue_lock(namespace_queue_mutex);
        if (namespace_unconfirmed.empty())
            return "head=none";
        const auto& op = namespace_unconfirmed.front();
        return "head_seq=" + std::to_string(op.sequence) +
               " head_kind=" + std::string(namespace_op_kind_name(op.kind)) +
               " head_from=" + op.from + (op.to.empty() ? "" : " head_to=" + op.to) +
               " head_published_generation=" + std::to_string(op.published_generation);
    }

    void confirm_namespace_from_snapshot(const MetadataSnapshotView& view) {
        std::vector<NamespaceOp> confirmed;
        {
            Lock queue_lock(namespace_queue_mutex);
            for (const auto& op : namespace_unconfirmed) {
                if (!namespace_op_confirmed(op, view, fs.namespace_nodes()))
                    break;
                confirmed.push_back(op);
            }
        }
        if (confirmed.empty())
            return;
        journal_namespace_done(confirmed);
        {
            Lock queue_lock(namespace_queue_mutex);
            for (const auto& op : confirmed) {
                if (namespace_unconfirmed.empty() ||
                    namespace_unconfirmed.front().sequence != op.sequence)
                    break;
                namespace_unconfirmed.pop_front();
            }
        }
        namespace_operations_confirmed.fetch_add(confirmed.size(), std::memory_order_relaxed);
        release_namespace_references(confirmed);
        namespace_cv.notify_all();
    }

    bool confirm_data_from_snapshot(const std::shared_ptr<Inode>& inode,
                                    const MetadataSnapshot& snapshot) {
        bool more = false;
        bool spool_clean = true;
        bool journal_idle = false;
        {
            // Held across journal_data_done() so concurrent confirmers cannot
            // both retire the same prefix.
            Lock lock(inode->mutex);
            if (!inode->unconfirmed_data_entry || !inode->unconfirmed_data_sequence)
                return false;

            const auto target = inode->unconfirmed_data_sequence;
            const auto expected = *inode->unconfirmed_data_entry;
            const auto path = inode->published_path;
            const auto retired = static_cast<size_t>(
                std::count_if(inode->data_ops.begin(), inode->data_ops.end(),
                              [&](const DataOp& op) { return op.sequence <= target; }));
            if (!retired)
                return false;
            if (path) {
                auto entry = snapshot_entry(snapshot, fs.namespace_nodes(), *path);
                if (!entry || !same_file_content(*entry, expected))
                    return false;
            }

            journal_idle = journal_data_done(inode->id, target, retired);
            if (path) {
                auto entry = snapshot_entry(snapshot, fs.namespace_nodes(), *path);
                if (entry)
                    inode->base = *entry;
            } else {
                inode->base = expected;
            }
            inode->data_ops.erase(
                std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                               [&](const DataOp& op) { return op.sequence <= target; }),
                inode->data_ops.end());
            if (inode->data_ops.empty())
                std::vector<DataOp>{}.swap(inode->data_ops);
            rebuild_data_overlay_locked(*inode);
            refresh_retained_owners_locked(*inode);
            data_publication_bytes_confirmed.fetch_add(inode->unconfirmed_publication_bytes,
                                                       std::memory_order_relaxed);
            inode->unconfirmed_publication_bytes = 0;
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
        reclaim_inode_if_quiescent(inode->id);
        return true;
    }

    void confirm_data_from_snapshot(const MetadataSnapshot& snapshot) {
        std::vector<std::shared_ptr<Inode>> candidates;
        {
            Lock lock(namespace_mutex);
            candidates.reserve(inodes.size());
            for (const auto& [_, inode] : inodes) {
                Lock inode_lock(inode->mutex);
                if (inode->unconfirmed_data_entry)
                    candidates.push_back(inode);
            }
        }
        for (const auto& inode : candidates)
            (void)confirm_data_from_snapshot(inode, snapshot);
    }

    bool have_unconfirmed_namespace() {
        Lock lock(namespace_queue_mutex);
        return !namespace_unconfirmed.empty();
    }

    void refresh_namespace_if_stale() {
        const auto current_revision = refreshed_namespace_revision.load(std::memory_order_acquire);
        const bool pending_confirmation = have_unconfirmed_namespace();
        const auto available_revision = fs.available_namespace_revision();
        if (available_revision <= current_revision && !pending_confirmation)
            return;

        // Log why a newer view is not adopted, once per revision, so a stalled
        // mount is distinguishable from an idle cluster.
        auto deferred = [&](const std::string& reason, uint64_t view_revision = 0) {
            if (available_revision <= current_revision)
                return;
            if (namespace_refresh_deferred_logged_revision.exchange(available_revision) ==
                available_revision)
                return;
            Log::debug("FUSE namespace refresh deferred reason=" + reason +
                       " available_revision=" + std::to_string(available_revision) +
                       " view_revision=" + std::to_string(view_revision) +
                       " refreshed_revision=" + std::to_string(current_revision));
        };

        // Adopt only a snapshot MetadataManager has already decoded; kernel
        // requests never do metadata-replica I/O.
        Lock refresh_lock(refresh_mutex);
        {
            Lock queue_lock(namespace_queue_mutex);
            // Local ops are already applied optimistically; do not race their
            // publication. A later request retries.
            if (namespace_inflight || !namespace_queue.empty()) {
                deferred("namespace-queue");
                return;
            }
        }

        const auto available = fs.available_snapshot_view();
        if (!available) {
            deferred("no-view");
            return;
        }
        const auto& view = *available;
        const auto& snapshot = *view.snapshot;

        // Publications retire only once observed in the decoded view, so an
        // older view cannot resurrect a rename/unlink or hide a mkdir/create.
        confirm_namespace_from_snapshot(view);
        confirm_data_from_snapshot(snapshot);
        if (have_unconfirmed_namespace()) {
            deferred("unconfirmed " + describe_unconfirmed_head(), view.namespace_revision);
            return;
        }
        if (view.namespace_revision <= refreshed_namespace_revision.load(std::memory_order_acquire)) {
            deferred("view-not-newer", view.namespace_revision);
            return;
        }

        std::vector<uint64_t> detached;
        size_t adopted_new = 0;
        // Counted during the walk; a tree-backed snapshot has no size.
        size_t walked = 0;
        {
            Lock lock(namespace_mutex);
            {
            // Admissions enqueue before releasing namespace_mutex, so a
            // non-empty queue here means the snapshot predates local state.
            Lock queue_lock(namespace_queue_mutex);
            if (namespace_inflight || !namespace_queue.empty() || !namespace_unconfirmed.empty()) {
                deferred("queue-race", view.namespace_revision);
                return;
            }
        }
            auto adopt_nodes = fs.namespace_nodes();
            const auto adopt_entry = [&](const std::string& path, const FsEntry& entry)
                                         MACHA_REQUIRES(namespace_mutex) {
            ++walked;
            const auto key = canonical_path(path);
            auto found = paths.find(key);
            if (found == paths.end()) {
                auto inode = std::make_shared<Inode>();
                Lock inode_lock(inode->mutex);
                inode->id = path == "/" ? 1 : next_inode++;
                inode->base = entry;
                inode->visible = entry;
                inode->current_path = path;
                inode->published_path = path;
                paths[key] = inode;
                inodes[inode->id] = std::move(inode);
                note_inode_inserted_locked();
                ++adopted_new;
                return;
            }
            auto inode = found->second;
            Lock inode_lock(inode->mutex);
            const bool content_changed = !same_file_content(inode->base, entry);
            // Snapshots carry no stable inode id. A dirty or open inode whose
            // path now holds replacement content is detached, so old writes
            // cannot commit into the new file. For an open read-only inode,
            // changed content without a version advance marks replacement; an
            // advancing version is an in-place change.
            const bool replaced_open_inode =
                content_changed &&
                (!inode->data_ops.empty() || inode->durability_pending ||
                 inode->unconfirmed_data_entry ||
                 (inode->open_handles != 0 && entry.version <= inode->base.version));
            if (replaced_open_inode && path != "/") {
                inode->published_path.reset();
                auto replacement = std::make_shared<Inode>();
                Lock replacement_lock(replacement->mutex);
                replacement->id = next_inode++;
                replacement->base = entry;
                replacement->visible = entry;
                replacement->current_path = path;
                replacement->published_path = path;
                found->second = replacement;
                inodes[replacement->id] = std::move(replacement);
                note_inode_inserted_locked();
                return;
            }
            inode->published_path = path;
            if (inode->data_ops.empty() && !inode->durability_pending &&
                !inode->unconfirmed_data_entry) {
                inode->base = entry;
                inode->visible = entry;
                inode->admitted_size = entry.size;
            }
            };
            // Remote unlink, POSIX semantics: the name goes now; open or dirty
            // state keeps the inode but cannot resurrect the path.
            const auto drop_path = [&](decltype(paths)::iterator it)
                                       MACHA_REQUIRES(namespace_mutex) {
                auto inode = it->second;
                Lock inode_lock(inode->mutex);
                inode->published_path.reset();
                // Reclaimed after namespace_mutex is released.
                detached.push_back(inode->id);
                return paths.erase(it);
            };

            const bool by_difference =
                snapshot.namespace_root && adopted_namespace_root &&
                !namespace_view_diverged.load(std::memory_order_acquire);
            if (by_difference) {
                for (const auto& [path, difference] : diff_namespace_trees(
                         *adopted_namespace_root, *snapshot.namespace_root, adopt_nodes)) {
                    if (difference.after) {
                        adopt_entry(path, *difference.after);
                        continue;
                    }
                    const auto key = canonical_path(path);
                    auto it = paths.find(key);
                    if (it == paths.end())
                        continue;
                    {
                        // The name stands for another spelling of it.
                        Lock inode_lock(it->second->mutex);
                        if (it->second->published_path && *it->second->published_path != path)
                            continue;
                    }
                    // A canonically-equivalent spelling may remain under the
                    // canonical name itself.
                    if (key != path) {
                        if (const auto twin = namespace_entry(snapshot, &adopt_nodes, key)) {
                            adopt_entry(key, *twin);
                            continue;
                        }
                    }
                    ++walked;
                    drop_path(it);
                }
            } else {
                std::set<std::string, std::less<>> seen;
                for_each_namespace_entry(snapshot, &adopt_nodes,
                                         [&](const std::string& path, const FsEntry& entry)
                                             MACHA_REQUIRES(namespace_mutex) {
                    seen.insert(canonical_path(path));
                    adopt_entry(path, entry);
                });
                for (auto it = paths.begin(); it != paths.end();) {
                    if (it->first == canonical_path("/") || seen.contains(it->first))
                        ++it;
                    else
                        it = drop_path(it);
                }
            }
            adopted_namespace_root = snapshot.namespace_root;
            namespace_view_diverged.store(false, std::memory_order_release);
            refreshed_namespace_revision.store(view.namespace_revision,
                                                 std::memory_order_release);
        }
        for (auto id : detached)
            reclaim_inode_if_quiescent(id);
        if (Log::enabled(LogLevel::debug))
            Log::debug("FUSE namespace adopted revision=" + std::to_string(view.namespace_revision) +
                       " generation=" + std::to_string(view.generation) +
                       " visited=" + std::to_string(walked) +
                       " new=" + std::to_string(adopted_new) +
                       " detached=" + std::to_string(detached.size()));
    }

    void start() {
        auto recovery = load_journal();
        initialise_namespace(std::move(recovery));

        // Must run before broker workers accept writes.
        durability_worker = std::jthread([this](std::stop_token stop) {
            run_supervised_loop("fuse-durability", stop, [this, stop] { durability_loop(stop); });
        });

        // One worker per operation class, the rest weighted to reads, writes
        // and lookups.
        const std::array<size_t, 12> preference{2, 3, 2, 3, 2, 0, 2, 3, 0, 1, 4, 5};
        size_t workers = 0;
        for (size_t i = 0; i < broker.size() && workers < config.request_workers; ++i, ++workers)
            broker[i].workers.emplace_back(
                [this, i](std::stop_token stop) { broker_loop(i, stop); });
        size_t preference_index = 0;
        while (workers < config.request_workers) {
            const auto index = preference[preference_index++ % preference.size()];
            broker[index].workers.emplace_back(
                [this, index](std::stop_token stop) { broker_loop(index, stop); });
            ++workers;
        }

        namespace_worker = std::jthread([this](std::stop_token stop) {
            run_supervised_loop("fuse-namespace", stop, [this, stop] { namespace_loop(stop); });
        });
        data_workers.reserve(config.commit_workers);
        for (size_t i = 0; i < config.commit_workers; ++i)
            data_workers.emplace_back([this](std::stop_token stop) {
                run_supervised_loop("fuse-data", stop, [this, stop] { data_loop(stop); });
            });
        // For comparison with runtime.loader_memory_reserve_bytes.
        if (config.publication_max_open_writers) {
            const auto per_writer =
                static_cast<uint64_t>(fs.extent_size()) + config.publication_pipeline_bytes;
            Log::info("FUSE publication open-writer bound=" +
                      std::to_string(config.publication_max_open_writers) +
                      " per_writer_bytes=" + std::to_string(per_writer) + " worst_case_bytes=" +
                      std::to_string(per_writer * config.publication_max_open_writers));
        } else {
            Log::warn("FUSE publication open-writer bound is disabled; retained-memory "
                      "admission can deadlock against itself under a wide backlog");
        }

        // Recovered state is complete before workers start; publication
        // resumes once they all run.
        resume_recovered_data();
        namespace_cv.notify_all();
    }

    void interrupt_waits() {
        {
            Lock lock(data_queue_mutex);
            waits_interrupted.store(true, std::memory_order_release);
        }
        data_cv.notify_all();
    }

    void stop() {
        interrupt_waits();
        if (stopping.exchange(true))
            return;
        namespace_cv.notify_all();
        data_cv.notify_all();
        spool_admission_cv.notify_all();
        write_request_cv.notify_all();
        operation_metadata_cv.notify_all();
        for (auto& queue : broker)
            queue.cv.notify_all();
        if (namespace_worker.joinable())
            namespace_worker.request_stop();
        for (auto& worker : data_workers)
            worker.request_stop();
        for (auto& queue : broker)
            for (auto& worker : queue.workers)
                worker.request_stop();
        if (namespace_worker.joinable())
            namespace_worker.join();
        for (auto& worker : data_workers)
            if (worker.joinable())
                worker.join();
        for (auto& queue : broker) {
            queue.cv.notify_all();
            for (auto& worker : queue.workers)
                if (worker.joinable())
                    worker.join();
        }

        // Last, since broker writers wait on durability tickets; it drains the
        // final batch.
        if (durability_worker.joinable()) {
            durability_worker.request_stop();
            durability_cv.notify_all();
            durability_worker.join();
        }
    }
};

FuseFrontend::FuseFrontend(FileSystem& filesystem, RetainedMemoryLedger& retained_memory,
                           FuseConfig config, std::unique_ptr<LoaderAdmission> admission,
                           PublicationTarget& target, std::stop_token stop)
    : state_(std::make_unique<State>(filesystem, retained_memory, std::move(config),
                                     std::move(admission), target, std::move(stop))) {
    state_->start();
}

FuseFrontend::~FuseFrontend() {
    stop();
}

std::chrono::milliseconds FuseFrontend::timeout_for(FuseOperationClass operation) const {
    switch (operation) {
    case FuseOperationClass::lookup:
        return state_->config.timeouts.lookup;
    case FuseOperationClass::namespace_mutation:
        return state_->config.timeouts.namespace_mutation;
    case FuseOperationClass::read:
        return state_->config.timeouts.read;
    case FuseOperationClass::write:
        return state_->config.timeouts.write;
    case FuseOperationClass::sync:
        return state_->config.timeouts.sync;
    case FuseOperationClass::lifecycle:
        return state_->config.timeouts.lifecycle;
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
        Lock lock(queue.mutex);
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
    return dispatch(FuseOperationClass::lookup,
                    [this, requested](Clock::time_point deadline, std::atomic_bool& cancelled) {
                        check_deadline(deadline, cancelled);
                        state_->refresh_namespace_if_stale();
                        check_deadline(deadline, cancelled);
                        std::shared_ptr<State::Inode> inode;
                        {
                            Lock lock(state_->namespace_mutex);
                            inode = state_->resolve_locked(requested);
                        }
                        Lock inode_lock(inode->mutex);
                        check_deadline(deadline, cancelled);
                        if (inode->backend_error)
                            throw FsError(*inode->backend_error, "asynchronous backend error");
                        return fuse_attributes(inode->visible);
                    });
}

std::vector<std::pair<std::string, FuseEntryAttributes>>
FuseFrontend::readdir(std::string_view path) {
    const auto requested = canonical_path(path);
    return dispatch(FuseOperationClass::lookup, [this, requested](Clock::time_point deadline,
                                                                  std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        std::vector<std::pair<std::string, FuseEntryAttributes>> out;
        Lock lock(state_->namespace_mutex);
        auto parent = state_->resolve_locked(requested);
        std::string parent_current_path;
        {
            Lock inode_lock(parent->mutex);
            if (parent->visible.type != EntryType::directory)
                throw FsError(ENOTDIR, "not directory");
            parent_current_path = parent->current_path;
        }
        state_->for_each_child_locked(parent_current_path, [&](const auto& inode) {
            check_deadline(deadline, cancelled);
            Lock inode_lock(inode->mutex);
            if (inode->current_path != "/" &&
                parent_path(inode->current_path) == parent_current_path)
                out.push_back({base_name(inode->current_path), fuse_attributes(inode->visible)});
            return true;
        });
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return out;
    });
}

void FuseFrontend::mkdir(std::string_view path, uint32_t mode, uint32_t uid, uint32_t gid) {
    const auto requested = canonical_path(path);
    dispatch(
        FuseOperationClass::namespace_mutation,
        [this, requested, mode, uid, gid](Clock::time_point deadline, std::atomic_bool& cancelled) {
            Lock accept(state_->namespace_apply_mutex);
            check_deadline(deadline, cancelled);
            state_->refresh_namespace_if_stale();
            check_deadline(deadline, cancelled);
            if (!state_->namespace_capacity_available())
                throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
            State::NamespaceOp op;
            {
                Lock lock(state_->namespace_mutex);
                if (state_->paths.contains(requested))
                    throw FsError(EEXIST, "exists");
                state_->require_parent_locked(requested);
                check_deadline(deadline, cancelled);
                auto inode = std::make_shared<State::Inode>();
                Lock inode_lock(inode->mutex);
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

                // Journaled before the change becomes visible or succeeds.
                {
                    Lock journal_admission(state_->journal_admission_mutex);
                    state_->journal_inode_locked(inode, false);
                    state_->journal_namespace_operation(op);
                }
                state_->paths[requested] = inode;
                state_->inodes[inode->id] = inode;
                state_->note_inode_inserted_locked();
                state_->enqueue_namespace(std::move(op));
            }
        });
}

void FuseFrontend::rmdir(std::string_view path) {
    const auto requested = canonical_path(path);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, requested](Clock::time_point deadline, std::atomic_bool& cancelled) {
                 Lock accept(state_->namespace_apply_mutex);
                 check_deadline(deadline, cancelled);
                 state_->refresh_namespace_if_stale();
                 check_deadline(deadline, cancelled);
                 if (!state_->namespace_capacity_available())
                     throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
                 State::NamespaceOp op;
                 {
                     Lock lock(state_->namespace_mutex);
                     auto inode = state_->resolve_locked(requested);
                     Lock inode_lock(inode->mutex);
                     if (inode->visible.type != EntryType::directory)
                         throw FsError(ENOTDIR, "not directory");
                     if (requested == "/")
                         throw FsError(EBUSY, "cannot remove root");
                     const std::string directory = inode->current_path;
                     state_->for_each_child_locked(directory, [&](const auto& candidate) {
                         if (candidate == inode)
                             return true;
                         Lock candidate_lock(candidate->mutex);
                         if (parent_path(candidate->current_path) == directory)
                             throw FsError(ENOTEMPTY, "directory not empty");
                         return true;
                     });
                     check_deadline(deadline, cancelled);
                     const auto sequence = state_->next_namespace_sequence++;
                     op.kind = State::NamespaceOp::Kind::rmdir;
                     op.sequence = sequence;
                     op.from = requested;
                     op.ctime_ns = wall_time_ns();
                     op.affected = {inode->id};
                     {
                         Lock journal_admission(state_->journal_admission_mutex);
                         state_->journal_inode_locked(inode, false);
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
                 Lock accept(state_->namespace_apply_mutex);
                 check_deadline(deadline, cancelled);
                 state_->refresh_namespace_if_stale();
                 check_deadline(deadline, cancelled);
                 if (!state_->namespace_capacity_available())
                     throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
                 State::NamespaceOp op;
                 {
                     Lock lock(state_->namespace_mutex);
                     auto inode = state_->resolve_locked(requested);
                     Lock inode_lock(inode->mutex);
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
                         Lock journal_admission(state_->journal_admission_mutex);
                         state_->journal_inode_locked(inode, false);
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
    dispatch(FuseOperationClass::namespace_mutation, [this, source, destination,
                                                      noreplace](Clock::time_point deadline,
                                                                 std::atomic_bool& cancelled) {
        if (source == destination)
            return;
        Lock accept(state_->namespace_apply_mutex);
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        if (!state_->namespace_capacity_available())
            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
        State::NamespaceOp op;
        {
            Lock lock(state_->namespace_mutex);
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
                    PairLock pair_lock(root->mutex, displaced_inode->mutex);
                    if (root->visible.type != displaced_inode->visible.type)
                        throw FsError(root->visible.type == EntryType::directory ? ENOTDIR : EISDIR,
                                      "rename type mismatch");
                }
                {
                    Lock destination_lock(displaced_inode->mutex);
                    if (displaced_inode->visible.type == EntryType::directory) {
                        const std::string displaced = displaced_inode->current_path;
                        state_->for_each_child_locked(displaced, [&](const auto& candidate) {
                            if (candidate == displaced_inode || candidate == root)
                                return true;
                            Lock candidate_lock(candidate->mutex);
                            if (parent_path(candidate->current_path) == displaced)
                                throw FsError(ENOTEMPTY, "destination directory not empty");
                            return true;
                        });
                    }
                }
            }

            std::vector<std::shared_ptr<State::Inode>> affected_inodes;
            state_->for_each_under_locked(source, [&](const auto& inode) {
                Lock inode_lock(inode->mutex);
                if (under_path(inode->current_path, source))
                    affected_inodes.push_back(inode);
            });
            const auto sequence = state_->next_namespace_sequence++;
            op.kind = State::NamespaceOp::Kind::rename;
            op.sequence = sequence;
            op.from = source;
            op.to = destination;
            op.noreplace = noreplace;
            op.ctime_ns = wall_time_ns();
            op.affected.reserve(affected_inodes.size());
            state_->journal_rename(affected_inodes, displaced_inode, op);

            // Exposed locally only once durable.
            if (displaced_inode) {
                Lock inode_lock(displaced_inode->mutex);
                displaced_inode->current_path.clear();
                displaced_inode->namespace_sequence = sequence;
                state_->paths.erase(destination);
            }
            for (const auto& inode : affected_inodes) {
                Lock inode_lock(inode->mutex);
                const auto old = inode->current_path;
                state_->paths.erase(canonical_path(old));
                inode->current_path = destination + old.substr(source.size());
                inode->namespace_sequence = sequence;
            }
            for (const auto& inode : affected_inodes) {
                Lock inode_lock(inode->mutex);
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
                 Lock accept(state_->namespace_apply_mutex);
                 check_deadline(deadline, cancelled);
                 state_->refresh_namespace_if_stale();
                 check_deadline(deadline, cancelled);
                 if (!state_->namespace_capacity_available())
                     throw FsError(EAGAIN, "namespace queue saturated");
                 State::NamespaceOp op;
                 {
                     Lock lock(state_->namespace_mutex);
                     auto inode = state_->resolve_locked(requested);
                     Lock inode_lock(inode->mutex);
                     const auto sequence = state_->next_namespace_sequence++;
                     const auto now = wall_time_ns();
                     op.kind = State::NamespaceOp::Kind::chmod;
                     op.sequence = sequence;
                     op.from = requested;
                     op.mode = mode;
                     op.ctime_ns = now;
                     op.affected = {inode->id};
                     {
                         Lock journal_admission(state_->journal_admission_mutex);
                         state_->journal_inode_locked(inode, false);
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
                 Lock accept(state_->namespace_apply_mutex);
                 check_deadline(deadline, cancelled);
                 state_->refresh_namespace_if_stale();
                 check_deadline(deadline, cancelled);
                 if (!state_->namespace_capacity_available())
                     throw FsError(EAGAIN, "namespace queue saturated");
                 State::NamespaceOp op;
                 {
                     Lock lock(state_->namespace_mutex);
                     auto inode = state_->resolve_locked(requested);
                     Lock inode_lock(inode->mutex);
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
                         Lock journal_admission(state_->journal_admission_mutex);
                         state_->journal_inode_locked(inode, false);
                         state_->journal_namespace_operation(op);
                     }
                     if (set_uid)
                         inode->visible.uid = uid;
                     if (set_gid)
                         inode->visible.gid = gid;
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
                 Lock accept(state_->namespace_apply_mutex);
                 check_deadline(deadline, cancelled);
                 state_->refresh_namespace_if_stale();
                 check_deadline(deadline, cancelled);
                 if (!state_->namespace_capacity_available())
                     throw FsError(EAGAIN, "namespace queue saturated");
                 State::NamespaceOp op;
                 {
                     Lock lock(state_->namespace_mutex);
                     auto inode = state_->resolve_locked(requested);
                     Lock inode_lock(inode->mutex);
                     const auto sequence = state_->next_namespace_sequence++;
                     const auto now = wall_time_ns();
                     op.kind = State::NamespaceOp::Kind::utimens;
                     op.sequence = sequence;
                     op.from = requested;
                     op.mtime_ns = mtime_ns;
                     op.ctime_ns = now;
                     op.affected = {inode->id};
                     {
                         Lock journal_admission(state_->journal_admission_mutex);
                         state_->journal_inode_locked(inode, false);
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
    return dispatch(FuseOperationClass::lifecycle, [this, requested, readable, writable, append,
                                                    truncate_on_open](Clock::time_point deadline,
                                                                      std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        std::shared_ptr<State::Inode> inode;
        {
            Lock lock(state_->namespace_mutex);
            inode = state_->resolve_locked(requested);
        }
        if (truncate_on_open)
            state_->wait_for_inode_durability(inode, deadline, cancelled);
        auto process_memory = truncate_on_open
                                  ? state_->reserve_process_memory(
                                        WorkClass::loader, MemoryOwner::fuse_operation,
                                        State::operation_metadata_charge(0), deadline)
                                  : nullptr;
        auto metadata_admission = truncate_on_open
                                      ? state_->reserve_operation_metadata(
                                            State::operation_metadata_charge(0), deadline,
                                            [&] { state_->request_data_publication(inode); })
                                      : nullptr;
        Lock inode_lock(inode->mutex);
        if (inode->visible.type != EntryType::file)
            throw FsError(EISDIR, "directory");
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "asynchronous backend error");
        if (truncate_on_open) {
            const auto seq = inode->next_data_sequence;
            const auto now = wall_time_ns();
            State::DataOp op;
            op.kind = State::DataOp::Kind::truncate;
            op.metadata_charge = State::operation_metadata_charge(0);
            op.process_memory = process_memory;
            op.sequence = seq;
            op.size = 0;
            op.mtime_ns = now;
            op.ctime_ns = now;
            {
                Lock journal_admission(state_->journal_admission_mutex);
                state_->journal_inode_locked(inode);
                state_->journal_data_operation(inode->id, op);
            }
            inode->next_data_sequence = seq + 1;
            State::apply_data_overlay_locked(*inode, op);
            inode->data_ops.push_back(op);
            inode->accounted_operation_metadata_bytes += op.metadata_charge;
            metadata_admission->release = {};
            state_->refresh_retained_owners_locked(*inode);
            inode->durable_data_sequence = seq;
            inode->visible.size = 0;
            inode->visible.mtime_ns = now;
            inode->visible.ctime_ns = now;
            inode->admitted_size = 0;
        }
        ++inode->open_handles;
        if (writable) {
            ++inode->writable_handles;
            state_->open_writers.fetch_add(1, std::memory_order_relaxed);
        }
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
                    [this, requested, mode, uid, gid, readable, writable,
                     append](Clock::time_point deadline, std::atomic_bool& cancelled) {
                        Lock accept(state_->namespace_apply_mutex);
                        check_deadline(deadline, cancelled);
                        state_->refresh_namespace_if_stale();
                        check_deadline(deadline, cancelled);
                        if (!state_->namespace_capacity_available())
                            throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
                        State::NamespaceOp op;
                        std::shared_ptr<State::Inode> inode;
                        {
                            Lock lock(state_->namespace_mutex);
                            if (state_->paths.contains(requested))
                                throw FsError(EEXIST, "exists");
                            state_->require_parent_locked(requested);
                            inode = std::make_shared<State::Inode>();
                            Lock inode_lock(inode->mutex);
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
                            inode->writable_handles = writable ? 1 : 0;

                            op.kind = State::NamespaceOp::Kind::create;
                            op.sequence = inode->namespace_sequence;
                            op.from = requested;
                            op.mode = mode;
                            op.uid = uid;
                            op.gid = gid;
                            op.ctime_ns = now;
                            op.affected = {inode->id};
                            {
                                Lock journal_admission(state_->journal_admission_mutex);
                                state_->journal_inode_locked(inode, false);
                                state_->journal_namespace_operation(op);
                            }
                            if (writable)
                                state_->open_writers.fetch_add(1, std::memory_order_relaxed);
                            state_->paths[requested] = inode;
                            state_->inodes[inode->id] = inode;
                            state_->note_inode_inserted_locked();
                            state_->enqueue_namespace(std::move(op));
                        }
                        std::shared_ptr<FuseReadSession> read_session;
                        if (readable) {
                            read_session = std::make_shared<FuseReadSession>();
                            read_session->inode = inode->id;
                        }
                        return FuseOpenHandle{inode->id, readable, writable, append,
                                              std::move(read_session)};
                    });
}

size_t FuseFrontend::read_impl(uint64_t inode_id,
                               const std::shared_ptr<FuseReadSession>& read_session,
                               uint64_t offset, std::span<uint8_t> output) {
    return dispatch(FuseOperationClass::read, [this, inode_id, read_session, offset,
                                               output](Clock::time_point deadline,
                                                       std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        struct ReadOverlay {
            uint64_t begin{};
            uint64_t end{};
            uint64_t spool_offset{};
            bool zero{};
        };
        std::optional<FsEntry> base_snapshot;
        uint64_t base_size = 0;
        uint64_t base_version = 0;
        uint64_t visible_size = 0;
        size_t count = 0;
        std::vector<ReadOverlay> overlay;
        std::filesystem::path spool_path;
        std::string logical_path;
        {
            Lock lock(inode->mutex);
            if (inode->visible.type != EntryType::file)
                throw FsError(EISDIR, "directory");
            visible_size = inode->visible.size;
            if (offset >= visible_size || output.empty())
                return size_t{0};
            count = static_cast<size_t>(
                std::min<uint64_t>(output.size(), visible_size - offset));
            base_size = inode->base.size;
            base_version = inode->base.version;
            if (offset < base_size) {
                bool need_snapshot = !read_session;
                if (read_session) {
                    Lock session_lock(read_session->mutex);
                    need_snapshot = !read_session->reader ||
                                    read_session->base_version != base_version;
                }
                if (need_snapshot)
                    base_snapshot = inode->base;
            }
            spool_path = inode->spool_path;
            logical_path =
                inode->current_path.empty() ? std::string("<unlinked>") : inode->current_path;

            auto range = inode->data_overlay.lower_bound(offset);
            if (range != inode->data_overlay.begin()) {
                auto previous = std::prev(range);
                if (previous->second.end > offset)
                    range = previous;
            }
            uint64_t examined = 0;
            const auto requested_end = offset + count;
            for (; range != inode->data_overlay.end() && range->first < requested_end; ++range) {
                ++examined;
                const auto copy_begin = std::max(offset, range->first);
                const auto copy_end = std::min(requested_end, range->second.end);
                if (copy_begin >= copy_end)
                    continue;
                overlay.push_back(
                    {copy_begin, copy_end,
                     range->second.zero
                         ? 0
                         : range->second.spool_offset + (copy_begin - range->first),
                     range->second.zero});
            }
            state_->data_overlay_read_queries.fetch_add(1, std::memory_order_relaxed);
            state_->data_overlay_ranges_examined.fetch_add(examined,
                                                           std::memory_order_relaxed);
            state_->data_overlay_descriptors_copied.fetch_add(overlay.size(),
                                                              std::memory_order_relaxed);
        }
        std::fill_n(output.data(), count, uint8_t{0});

        if (offset < base_size) {
            const auto base_count =
                static_cast<size_t>(std::min<uint64_t>(count, base_size - offset));
            std::shared_ptr<ReadHandle> reader;
            if (read_session) {
                Lock session_lock(read_session->mutex);
                if (read_session->inode != inode_id)
                    throw FsError(EBADF, "FUSE read session inode mismatch");
                if (!read_session->reader || read_session->base_version != base_version) {
                    if (!base_snapshot)
                        throw std::logic_error("FUSE read base snapshot unavailable");
                    read_session->reader =
                        state_->fs.open_read(*base_snapshot, logical_path, false,
                                             FrameType::loader);
                    read_session->base_version = base_version;
                }
                reader = read_session->reader;
            } else {
                if (!base_snapshot)
                    throw std::logic_error("FUSE read base snapshot unavailable");
                reader = state_->fs.open_read(*base_snapshot, logical_path, false,
                                              FrameType::loader);
            }
            size_t done = 0;
            while (done < base_count) {
                check_deadline(deadline, cancelled);
                auto n = reader->read(offset + done, {output.data() + done, base_count - done},
                                      deadline, &cancelled);
                if (!n)
                    break;
                done += n;
            }
            if (done < base_count)
                throw FsError(Clock::now() >= deadline ? ETIMEDOUT : EIO,
                              "FUSE read source unavailable");
        }

        ScopedFd spool;
        for (const auto& range : overlay) {
            check_deadline(deadline, cancelled);
            if (range.zero) {
                std::fill(output.begin() + static_cast<ptrdiff_t>(range.begin - offset),
                          output.begin() + static_cast<ptrdiff_t>(range.end - offset), 0);
                continue;
            }
            if (spool.get() < 0) {
                const int fd = ::open(spool_path.c_str(), O_RDONLY);
                if (fd < 0)
                    throw FsError(errno, "cannot open FUSE write spool for read");
                spool.reset(fd);
            }
            const auto n = static_cast<size_t>(range.end - range.begin);
            if (pread_exact(spool.get(),
                            {output.data() + static_cast<size_t>(range.begin - offset), n},
                            range.spool_offset) != n)
                throw FsError(EIO, "short FUSE spool read");
        }

        // Kernel demand also becomes a high-priority cache hint, for read-ahead
        // and retries.
        if (base_snapshot && !base_snapshot->extents.empty()) {
            const auto& base = *base_snapshot;
            size_t first = base.extents.size();
            size_t last = 0;
            for (size_t i = 0; i < base.extents.size(); ++i) {
                const auto& extent = base.extents[i];
                if (extent.offset + extent.length <= offset)
                    continue;
                if (extent.offset >= offset + count)
                    break;
                first = std::min(first, i);
                last = i;
            }
            if (first < base.extents.size()) {
                std::function<void()> wake;
                {
                    Lock hint_lock(state_->hint_mutex);
                    state_->hint_states[inode_id] = State::HintState{
                        base, first, last, Clock::now() + state_->config.hint_lifetime};
                    wake = state_->hint_wake_callback;
                }
                if (wake)
                    wake();
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
    const auto admission_deadline =
        Clock::now() + std::min(timeout_for(FuseOperationClass::write), absolute_timeout());
    auto write_admission =
        state_->reserve_write_request_bytes(static_cast<uint64_t>(data.size()), admission_deadline);
    auto process_write_admission = state_->reserve_process_memory(
        WorkClass::loader, MemoryOwner::fuse_request, data.size(), admission_deadline);
    Bytes owned(data.begin(), data.end());
    return dispatch(
        FuseOperationClass::write, [this, inode_id, offset, append,
                                    write_admission = std::move(write_admission),
                                    process_write_admission = std::move(process_write_admission),
                                    owned = std::move(owned)](
                                       Clock::time_point deadline, std::atomic_bool& cancelled) {
            (void)write_admission;
            (void)process_write_admission;
            auto inode = state_->resolve_inode(inode_id);
            state_->throw_if_durability_poisoned();
            check_deadline(deadline, cancelled);

            const auto checksum_count =
                (owned.size() + State::spool_checksum_chunk_size - 1) /
                State::spool_checksum_chunk_size;
            const auto metadata_charge = State::operation_metadata_charge(checksum_count);
            auto process_operation = state_->reserve_process_memory(
                WorkClass::loader, MemoryOwner::fuse_operation, metadata_charge, deadline);
            auto metadata_admission = state_->reserve_operation_metadata(
                metadata_charge, deadline, [&] { state_->request_data_publication(inode); });

            // May wait on publication: never hold inode.mutex here, as
            // durability and publication need it to progress.
            state_->reserve_spool_bytes(static_cast<uint64_t>(owned.size()));
            // The request's time budget starts after admission.
            deadline = Clock::now() + timeout_for(FuseOperationClass::write);
            check_deadline(deadline, cancelled);
            bool reservation_transferred = false;

            auto ticket = std::make_shared<State::DurabilityTicket>();
            ticket->inode = inode;
            uint64_t spool_offset = 0;
            int fd = -1;
            try {
                Lock lock(inode->mutex);
                if (inode->visible.type != EntryType::file)
                    throw FsError(EISDIR, "directory");
                if (inode->backend_error)
                    throw FsError(*inode->backend_error, "asynchronous backend error");
                if (!inode->durability_pending)
                    inode->admitted_size = inode->visible.size;

                const auto target = append ? inode->admitted_size : offset;
                spool_offset = inode->spool_end;
                const auto seq = inode->next_data_sequence;
                const auto now = wall_time_ns();
                ticket->op.kind = State::DataOp::Kind::write;
                ticket->op.metadata_charge = metadata_charge;
                ticket->op.process_memory = process_operation;
                ticket->op.sequence = seq;
                ticket->op.offset = target;
                ticket->op.length = static_cast<uint64_t>(owned.size());
                ticket->op.spool_offset = spool_offset;
                ticket->op.mtime_ns = now;
                ticket->op.ctime_ns = now;
                for (size_t checksum_offset = 0; checksum_offset < owned.size();
                     checksum_offset += State::spool_checksum_chunk_size) {
                    const auto checksum_size =
                        std::min(State::spool_checksum_chunk_size, owned.size() - checksum_offset);
                    ticket->op.spool_hashes.push_back(sha256(
                        std::span<const uint8_t>{owned.data() + checksum_offset, checksum_size}));
                }

                // Allocate before taking spool bytes, so every later failure can
                // roll the tail back exactly under this inode lock.
                State::DataOp overlay_op = ticket->op;
                inode->data_ops.reserve(inode->data_ops.size() + 1);
                bool admission_active = false;
                bool queued = false;
                try {
                    fd = state_->ensure_spool_locked(inode);

                    // The in-flight count keeps compaction from dropping this
                    // descriptor before the group-committed data-op frame.
                    {
                        Lock journal_admission(state_->journal_admission_mutex);
                        state_->journal_inode_locked(inode);
                        state_->journal_inflight_admissions.fetch_add(1, std::memory_order_relaxed);
                        admission_active = true;
                    }

                    if (pwrite_exact(fd, owned, spool_offset) != owned.size())
                        throw FsError(errno ? errno : EIO, "short FUSE spool write");

                    // The worker cannot see the ticket until inode.mutex is
                    // released, and nothing below throws.
                    state_->enqueue_durability(ticket);
                    queued = true;
                    reservation_transferred = true;

                    inode->spool_end += owned.size();
                    inode->admitted_size =
                        std::max<uint64_t>(inode->admitted_size, target + owned.size());
                    inode->next_data_sequence = seq + 1;

                    // Visible locally now; publication is clamped to the
                    // durable prefix, so nothing unstable leaves this node.
                    State::apply_data_overlay_locked(*inode, overlay_op);
                    inode->data_ops.push_back(std::move(overlay_op));
                    inode->accounted_operation_metadata_bytes += metadata_charge;
                    metadata_admission->release = {};
                    state_->refresh_retained_owners_locked(*inode);
                    inode->visible.size =
                        std::max<uint64_t>(inode->visible.size, target + owned.size());
                    inode->visible.mtime_ns = now;
                    inode->visible.ctime_ns = now;
                    ++inode->durability_pending;
                } catch (...) {
                    if (!queued && admission_active) {
                        Lock journal_admission(state_->journal_admission_mutex);
                        const auto previous = state_->journal_inflight_admissions.fetch_sub(
                            1, std::memory_order_relaxed);
                        if (!previous)
                            state_->journal_inflight_admissions.store(0, std::memory_order_relaxed);
                    }
                    if (!queued && fd >= 0) {
                        if (::ftruncate(fd, static_cast<off_t>(spool_offset)) == 0) {
                            try {
                                fsync_fd(fd, "cannot sync rolled-back FUSE spool");
                            } catch (const std::exception& e) {
                                Log::error("cannot sync rolled-back FUSE spool inode=" +
                                           std::to_string(inode->id) + " error=" + e.what());
                            }
                        }
                    }
                    throw;
                }
            } catch (...) {
                if (!reservation_transferred)
                    state_->release_spool_bytes(static_cast<uint64_t>(owned.size()));
                throw;
            }

            // POSIX: accepted, not yet stable. release() waits for local
            // durability; fsync() also for distributed publication.
            return owned.size();
        });
}

void FuseFrontend::truncate(uint64_t inode_id, uint64_t size) {
    dispatch(FuseOperationClass::write,
             [this, inode_id, size](Clock::time_point deadline, std::atomic_bool& cancelled) {
                 auto inode = state_->resolve_inode(inode_id);
                 state_->wait_for_inode_durability(inode, deadline, cancelled);
                 auto process_memory = state_->reserve_process_memory(
                     WorkClass::loader, MemoryOwner::fuse_operation,
                     State::operation_metadata_charge(0), deadline);
                 auto metadata_admission = state_->reserve_operation_metadata(
                     State::operation_metadata_charge(0), deadline,
                     [&] { state_->request_data_publication(inode); });
                 deadline = Clock::now() + timeout_for(FuseOperationClass::write);
                 Lock lock(inode->mutex);
                 check_deadline(deadline, cancelled);
                 if (inode->visible.type != EntryType::file)
                     throw FsError(EISDIR, "directory");
                 if (inode->backend_error)
                     throw FsError(*inode->backend_error, "asynchronous backend error");
                 const auto seq = inode->next_data_sequence;
                 const auto now = wall_time_ns();
                 State::DataOp op;
                 op.kind = State::DataOp::Kind::truncate;
                 op.metadata_charge = State::operation_metadata_charge(0);
                 op.process_memory = process_memory;
                 op.sequence = seq;
                 op.size = size;
                 op.mtime_ns = now;
                 op.ctime_ns = now;
                 {
                     Lock journal_admission(state_->journal_admission_mutex);
                     state_->journal_inode_locked(inode);
                     state_->journal_data_operation(inode->id, op);
                 }
                 inode->next_data_sequence = seq + 1;
                 State::apply_data_overlay_locked(*inode, op);
                 inode->data_ops.push_back(op);
                 inode->accounted_operation_metadata_bytes += op.metadata_charge;
                 metadata_admission->release = {};
                 state_->refresh_retained_owners_locked(*inode);
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
                 // Not a durability barrier: request publication of the durable
                 // prefix and return.
                 state_->request_data_publication(inode);
                 check_deadline(deadline, cancelled);
             });
}

void FuseFrontend::fsync(uint64_t inode_id) {
    dispatch(FuseOperationClass::sync,
             [this, inode_id](Clock::time_point deadline, std::atomic_bool& cancelled) {
                 auto inode = state_->resolve_inode(inode_id);

                 state_->wait_for_inode_durability(inode, deadline, cancelled);
                 const auto target = state_->durable_sequence(inode);
                 check_deadline(deadline, cancelled);

                 // Stronger than a local sync: waits for the durable prefix to be
                 // published (data and metadata committed).
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
                 // A read-only close must not publish another writer's data.
                 if (writable)
                     state_->request_data_publication(inode);
                 bool closed = false;
                 {
                     Lock lock(inode->mutex);
                     if (inode->open_handles) {
                         --inode->open_handles;
                         closed = true;
                     }
                     if (writable && inode->writable_handles)
                         --inode->writable_handles;
                 }
                 if (writable && closed) {
                     state_->open_writers.fetch_sub(1, std::memory_order_relaxed);
                     state_->data_cv.notify_all();
                 }
                 state_->reclaim_inode_if_quiescent(inode_id);
             });
}

std::pair<uint64_t, uint64_t> FuseFrontend::logical_capacity() const {
    return state_->fs.logical_capacity();
}

std::string FuseFrontend::path_for_inode(uint64_t id) const {
    auto inode = state_->resolve_inode(id);
    Lock lock(inode->mutex);
    return inode->current_path;
}

std::optional<uint64_t> FuseFrontend::inode_for_path(std::string_view path) {
    state_->refresh_namespace_if_stale();
    Lock lock(state_->namespace_mutex);
    auto found = state_->paths.find(canonical_path(path));
    if (found == state_->paths.end())
        return {};
    return found->second->id;
}

std::vector<FuseDirtyRange> FuseFrontend::dirty_ranges(uint64_t id) const {
    auto inode = state_->resolve_inode(id);
    Lock lock(inode->mutex);
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
        Lock lock(state_->namespace_queue_mutex);
        out.pending_namespace = state_->namespace_pending_locked();
    }
    // Publications cycle deferred -> queued -> active under data_queue_mutex,
    // so all counts are one sample under it, or an inode could be missed
    // between places. The inode list is copied first so data_queue_mutex is
    // never taken under namespace_mutex. pending_data counts deferred,
    // unconfirmed and enqueue-pending inodes, not active ones.
    std::vector<decltype(state_->inodes.begin()->second)> candidates;
    {
        Lock lock(state_->namespace_mutex);
        candidates.reserve(state_->inodes.size());
        for (const auto& [_, inode] : state_->inodes)
            candidates.push_back(inode);
    }
    {
        Lock lock(state_->data_queue_mutex);
        out.pending_data = state_->data_queue.size();
        out.pending_recovery_data = static_cast<size_t>(
            std::count_if(state_->data_queue.begin(), state_->data_queue.end(),
                          [](const State::DataQueueItem& item) { return item.recovered; }));
        for (const auto& inode : candidates) {
            Lock inode_lock(inode->mutex);
            if (!inode->published_path)
                ++out.detached_inode_count;
            if ((inode->data_deferred || inode->unconfirmed_data_entry ||
                 inode->data_enqueue_pending) &&
                !inode->data_queued && !inode->data_running)
                ++out.pending_data;
        }
        out.active_data = state_->active_data.load();
        out.active_recovery_data = state_->active_recovery_data.load();
    }
    out.inode_count = state_->inode_count.load(std::memory_order_relaxed);
    out.peak_inode_count = state_->peak_inode_count.load(std::memory_order_relaxed);
    out.reclaimed_inode_count =
        state_->reclaimed_inode_count.load(std::memory_order_relaxed);
    const auto diagnostics = this->diagnostics();
    out.timed_out_requests = diagnostics.timed_out_requests;
    out.merged_publications = diagnostics.merged_publications;
    out.data_publication_requests = diagnostics.data_publication_requests;
    out.data_publication_notifications_suppressed =
        diagnostics.data_publication_notifications_suppressed;
    out.spool_pressure_publication_sweeps = diagnostics.spool_pressure_publication_sweeps;
    out.data_publication_coalesced_queued = diagnostics.data_publication_coalesced_queued;
    out.data_publication_coalesced_running = diagnostics.data_publication_coalesced_running;
    out.data_publication_coalesced_unconfirmed =
        diagnostics.data_publication_coalesced_unconfirmed;
    out.data_publications_started = diagnostics.data_publications_started;
    out.data_publications_completed = diagnostics.data_publications_completed;
    out.data_publication_peak_active = diagnostics.data_publication_peak_active;
    out.data_publication_quanta = diagnostics.data_publication_quanta;
    out.data_publication_yields = diagnostics.data_publication_yields;
    out.data_publication_peak_inflight_bytes =
        diagnostics.data_publication_peak_inflight_bytes;
    out.data_publication_pipeline_limit_bytes =
        diagnostics.data_publication_pipeline_limit_bytes;
    out.data_publication_peak_pipeline_extents =
        diagnostics.data_publication_peak_pipeline_extents;
    out.data_closed_priority_selections = diagnostics.data_closed_priority_selections;
    out.data_retirement_priority_selections =
        diagnostics.data_retirement_priority_selections;
    out.open_publications = diagnostics.open_publications;
    out.peak_open_publications = diagnostics.peak_open_publications;
    out.publication_max_open_writers = diagnostics.publication_max_open_writers;
    out.data_publication_selections_under_writer_cap =
        diagnostics.data_publication_selections_under_writer_cap;
    out.data_publication_progress_events = diagnostics.data_publication_progress_events;
    out.data_publication_bytes_read = diagnostics.data_publication_bytes_read;
    out.data_publication_bytes_committed = diagnostics.data_publication_bytes_committed;
    out.data_publication_bytes_confirmed = diagnostics.data_publication_bytes_confirmed;
    out.data_publication_completed_spool_bytes_read =
        diagnostics.data_publication_completed_spool_bytes_read;
    out.data_publication_completed_source_bytes_read =
        diagnostics.data_publication_completed_source_bytes_read;
    out.data_publication_completed_reused_extents =
        diagnostics.data_publication_completed_reused_extents;
    out.data_publication_completed_put_extents =
        diagnostics.data_publication_completed_put_extents;
    out.data_overlay_read_queries = diagnostics.data_overlay_read_queries;
    out.data_overlay_ranges_examined = diagnostics.data_overlay_ranges_examined;
    out.data_overlay_descriptors_copied = diagnostics.data_overlay_descriptors_copied;
    out.retained_data_operations = diagnostics.retained_data_operations;
    out.retained_data_operation_bytes = diagnostics.retained_data_operation_bytes;
    out.retained_overlay_ranges = diagnostics.retained_overlay_ranges;
    out.retained_overlay_bytes = diagnostics.retained_overlay_bytes;
    out.retained_publication_operations = diagnostics.retained_publication_operations;
    out.retained_publication_operation_bytes =
        diagnostics.retained_publication_operation_bytes;
    out.operation_metadata_bytes = diagnostics.operation_metadata_bytes;
    out.peak_operation_metadata_bytes = diagnostics.peak_operation_metadata_bytes;
    out.operation_metadata_limit_bytes = diagnostics.operation_metadata_limit_bytes;
    out.operation_metadata_waits = diagnostics.operation_metadata_waits;
    out.retained_durability_tickets = diagnostics.retained_durability_tickets;
    out.data_publication_inflight_bytes = diagnostics.data_publication_inflight_bytes;
    out.backend_failures = diagnostics.backend_failures;
    out.durability_batches = diagnostics.durability_batches;
    out.durability_writes = diagnostics.durability_writes;
    out.namespace_operations_admitted = diagnostics.namespace_operations_admitted;
    out.namespace_operations_recovered = diagnostics.namespace_operations_recovered;
    out.namespace_publication_attempts = diagnostics.namespace_publication_attempts;
    out.namespace_publication_batches = diagnostics.namespace_publication_batches;
    out.namespace_operations_batched = diagnostics.namespace_operations_batched;
    out.namespace_operations_published = diagnostics.namespace_operations_published;
    out.namespace_operations_confirmed = diagnostics.namespace_operations_confirmed;
    out.journal_append_batches = diagnostics.journal_append_batches;
    out.journal_records_appended = diagnostics.journal_records_appended;
    out.journal_durability_barriers = diagnostics.journal_durability_barriers;
    out.spool_bytes = diagnostics.spool_bytes;
    out.spool_limit_bytes = diagnostics.spool_limit_bytes;
    out.spool_publish_rate_bytes_per_second =
        diagnostics.spool_publish_rate_bytes_per_second;
    out.spool_publish_rate_window_bytes = diagnostics.spool_publish_rate_window_bytes;
    out.spool_publish_rate_window_ms = diagnostics.spool_publish_rate_window_ms;
    out.spool_throttle_waits = diagnostics.spool_throttle_waits;
    out.spool_throttle_wait_ms = diagnostics.spool_throttle_wait_ms;
    out.pending_write_request_bytes = diagnostics.pending_write_request_bytes;
    out.peak_pending_write_request_bytes = diagnostics.peak_pending_write_request_bytes;
    out.pending_write_request_limit_bytes = diagnostics.pending_write_request_limit_bytes;
    out.extent_executor_workers = diagnostics.extent_executor_workers;
    out.extent_executor_queued = diagnostics.extent_executor_queued;
    out.extent_executor_active = diagnostics.extent_executor_active;
    out.extent_executor_peak_queued = diagnostics.extent_executor_peak_queued;
    out.extent_executor_peak_active = diagnostics.extent_executor_peak_active;
    out.extent_executor_submitted = diagnostics.extent_executor_submitted;
    return out;
}

FuseFrontendDiagnostics FuseFrontend::diagnostics() const noexcept {
    const auto executor = state_->fs.extent_executor_diagnostics();
    return {
        state_->timed_out_requests.load(std::memory_order_relaxed),
        state_->merged_publications.load(std::memory_order_relaxed),
        state_->data_publication_requests.load(std::memory_order_relaxed),
        state_->data_publication_notifications_suppressed.load(std::memory_order_relaxed),
        state_->spool_pressure_publication_sweeps.load(std::memory_order_relaxed),
        state_->data_publication_coalesced_queued.load(std::memory_order_relaxed),
        state_->data_publication_coalesced_running.load(std::memory_order_relaxed),
        state_->data_publication_coalesced_unconfirmed.load(std::memory_order_relaxed),
        state_->data_publications_started.load(std::memory_order_relaxed),
        state_->data_publications_completed.load(std::memory_order_relaxed),
        state_->data_publication_peak_active.load(std::memory_order_relaxed),
        state_->data_publication_quanta.load(std::memory_order_relaxed),
        state_->data_publication_yields.load(std::memory_order_relaxed),
        state_->data_publication_peak_inflight_bytes.load(std::memory_order_relaxed),
        state_->config.publication_pipeline_bytes,
        state_->data_publication_peak_pipeline_extents.load(std::memory_order_relaxed),
        state_->data_closed_priority_selections.load(std::memory_order_relaxed),
        state_->data_retirement_priority_selections.load(std::memory_order_relaxed),
        state_->open_publications.load(std::memory_order_relaxed),
        state_->peak_open_publications.load(std::memory_order_relaxed),
        state_->config.publication_max_open_writers,
        state_->data_publication_selections_under_writer_cap.load(std::memory_order_relaxed),
        state_->fs.write_progress().load(std::memory_order_relaxed),
        state_->data_publication_bytes_read.load(std::memory_order_relaxed),
        state_->data_publication_bytes_committed.load(std::memory_order_relaxed),
        state_->data_publication_bytes_confirmed.load(std::memory_order_relaxed),
        state_->data_publication_completed_spool_bytes_read.load(std::memory_order_relaxed),
        state_->data_publication_completed_source_bytes_read.load(std::memory_order_relaxed),
        state_->data_publication_completed_reused_extents.load(std::memory_order_relaxed),
        state_->data_publication_completed_put_extents.load(std::memory_order_relaxed),
        state_->data_overlay_read_queries.load(std::memory_order_relaxed),
        state_->data_overlay_ranges_examined.load(std::memory_order_relaxed),
        state_->data_overlay_descriptors_copied.load(std::memory_order_relaxed),
        state_->retained_data_operations.load(std::memory_order_relaxed),
        state_->retained_data_operation_bytes.load(std::memory_order_relaxed),
        state_->retained_overlay_ranges.load(std::memory_order_relaxed),
        state_->retained_overlay_bytes.load(std::memory_order_relaxed),
        state_->retained_publication_operations.load(std::memory_order_relaxed),
        state_->retained_publication_operation_bytes.load(std::memory_order_relaxed),
        state_->operation_metadata_bytes_diagnostic.load(std::memory_order_relaxed),
        state_->peak_operation_metadata_bytes.load(std::memory_order_relaxed),
        state_->config.max_operation_metadata_bytes,
        state_->operation_metadata_waits.load(std::memory_order_relaxed),
        state_->retained_durability_tickets.load(std::memory_order_relaxed),
        state_->publication_inflight_bytes_diagnostic.load(std::memory_order_relaxed),
        state_->backend_failures.load(std::memory_order_relaxed),
        state_->durability_batches.load(std::memory_order_relaxed),
        state_->durability_writes.load(std::memory_order_relaxed),
        state_->namespace_operations_admitted.load(std::memory_order_relaxed),
        state_->namespace_operations_recovered.load(std::memory_order_relaxed),
        state_->namespace_publication_attempts.load(std::memory_order_relaxed),
        state_->namespace_publication_batches.load(std::memory_order_relaxed),
        state_->namespace_operations_batched.load(std::memory_order_relaxed),
        state_->namespace_operations_published.load(std::memory_order_relaxed),
        state_->namespace_operations_confirmed.load(std::memory_order_relaxed),
        state_->journal_append_batches.load(std::memory_order_relaxed),
        state_->journal_records_appended.load(std::memory_order_relaxed),
        state_->journal_durability_barriers.load(std::memory_order_relaxed),
        state_->spool_bytes.load(std::memory_order_relaxed),
        state_->config.max_spool_bytes,
        state_->spool_publish_rate_diagnostic.load(std::memory_order_relaxed),
        state_->spool_publish_rate_window_bytes.load(std::memory_order_relaxed),
        state_->spool_publish_rate_window_ms.load(std::memory_order_relaxed),
        state_->spool_throttle_waits.load(std::memory_order_relaxed),
        state_->spool_throttle_wait_ns.load(std::memory_order_relaxed) / 1000000,
        state_->pending_write_request_bytes_diagnostic.load(std::memory_order_relaxed),
        state_->peak_pending_write_request_bytes.load(std::memory_order_relaxed),
        state_->config.max_pending_write_bytes,
        executor.workers,
        executor.queued,
        executor.active,
        executor.peak_queued,
        executor.peak_active,
        executor.submitted,
        state_->inode_count.load(std::memory_order_relaxed),
        state_->peak_inode_count.load(std::memory_order_relaxed),
        state_->reclaimed_inode_count.load(std::memory_order_relaxed),
        state_->refreshed_namespace_revision.load(std::memory_order_acquire),
        state_->fs.available_namespace_revision(),
        state_->parked_publications.load(std::memory_order_relaxed),
        state_->publication_retries_backed_off.load(std::memory_order_relaxed),
        state_->publications_retrying_persistently.load(std::memory_order_relaxed),
        state_->journal_recovery_skipped_frames.load(std::memory_order_relaxed),
        state_->journal_recovery_quarantined_bytes.load(std::memory_order_relaxed),
        state_->recovery_dropped_operations.load(std::memory_order_relaxed),
        state_->publications_abandoned.load(std::memory_order_relaxed),
    };
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

std::optional<BlockedNamespaceOperation> FuseFrontend::blocked_namespace_operation() const {
    Lock lock(state_->namespace_queue_mutex);
    if (!state_->namespace_blocked_op)
        return std::nullopt;
    const auto& op = *state_->namespace_blocked_op;
    BlockedNamespaceOperation out;
    out.sequence = op.sequence;
    out.kind = std::string(State::namespace_op_kind_name(op.kind));
    out.path = op.from;
    out.secondary_path = op.kind == State::NamespaceOp::Kind::rename ? op.to : std::string{};
    out.error_code = state_->namespace_blocked_error_code;
    out.error_message = state_->namespace_blocked_error_message;
    out.blocked_for = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - state_->namespace_blocked_since);
    return out;
}

bool FuseFrontend::skip_blocked_namespace_operation(uint64_t sequence) {
    Lock lock(state_->namespace_queue_mutex);
    if (!state_->namespace_blocked_op || state_->namespace_blocked_op->sequence != sequence)
        return false;
    state_->namespace_skip_requested_sequence = sequence;
    state_->namespace_cv.notify_all();
    return true;
}

std::vector<ParkedPublication> FuseFrontend::parked_publications() const {
    std::vector<ParkedPublication> out;
    const auto now = Clock::now();
    Lock lock(state_->namespace_mutex);
    for (const auto& [id, inode] : state_->inodes) {
        Lock inode_lock(inode->mutex);
        if (!inode->parked)
            continue;
        ParkedPublication item;
        item.inode = id;
        item.path = inode->current_path;
        item.error_code = inode->parked->error_code;
        item.error_message = inode->parked->error_message;
        item.attempts = inode->publication_retry.total_failures();
        item.failing_for = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - inode->publication_retry.first_failure());
        item.parked_for =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - inode->parked->since);
        for (const auto& op : inode->data_ops)
            if (op.kind == State::DataOp::Kind::write && op.sequence > inode->published_data_sequence)
                item.pending_bytes += op.length;
        out.push_back(std::move(item));
    }
    return out;
}

bool FuseFrontend::retry_parked_publication(uint64_t id) {
    std::shared_ptr<State::Inode> inode;
    {
        Lock lock(state_->namespace_mutex);
        auto found = state_->inodes.find(id);
        if (found == state_->inodes.end())
            return false;
        inode = found->second;
    }
    {
        Lock inode_lock(inode->mutex);
        if (!inode->parked)
            return false;
        inode->parked.reset();
        inode->publication_retry.reset();
        inode->data_deferred = true;
    }
    state_->parked_publications.fetch_sub(1, std::memory_order_relaxed);
    Log::info("FUSE data publication retry requested by operator inode=" + std::to_string(id));
    state_->admit_deferred();
    return true;
}

bool FuseFrontend::abandon_parked_publication(uint64_t id) {
    std::shared_ptr<State::Inode> inode;
    {
        Lock lock(state_->namespace_mutex);
        auto found = state_->inodes.find(id);
        if (found == state_->inodes.end())
            return false;
        inode = found->second;
    }
    {
        Lock inode_lock(inode->mutex);
        if (!inode->parked || inode->durability_pending)
            return false;
    }
    try {
        state_->abandon_data(inode, "abandoned by operator");
    } catch (const std::exception& error) {
        Log::warn("FUSE data publication abandon failed inode=" + std::to_string(id) + ": " +
                  error.what());
        return false;
    }
    {
        Lock inode_lock(inode->mutex);
        inode->parked.reset();
        inode->publication_retry.reset();
        inode->data_deferred = false;
    }
    state_->parked_publications.fetch_sub(1, std::memory_order_relaxed);
    Log::info("FUSE data publication abandoned by operator inode=" + std::to_string(id));
    return true;
}

std::vector<HydrationHint> FuseFrontend::hints() {
    std::vector<HydrationHint> out;
    Lock lock(state_->hint_mutex);
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
        // Prefetch for mount reads, which are loader work.
        result.frame_type = FrameType::loader;
        const auto end =
            std::min(hint.entry.extents.size(), hint.last + 1 + state_->config.read_ahead_extents);
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
    Lock lock(state_->hint_mutex);
    state_->hint_wake_callback = std::move(callback);
}

void FuseFrontend::interrupt_waits() {
    if (state_)
        state_->interrupt_waits();
}

void FuseFrontend::stop() {
    if (state_)
        state_->stop();
}

} // namespace macha
