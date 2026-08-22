// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_frontend.hpp"

#include "log.hpp"
#include "macos_unicode.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
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

} // namespace

struct FuseFrontend::State {
    struct DataOp {
        enum class Kind : uint8_t { write, truncate };
        Kind kind{Kind::write};
        uint64_t sequence{};
        uint64_t offset{};
        uint64_t length{};
        uint64_t spool_offset{};
        uint64_t size{};
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
        uint64_t published_data_sequence{};
        uint64_t requested_namespace_sequence{};
        std::vector<DataOp> data_ops;
        int spool_fd{-1};
        std::filesystem::path spool_path;
        uint64_t spool_end{};
        size_t open_handles{};
        bool data_queued{};
        bool data_running{};
        bool data_deferred{};
        std::optional<int> backend_error;

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
        std::vector<uint64_t> affected;
        // Namespace identities displaced by this operation (e.g. rename-over).
        // They remain valid for already-open handles but no longer own a published path.
        std::vector<uint64_t> removed;
    };

    struct BrokerTask {
        Clock::time_point deadline{};
        std::shared_ptr<std::atomic_bool> cancelled;
        std::function<void(Clock::time_point, std::atomic_bool&)> fn;
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

    mutable std::mutex namespace_mutex;
    std::map<std::string, std::shared_ptr<Inode>, std::less<>> paths;
    std::map<uint64_t, std::shared_ptr<Inode>> inodes;
    uint64_t next_inode{2};
    uint64_t next_namespace_sequence{1};
    std::mutex namespace_apply_mutex;

    std::mutex namespace_queue_mutex;
    std::condition_variable_any namespace_cv;
    std::deque<NamespaceOp> namespace_queue;
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

    std::mutex refresh_mutex;
    uint64_t refreshed_metadata_generation{};

    std::atomic_uint64_t timed_out_requests{};
    std::atomic_uint64_t merged_publications{};
    std::atomic_uint64_t backend_failures{};

    explicit State(FileSystem& filesystem, FuseConfig policy)
        : fs(filesystem), config(std::move(policy)),
          spool_dir(fs.node().config().state_path / "fuse-spool") {}

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

    size_t namespace_pending_locked() const {
        return namespace_queue.size() + (namespace_inflight ? 1U : 0U);
    }

    bool namespace_capacity_available() {
        std::lock_guard lock(namespace_queue_mutex);
        return namespace_pending_locked() < config.max_pending_operations;
    }

    void enqueue_namespace(NamespaceOp operation) {
        {
            std::lock_guard lock(namespace_queue_mutex);
            if (namespace_pending_locked() >= config.max_pending_operations)
                throw FsError(EAGAIN, "FUSE namespace publication queue saturated");
            namespace_queue.push_back(std::move(operation));
        }
        namespace_cv.notify_one();
    }

    int ensure_spool_locked(const std::shared_ptr<Inode>& inode) {
        if (inode->spool_fd >= 0)
            return inode->spool_fd;
        std::filesystem::create_directories(spool_dir);
        inode->spool_path = spool_dir / ("inode-" + std::to_string(inode->id) + ".spool");
        int fd = ::open(inode->spool_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0)
            throw FsError(errno, "cannot open FUSE spool");
        inode->spool_fd = fd;
        auto end = ::lseek(fd, 0, SEEK_END);
        if (end < 0)
            throw FsError(errno, "cannot seek FUSE spool");
        inode->spool_end = static_cast<uint64_t>(end);
        return fd;
    }

    void request_data_publication(const std::shared_ptr<Inode>& inode) {
        bool enqueue = false;
        {
            std::lock_guard inode_lock(inode->mutex);
            if (inode->data_ops.empty())
                return;
            inode->requested_data_sequence = inode->data_ops.back().sequence;
            inode->requested_namespace_sequence = inode->namespace_sequence;
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
        if (inode->data_queued || inode->data_running)
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
            if (!inode->data_deferred || inode->data_queued || inode->data_running)
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
    }

    void namespace_success(const NamespaceOp& op) {
        std::lock_guard lock(namespace_mutex);
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

            bool complete = false;
            std::chrono::milliseconds backoff{50};
            while (!complete && !stop.stop_requested() && !stopping.load()) {
                try {
                    std::lock_guard backend(publication_mutex);
                    apply_namespace_backend(op);
                    namespace_success(op);
                    complete = true;
                } catch (const std::exception& e) {
                    ++backend_failures;
                    if (!retryable_backend_error(e)) {
                        int error = EIO;
                        if (const auto* fs_error = dynamic_cast<const FsError*>(&e))
                            error = fs_error->code();
                        auto errored = op.affected;
                        errored.insert(errored.end(), op.removed.begin(), op.removed.end());
                        mark_backend_error(errored, error);
                        Log::error("FUSE async namespace publication failed permanently seq=" +
                                   std::to_string(op.sequence) + " error=" + e.what());
                        complete = true;
                    } else {
                        Log::debug("FUSE async namespace publication retry seq=" +
                                   std::to_string(op.sequence) + " error=" + e.what());
                        std::this_thread::sleep_for(backoff);
                        backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
                    }
                }
            }

            if (complete) {
                published_namespace_sequence.store(op.sequence, std::memory_order_release);
                publication_cv.notify_all();
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
        int spool_fd{-1};
    };

    DataSnapshot snapshot_data(const std::shared_ptr<Inode>& inode) {
        std::lock_guard lock(inode->mutex);
        DataSnapshot snapshot;
        snapshot.target_sequence = inode->requested_data_sequence;
        snapshot.required_namespace_sequence = inode->requested_namespace_sequence;
        snapshot.published_path = inode->published_path;
        snapshot.spool_fd = inode->spool_fd;
        for (const auto& op : inode->data_ops) {
            if (op.sequence <= snapshot.target_sequence)
                snapshot.operations.push_back(op);
        }
        return snapshot;
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
            std::lock_guard lock(inode->mutex);
            inode->data_ops.erase(
                std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                               [&](const DataOp& op) { return op.sequence <= snapshot.target_sequence; }),
                inode->data_ops.end());
            inode->published_data_sequence =
                std::max(inode->published_data_sequence, snapshot.target_sequence);
            return;
        }

        auto writer = fs.open_write(*snapshot.published_path, false, config.write_through_cache);
        constexpr size_t chunk_size = 256 * 1024;
        Bytes buffer(chunk_size);
        for (const auto& op : snapshot.operations) {
            if (stopping.load())
                throw FsError(EINTR, "FUSE publication stopping");
            if (op.kind == DataOp::Kind::truncate) {
                writer->truncate(op.size);
                continue;
            }
            uint64_t done = 0;
            while (done < op.length) {
                const auto chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), op.length - done));
                if (pread_exact(snapshot.spool_fd, {buffer.data(), chunk}, op.spool_offset + done) != chunk)
                    throw FsError(EIO, "short read from FUSE write spool");
                if (writer->write(op.offset + done, {buffer.data(), chunk}) != chunk)
                    throw FsError(EIO, "short replay into Macha write handle");
                done += chunk;
            }
        }
        // Extent creation may proceed concurrently, but metadata publication
        // is one shared namespace transaction. Serialise the final commit with
        // namespace publication and other FUSE data commits to avoid a fan-out
        // of large metadata CAS attempts fighting over the same snapshot.
        {
            std::lock_guard backend(publication_mutex);
            writer->commit();
        }
        auto committed = writer->committed_entry();

        std::lock_guard lock(inode->mutex);
        inode->base = std::move(committed);
        inode->data_ops.erase(
            std::remove_if(inode->data_ops.begin(), inode->data_ops.end(),
                           [&](const DataOp& op) { return op.sequence <= snapshot.target_sequence; }),
            inode->data_ops.end());
        inode->published_data_sequence =
            std::max(inode->published_data_sequence, snapshot.target_sequence);
        if (inode->data_ops.empty() && inode->spool_fd >= 0) {
            (void)::ftruncate(inode->spool_fd, 0);
            inode->spool_end = 0;
        }
    }

    bool data_slot_available() const {
        const bool recent_foreground = config.publication_quiet.count() > 0 &&
            fs.interactive_idle_for() < config.publication_quiet;
        const bool foreground_busy =
            open_writers.load(std::memory_order_relaxed) > 0 || recent_foreground;
        const auto limit = foreground_busy ? config.foreground_commit_workers : config.commit_workers;
        return active_data.load(std::memory_order_relaxed) < limit;
    }

    void data_loop(std::stop_token stop) {
        while (!stop.stop_requested() && !stopping.load()) {
            std::shared_ptr<Inode> inode;
            {
                std::unique_lock lock(data_queue_mutex);
                // Re-evaluate periodically because foreground activity may go
                // quiet without another publication-queue event to wake us.
                data_cv.wait_for(lock, stop, std::chrono::milliseconds(50), [&] {
                    return stopping.load() || (!data_queue.empty() && data_slot_available());
                });
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
                if (retry || still_requested)
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

    void initialise_namespace() {
        auto view = fs.local_snapshot_view();
        const auto& snapshot = *view.snapshot;
        std::filesystem::create_directories(spool_dir);
        std::lock_guard lock(namespace_mutex);
        for (const auto& [path, entry] : snapshot.entries) {
            auto inode = std::make_shared<Inode>();
            inode->id = path == "/" ? 1 : next_inode++;
            inode->base = entry;
            inode->visible = entry;
            inode->current_path = path;
            inode->published_path = path;
            paths[canonical_path(path)] = inode;
            inodes[inode->id] = std::move(inode);
        }
        if (!paths.contains(canonical_path("/")))
            throw std::runtime_error("FUSE frontend cannot initialise without namespace root");
        refreshed_metadata_generation = view.generation;
    }

    void refresh_namespace_if_stale() {
        // Namespace synchronisation is demand-driven. No background timer wakes
        // merely to ask whether metadata changed: a FUSE operation that actually
        // needs namespace state performs one cheap generation comparison and only
        // adopts the shared decoded snapshot when a newer generation is known.
        // Serialising refreshes prevents concurrent lookup workers from rebuilding
        // the same generation more than once.
        std::lock_guard refresh_lock(refresh_mutex);
        {
            std::lock_guard queue_lock(namespace_queue_mutex);
            // Local FUSE namespace operations are already reflected optimistically
            // in paths/inodes. Do not race a backend publication with a snapshot
            // adoption; a subsequent namespace-facing request will retry.
            if (namespace_inflight || !namespace_queue.empty())
                return;
        }
        if (fs.known_metadata_generation() <= refreshed_metadata_generation)
            return;

        MetadataSnapshotView view;
        try {
            view = fs.local_snapshot_view();
        } catch (const std::exception& e) {
            // The existing coherent namespace view remains usable when a newer
            // distributed generation is temporarily unreachable. A later FUSE
            // namespace operation will retry without any background polling.
            Log::debug("FUSE on-demand namespace refresh skipped: " + std::string(e.what()));
            return;
        }
        if (view.generation <= refreshed_metadata_generation)
            return;
        const auto& snapshot = *view.snapshot;
        std::lock_guard lock(namespace_mutex);
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
            inode->published_path = path;
            if (inode->data_ops.empty()) {
                inode->base = entry;
                const auto visible_size = inode->visible.size;
                inode->visible = entry;
                if (inode->requested_data_sequence > inode->published_data_sequence)
                    inode->visible.size = visible_size;
            }
        }

        for (auto it = paths.begin(); it != paths.end();) {
            if (it->first == canonical_path("/") || seen.contains(it->first)) {
                ++it;
                continue;
            }
            auto inode = it->second;
            std::lock_guard inode_lock(inode->mutex);
            if (inode->open_handles || !inode->data_ops.empty()) {
                ++it;
                continue;
            }
            inode->published_path.reset();
            it = paths.erase(it);
        }
        refreshed_metadata_generation = view.generation;
    }

    void start() {
        initialise_namespace();

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

FsEntry FuseFrontend::getattr(std::string_view path) {
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
        return inode->visible;
    });
}

std::vector<std::pair<std::string, FsEntry>> FuseFrontend::readdir(std::string_view path) {
    const auto requested = canonical_path(path);
    return dispatch(FuseOperationClass::lookup, [this, requested](Clock::time_point deadline,
                                                                  std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        state_->refresh_namespace_if_stale();
        check_deadline(deadline, cancelled);
        std::vector<std::pair<std::string, FsEntry>> out;
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
            out.push_back({base_name(inode->current_path), inode->visible});
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
            inode->visible.ctime_ns = inode->visible.mtime_ns = wall_time_ns();
            inode->base = inode->visible;
            inode->current_path = requested;
            inode->namespace_sequence = state_->next_namespace_sequence++;
            state_->paths[requested] = inode;
            state_->inodes[inode->id] = inode;
            op = {State::NamespaceOp::Kind::mkdir, inode->namespace_sequence, requested, {}, false,
                  mode, uid, gid, false, false, 0, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
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
            inode->namespace_sequence = state_->next_namespace_sequence++;
            state_->paths.erase(requested);
            op = {State::NamespaceOp::Kind::rmdir, inode->namespace_sequence, requested, {}, false,
                  0, 0, 0, false, false, 0, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
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
            inode->namespace_sequence = state_->next_namespace_sequence++;
            inode->current_path.clear();
            state_->paths.erase(requested);
            op = {State::NamespaceOp::Kind::unlink, inode->namespace_sequence, requested, {}, false,
                  0, 0, 0, false, false, 0, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
    });
}

void FuseFrontend::rename(std::string_view from, std::string_view to, bool noreplace) {
    const auto source = canonical_path(from);
    const auto destination = canonical_path(to);
    dispatch(FuseOperationClass::namespace_mutation,
             [this, source, destination, noreplace](Clock::time_point deadline,
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
            auto root = state_->resolve_locked(source);
            if (source == "/")
                throw FsError(EBUSY, "cannot rename root");
            state_->require_parent_locked(destination);
            if (destination.starts_with(source + "/"))
                throw FsError(EINVAL, "cannot move directory under itself");
            std::vector<uint64_t> displaced;
            auto existing = state_->paths.find(destination);
            if (existing != state_->paths.end()) {
                if (noreplace)
                    throw FsError(EEXIST, "destination exists");
                std::lock_guard source_lock(root->mutex);
                std::lock_guard destination_lock(existing->second->mutex);
                if (root->visible.type != existing->second->visible.type)
                    throw FsError(root->visible.type == EntryType::directory ? ENOTDIR : EISDIR,
                                  "rename type mismatch");
                if (existing->second->visible.type == EntryType::directory) {
                    for (const auto& [_, candidate] : state_->paths) {
                        if (candidate == existing->second || candidate == root) continue;
                        std::lock_guard candidate_lock(candidate->mutex);
                        if (parent_path(candidate->current_path) == existing->second->current_path)
                            throw FsError(ENOTEMPTY, "destination directory not empty");
                    }
                }
                displaced.push_back(existing->second->id);
                existing->second->current_path.clear();
                existing->second->namespace_sequence = state_->next_namespace_sequence;
                state_->paths.erase(existing);
            }

            std::vector<std::shared_ptr<State::Inode>> affected_inodes;
            for (const auto& [_, inode] : state_->paths) {
                std::lock_guard inode_lock(inode->mutex);
                if (under_path(inode->current_path, source))
                    affected_inodes.push_back(inode);
            }
            const auto sequence = state_->next_namespace_sequence++;
            std::vector<uint64_t> affected;
            for (const auto& inode : affected_inodes) {
                std::lock_guard inode_lock(inode->mutex);
                auto old = inode->current_path;
                state_->paths.erase(canonical_path(old));
                inode->current_path = destination + old.substr(source.size());
                inode->namespace_sequence = sequence;
                affected.push_back(inode->id);
            }
            for (const auto& inode : affected_inodes) {
                std::lock_guard inode_lock(inode->mutex);
                state_->paths[canonical_path(inode->current_path)] = inode;
            }
            check_deadline(deadline, cancelled);
            op = {State::NamespaceOp::Kind::rename, sequence, source, destination, noreplace,
                  0, 0, 0, false, false, 0, std::move(affected), std::move(displaced)};
        }
        state_->enqueue_namespace(std::move(op));
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
            inode->visible.mode = mode & 07777;
            inode->visible.ctime_ns = wall_time_ns();
            ++inode->visible.version;
            inode->namespace_sequence = state_->next_namespace_sequence++;
            op = {State::NamespaceOp::Kind::chmod, inode->namespace_sequence, requested, {}, false,
                  mode, 0, 0, false, false, 0, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
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
            if (set_uid) inode->visible.uid = uid;
            if (set_gid) inode->visible.gid = gid;
            inode->visible.ctime_ns = wall_time_ns();
            ++inode->visible.version;
            inode->namespace_sequence = state_->next_namespace_sequence++;
            op = {State::NamespaceOp::Kind::chown, inode->namespace_sequence, requested, {}, false,
                  0, uid, gid, set_uid, set_gid, 0, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
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
            inode->visible.mtime_ns = mtime_ns;
            inode->visible.ctime_ns = wall_time_ns();
            ++inode->visible.version;
            inode->namespace_sequence = state_->next_namespace_sequence++;
            op = {State::NamespaceOp::Kind::utimens, inode->namespace_sequence, requested, {}, false,
                  0, 0, 0, false, false, mtime_ns, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
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
        std::lock_guard inode_lock(inode->mutex);
        if (inode->visible.type != EntryType::file)
            throw FsError(EISDIR, "directory");
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "asynchronous backend error");
        if (truncate_on_open) {
            const auto seq = inode->next_data_sequence++;
            inode->data_ops.push_back({State::DataOp::Kind::truncate, seq, 0, 0, 0, 0});
            inode->visible.size = 0;
        }
        ++inode->open_handles;
        if (writable)
            state_->open_writers.fetch_add(1, std::memory_order_relaxed);
        return FuseOpenHandle{inode->id, readable, writable, append};
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
            inode->visible.ctime_ns = inode->visible.mtime_ns = wall_time_ns();
            inode->base = inode->visible;
            inode->current_path = requested;
            inode->namespace_sequence = state_->next_namespace_sequence++;
            inode->open_handles = 1;
            if (writable)
                state_->open_writers.fetch_add(1, std::memory_order_relaxed);
            state_->paths[requested] = inode;
            state_->inodes[inode->id] = inode;
            op = {State::NamespaceOp::Kind::create, inode->namespace_sequence, requested, {}, false,
                  mode, uid, gid, false, false, 0, {inode->id}, {}};
        }
        state_->enqueue_namespace(std::move(op));
        return FuseOpenHandle{inode->id, readable, writable, append};
    });
}

size_t FuseFrontend::read(uint64_t inode_id, uint64_t offset, std::span<uint8_t> output) {
    return dispatch(FuseOperationClass::read,
                    [this, inode_id, offset, output](Clock::time_point deadline,
                                                     std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        FsEntry base;
        FsEntry visible;
        std::vector<State::DataOp> operations;
        int spool_fd = -1;
        std::string logical_path;
        {
            std::lock_guard lock(inode->mutex);
            if (inode->visible.type != EntryType::file)
                throw FsError(EISDIR, "directory");
            base = inode->base;
            visible = inode->visible;
            operations = inode->data_ops;
            spool_fd = inode->spool_fd;
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
            auto reader = state_->fs.open_read(base, logical_path, false, FrameType::read_ahead);
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
            if (spool_fd < 0)
                throw FsError(EIO, "missing FUSE write spool");
            const auto write_begin = op.offset;
            const auto write_end = op.offset + op.length;
            const auto copy_begin = std::max<uint64_t>(offset, write_begin);
            const auto copy_end = std::min<uint64_t>(offset + count, write_end);
            if (copy_begin < copy_end) {
                const auto n = static_cast<size_t>(copy_end - copy_begin);
                const auto spool = op.spool_offset + (copy_begin - write_begin);
                if (pread_exact(spool_fd,
                                {output.data() + static_cast<size_t>(copy_begin - offset), n}, spool) != n)
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
                std::lock_guard hint_lock(state_->hint_mutex);
                state_->hint_states[inode_id] =
                    State::HintState{base, first, last, Clock::now() + state_->config.hint_lifetime};
            }
        }
        return count;
    });
}

size_t FuseFrontend::write(uint64_t inode_id, uint64_t offset, std::span<const uint8_t> data,
                           bool append) {
    Bytes owned(data.begin(), data.end());
    return dispatch(FuseOperationClass::write,
                    [this, inode_id, offset, append, owned = std::move(owned)](
                        Clock::time_point deadline, std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        std::lock_guard lock(inode->mutex);
        check_deadline(deadline, cancelled);
        if (inode->visible.type != EntryType::file)
            throw FsError(EISDIR, "directory");
        if (inode->backend_error)
            throw FsError(*inode->backend_error, "asynchronous backend error");
        const auto target = append ? inode->visible.size : offset;
        const auto fd = state_->ensure_spool_locked(inode);
        const auto spool_offset = inode->spool_end;
        if (pwrite_exact(fd, owned, spool_offset) != owned.size())
            throw FsError(errno ? errno : EIO, "short FUSE spool write");
        check_deadline(deadline, cancelled);
        const auto seq = inode->next_data_sequence++;
        inode->data_ops.push_back({State::DataOp::Kind::write, seq, target,
                                   static_cast<uint64_t>(owned.size()), spool_offset, 0});
        inode->spool_end += owned.size();
        inode->visible.size = std::max<uint64_t>(inode->visible.size, target + owned.size());
        inode->visible.mtime_ns = inode->visible.ctime_ns = wall_time_ns();
        return owned.size();
    });
}

void FuseFrontend::truncate(uint64_t inode_id, uint64_t size) {
    dispatch(FuseOperationClass::write,
             [this, inode_id, size](Clock::time_point deadline, std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        std::lock_guard lock(inode->mutex);
        check_deadline(deadline, cancelled);
        if (inode->visible.type != EntryType::file)
            throw FsError(EISDIR, "directory");
        const auto seq = inode->next_data_sequence++;
        inode->data_ops.push_back({State::DataOp::Kind::truncate, seq, 0, 0, 0, size});
        inode->visible.size = size;
        inode->visible.mtime_ns = inode->visible.ctime_ns = wall_time_ns();
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
        state_->request_data_publication(inode);
    });
}

void FuseFrontend::fsync(uint64_t inode_id) {
    dispatch(FuseOperationClass::sync,
             [this, inode_id](Clock::time_point deadline, std::atomic_bool& cancelled) {
        auto inode = state_->resolve_inode(inode_id);
        int fd = -1;
        {
            std::lock_guard lock(inode->mutex);
            if (inode->spool_fd >= 0)
                fd = ::dup(inode->spool_fd);
        }
        if (fd >= 0) {
            int rc;
            do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
            const int saved = errno;
            ::close(fd);
            if (rc != 0)
                throw FsError(saved, "FUSE local spool fsync failed");
        }
        check_deadline(deadline, cancelled);
        state_->request_data_publication(inode);
    });
}

void FuseFrontend::release(uint64_t inode_id, bool writable) {
    dispatch(FuseOperationClass::lifecycle,
             [this, inode_id, writable](Clock::time_point deadline, std::atomic_bool& cancelled) {
        check_deadline(deadline, cancelled);
        auto inode = state_->resolve_inode(inode_id);
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
        out.pending_namespace = state_->namespace_queue.size() + (state_->namespace_inflight ? 1 : 0);
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
            if (inode->data_deferred && !inode->data_queued && !inode->data_running)
                ++out.pending_data;
        }
    }
    out.active_data = state_->active_data.load();
    out.timed_out_requests = state_->timed_out_requests.load();
    out.merged_publications = state_->merged_publications.load();
    out.backend_failures = state_->backend_failures.load();
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

void FuseFrontend::stop() {
    if (state_)
        state_->stop();
}

} // namespace macha
