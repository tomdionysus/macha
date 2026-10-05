// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "metadata/metadata_server.hpp"
#include "cluster/data_work.hpp"
#include "contract/thread_safety.hpp"
#include "cluster/membership.hpp"
#include "cluster/distributed_store.hpp"
#include "metadata/metadata_manager.hpp"
#include "metadata/namespace_control_store.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <stdexcept>
#include <vector>
#include <thread>
namespace macha {
class FsError : public std::runtime_error {
    int c_;

  public:
    FsError(int c, const std::string& s) : std::runtime_error(s), c_(c) {}
    int code() const {
        return c_;
    }
};
class FileSystem;
class PlaybackTracker;

// What a tree-backed namespace refers to: the DATA extents its entries name
// and the tree's own nodes, each sorted and distinct.
struct NamespaceReferences {
    std::vector<ObjectId> extents;
    std::vector<ObjectId> nodes;
};

struct MaintenanceObjects {
    // Sorted, unique vectors: far smaller than a tree node per extent at
    // millions of objects.
    std::vector<ObjectId> live;
    std::vector<GarbageRef> garbage;
    // The namespace tree's own nodes, control objects; empty for an inline
    // namespace.
    std::vector<ObjectId> namespace_nodes;
    uint64_t metadata_generation{};
    RetentionClock observed_mutations;
    size_t entries{};
    size_t extents{};
};

struct WriteHandleDiagnostics {
    uint64_t id{};
    uint64_t logical_size{};
    uint64_t staged_size{};
    size_t buffer_size{};
    bool sequential{};
    bool temp_open{};
    uint64_t temp_size{};
    size_t append_tail_fetches{};
    size_t materialize_source_reads{};
    size_t new_extent_puts{};
    size_t rebuild_reused_extents{};
    size_t rebuild_put_extents{};
    size_t pending_extent_puts{};
    size_t peak_pending_extent_puts{};
    FrameType work_frame_type{FrameType::loader};
    uint64_t work_quantum_bytes{};
    uint64_t materialize_source_bytes{};
    size_t materialize_steps{};
    uint64_t rebuild_source_bytes{};
    size_t rebuild_steps{};
};

struct WritePreparation {
    bool ready{};
    uint64_t bytes_processed{};
};

struct ExtentExecutorDiagnostics {
    uint64_t workers{};
    uint64_t queued{};
    uint64_t active{};
    uint64_t peak_queued{};
    uint64_t peak_active{};
    uint64_t submitted{};
};

struct FilesystemNamespaceMutation {
    enum class Kind : uint8_t { mkdir, create, rmdir, unlink, rename, chmod, chown, utimens };
    Kind kind{};
    std::string from;
    std::string to;
    bool noreplace{};
    uint32_t mode{};
    uint32_t uid{};
    uint32_t gid{};
    bool set_uid{};
    bool set_gid{};
    int64_t mtime_ns{};
};

struct FilesystemNamespaceBatchResult {
    MetadataRecord record;
    size_t applied{};
    std::vector<std::optional<FsEntry>> entries;
    std::optional<int> failure_code;
    std::string failure_message;
};

class ReadHandle {
    DistributedStore& s_;
    // e_, playback_ and playback_session_ are fixed at construction.
    const FsEntry e_;
    PlaybackTracker* const playback_{};
    uint64_t playback_session_{};
    std::atomic<FrameType> frame_type_{FrameType::read_ahead};
    // Held across extent fetches from the store.
    IoMutex m_;
    uint64_t last_ MACHA_GUARDED_BY(m_){};
    size_t cached_index_ MACHA_GUARDED_BY(m_){static_cast<size_t>(-1)};
    DistributedStore::ObjectData cached_extent_ MACHA_GUARDED_BY(m_);
    const Bytes& extent(size_t, Clock::time_point, std::atomic_bool*) MACHA_REQUIRES(m_);

  public:
    ReadHandle(DistributedStore&, FsEntry, PlaybackTracker* = nullptr,
               std::string path = {}, FrameType frame_type = FrameType::read_ahead);
    ~ReadHandle();
    void promote(FrameType) noexcept;
    size_t read(uint64_t, std::span<uint8_t>, Clock::time_point deadline = {},
                std::atomic_bool* cancelled = nullptr);
};
enum class WriteDurability : uint8_t {
    immediate,
    publication_generation,
};

// One file generation being published from durable local input: its extents
// are staged provisionally, drained, and committed to the namespace. Every
// operation is thread_safe; a publication drives one writer from one worker
// at a time.
class PublicationWriter {
  public:
    virtual ~PublicationWriter() = default;

    // Stage the existing generation a write at `offset` overlaps, within
    // `byte_budget` (zero: to completion); !ready asks the caller to yield.
    static constexpr Waits prepare_write_waits = Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety prepare_write_safety = ThreadSafety::thread_safe;
    virtual WritePreparation prepare_write(uint64_t offset, uint64_t byte_budget) = 0;

    static constexpr Waits prepare_commit_waits = Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety prepare_commit_safety = ThreadSafety::thread_safe;
    virtual WritePreparation prepare_commit(uint64_t byte_budget) = 0;

    static constexpr Waits write_waits = Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety write_safety = ThreadSafety::thread_safe;
    virtual size_t write(uint64_t offset, std::span<const uint8_t>) = 0;

    static constexpr Waits truncate_waits = Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety truncate_safety = ThreadSafety::thread_safe;
    virtual void truncate(uint64_t size) = 0;

    // Waits for every staged extent put; a failure here is retryable and
    // leaves the writer usable.
    static constexpr Waits drain_staging_waits = Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety drain_staging_safety = ThreadSafety::thread_safe;
    virtual void drain_staging() = 0;

    // The mtime the next commit publishes.
    static constexpr Waits set_committed_mtime_waits = Waits::locks;
    static constexpr ThreadSafety set_committed_mtime_safety = ThreadSafety::thread_safe;
    virtual void set_committed_mtime(int64_t mtime_ns) = 0;

    static constexpr Waits commit_waits = Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety commit_safety = ThreadSafety::thread_safe;
    virtual void commit() = 0;

    // The entry the last commit published.
    static constexpr Waits committed_entry_waits = Waits::locks;
    static constexpr ThreadSafety committed_entry_safety = ThreadSafety::thread_safe;
    virtual FsEntry committed_entry() const = 0;

    static constexpr Waits diagnostics_waits = Waits::locks;
    static constexpr ThreadSafety diagnostics_safety = ThreadSafety::thread_safe;
    virtual WriteHandleDiagnostics diagnostics() const = 0;
};

// Where the FUSE frontend publishes spooled file data. FileSystem is the
// production target.
class PublicationTarget {
  public:
    virtual ~PublicationTarget() = default;

    // Opens a writer for one generation of the file at `path`; ENOENT when
    // the path is absent. `pipeline_bytes` bounds provisional extent data in
    // flight.
    static constexpr Waits open_publication_waits = Waits::network | Waits::locks;
    static constexpr ThreadSafety open_publication_safety = ThreadSafety::thread_safe;
    virtual std::shared_ptr<PublicationWriter> open_publication(const std::string& path,
                                                                bool cache_puts,
                                                                uint64_t pipeline_bytes,
                                                                DataWorkContext) = 0;

    // Advances whenever the nodes or the storage a publication depends on
    // change: a publication that kept failing is worth another attempt. An
    // atomic read.
    virtual uint64_t reachability_epoch() const noexcept { return 0; }
};

class WriteHandle final : public PublicationWriter {
    friend class FileSystem;
class PlaybackTracker;

    // Held across the temp file's I/O, extent puts and fetches, durability
    // barriers and the metadata commit.
    mutable IoMutex m_;
    FileSystem& fs_;
    // Written by FileSystem under fs_.open_writes_mutex_; read through
    // current_path(), which takes it.
    std::string path_;
    FsEntry base_ MACHA_GUARDED_BY(m_);
    uint64_t expected_ MACHA_GUARDED_BY(m_){};
    bool sequential_ MACHA_GUARDED_BY(m_){};
    bool dirty_ MACHA_GUARDED_BY(m_){};
    const bool cache_puts_{};
    const WriteDurability durability_{WriteDurability::immediate};
    const DataWorkContext work_context_{};
    DistributedStore::DurabilityBatch durability_batch_ MACHA_GUARDED_BY(m_);
    struct StagedExtentResult {
        ExtentRef extent;
        DistributedStore::DurabilityBatch durability;
        std::chrono::milliseconds elapsed{};
    };
    struct PendingExtent {
        uint64_t bytes{};
        uint64_t offset{};
        bool cache_put{};
        std::shared_ptr<const Bytes> payload;
        std::future<StagedExtentResult> result;
        std::optional<RetainedMemoryLedger::Lease> memory;
    };
    std::deque<PendingExtent> pending_extents_ MACHA_GUARDED_BY(m_);
    uint64_t pending_extent_bytes_ MACHA_GUARDED_BY(m_){};
    const uint64_t publication_pipeline_bytes_{};
    size_t peak_pending_extents_ MACHA_GUARDED_BY(m_){};
    uint64_t logical_ MACHA_GUARDED_BY(m_){};
    uint64_t staged_ MACHA_GUARDED_BY(m_){};
    std::vector<ExtentRef> extents_ MACHA_GUARDED_BY(m_);
    Bytes buffer_ MACHA_GUARDED_BY(m_);
    std::optional<RetainedMemoryLedger::Lease> buffer_memory_ MACHA_GUARDED_BY(m_);
    std::optional<ExtentRef> append_tail_ MACHA_GUARDED_BY(m_);
    int temp_ MACHA_GUARDED_BY(m_){-1};
    std::filesystem::path temp_path_ MACHA_GUARDED_BY(m_);
    const uint64_t diagnostic_id_{};
    uint64_t diagnostic_write_sequence_ MACHA_GUARDED_BY(m_){};
    size_t diagnostic_completed_extents_ MACHA_GUARDED_BY(m_){};
    size_t append_tail_fetches_ MACHA_GUARDED_BY(m_){};
    size_t materialize_source_reads_ MACHA_GUARDED_BY(m_){};
    uint64_t materialize_source_bytes_ MACHA_GUARDED_BY(m_){};
    size_t materialize_steps_ MACHA_GUARDED_BY(m_){};
    bool materializing_ MACHA_GUARDED_BY(m_){};
    uint64_t materialize_offset_ MACHA_GUARDED_BY(m_){};
    size_t materialize_extent_index_ MACHA_GUARDED_BY(m_){};
    size_t new_extent_puts_ MACHA_GUARDED_BY(m_){};
    size_t rebuild_reused_extents_ MACHA_GUARDED_BY(m_){};
    size_t rebuild_put_extents_ MACHA_GUARDED_BY(m_){};
    uint64_t rebuild_source_bytes_ MACHA_GUARDED_BY(m_){};
    size_t rebuild_steps_ MACHA_GUARDED_BY(m_){};
    bool rebuilding_ MACHA_GUARDED_BY(m_){};
    bool rebuild_prepared_ MACHA_GUARDED_BY(m_){};
    // Edit a canonical committed manifest as a sparse changed-range overlay;
    // unchanged extents stay references and are never copied to the temp file.
    bool sparse_overlay_ MACHA_GUARDED_BY(m_){};
    std::optional<int64_t> committed_mtime_ MACHA_GUARDED_BY(m_);
    struct ChangedRange {
        uint64_t begin{};
        uint64_t end{};
    };
    std::vector<ChangedRange> changed_ranges_ MACHA_GUARDED_BY(m_);
    uint64_t rebuild_offset_ MACHA_GUARDED_BY(m_){};
    size_t rebuild_index_ MACHA_GUARDED_BY(m_){};
    std::vector<ExtentRef> rebuild_handle_extents_ MACHA_GUARDED_BY(m_);
    struct DiagnosticWriteRange {
        uint64_t sequence{};
        uint64_t offset{};
        size_t length{};
        Hash256 hash{};
    };
    std::map<std::pair<uint64_t, size_t>, std::pair<uint64_t, Hash256>> diagnostic_exact_writes_
        MACHA_GUARDED_BY(m_);
    // Trace-only; bounded so it never records every write of a bulk transfer.
    std::deque<DiagnosticWriteRange> diagnostic_writes_ MACHA_GUARDED_BY(m_);
    static constexpr size_t diagnostic_write_limit_ = 4096;
    std::chrono::milliseconds flush() MACHA_REQUIRES(m_);
    std::chrono::milliseconds drain_one_extent() MACHA_REQUIRES(m_);
    std::chrono::milliseconds drain_staging_locked() MACHA_REQUIRES(m_);
    void prepare_append_tail() MACHA_REQUIRES(m_);
    bool canonical_base() const MACHA_REQUIRES(m_);
    void begin_sparse_overlay() MACHA_REQUIRES(m_);
    void note_changed_range(uint64_t, uint64_t) MACHA_REQUIRES(m_);
    bool range_changed(uint64_t, uint64_t) const MACHA_REQUIRES(m_);
    WritePreparation materialize_step(uint64_t) MACHA_REQUIRES(m_);
    void materialize() MACHA_REQUIRES(m_);
    WritePreparation rebuild_step(uint64_t) MACHA_REQUIRES(m_);
    void rebuild() MACHA_REQUIRES(m_);
    void cleanup() MACHA_REQUIRES(m_);
    void diagnostic_stage_extent(const char*, size_t, uint64_t, size_t) MACHA_REQUIRES(m_);
    void diagnostic_stage_checkpoint(const char*) MACHA_REQUIRES(m_);
    void launch_pending_extent(PendingExtent&) MACHA_REQUIRES(m_);
    void ensure_buffer_memory() MACHA_REQUIRES(m_);
    std::string current_path() const;

  public:
    WriteHandle(FileSystem&, std::string, FsEntry, bool, bool cache_puts = false,
                WriteDurability = WriteDurability::immediate,
                uint64_t publication_pipeline_bytes = 0,
                DataWorkContext work_context = DataWorkContext{});
    ~WriteHandle() override;
    // Prepares any existing generation needed for a write at `offset`. Zero
    // budget runs to completion; non-zero is a hard DATA byte quantum and may
    // return !ready so the caller can yield.
    WritePreparation prepare_write(uint64_t offset, uint64_t byte_budget = 0) override;
    WritePreparation prepare_commit(uint64_t byte_budget = 0) override;
    size_t write(uint64_t, std::span<const uint8_t>) override;
    void truncate(uint64_t) override;
    void commit() override;
    // The mtime the next commit publishes. The FUSE frontend sets the inode's
    // visible mtime so a utimens after the writes (rsync's order) is not
    // overwritten by the asynchronous publication's own timestamp.
    void set_committed_mtime(int64_t mtime_ns) override {
        Lock lock(m_);
        committed_mtime_ = mtime_ns;
    }
    void drain_staging() override;
    WriteHandleDiagnostics diagnostics() const override;
    FsEntry committed_entry() const override { Lock lock(m_); return base_; }
    uint64_t diagnostic_id() const noexcept {
        return diagnostic_id_;
    }
    uint64_t size() const {
        Lock lock(m_);
        return logical_;
    }
};
class FileSystem final : public PublicationTarget {
    friend class WriteHandle;

    const Config& config_;
    NodeId node_id_;
    const Membership& membership_;
    LocalState& local_;
    MetadataServer& metadata_server_;
    RetainedMemoryLedger& retained_memory_;
    DistributedStore& s_;
    MetadataView& m_;
    PlaybackTracker* playback_{};
    // Held across open_write()'s path resolution and getattr, and the rename
    // fix-up's logging.
    IoMutex open_writes_mutex_;
    std::vector<std::weak_ptr<WriteHandle>> open_writes_ MACHA_GUARDED_BY(open_writes_mutex_);
    // Group commit of namespace batches. A batch that arrives while a commit
    // is in flight waits in the queue; whoever commits next takes every
    // waiting batch into its one metadata commit. A batch whose operations
    // fail is undone within that commit and fails alone.
    struct QueuedNamespaceBatch {
        std::span<const FilesystemNamespaceMutation> operations;
        bool atomic{};
        FilesystemNamespaceBatchResult result;
        std::exception_ptr error;
        bool done{};
    };
    Mutex namespace_batch_queue_mutex_;
    std::vector<QueuedNamespaceBatch*> namespace_batch_queue_
        MACHA_GUARDED_BY(namespace_batch_queue_mutex_);
    // Held across the metadata commit by the thread committing the queue.
    IoMutex namespace_batch_commit_mutex_;
    void apply_namespace_batch_to(QueuedNamespaceBatch&, NamespaceWorkingSet&, MetadataSnapshot&,
                                  MetadataDelta&);
    // Cooperative cancellation of mount I/O at shutdown; extent transfers carry
    // it into DistributedStore.
    std::atomic_bool io_cancelled_{};
    // Fixed process-lifetime executor for publication extents: a thread per
    // extent makes glibc's per-thread arenas retain gigabytes. The queue holds
    // two tasks per worker, matching the default per-publication pipeline.
    using ExtentTask = std::packaged_task<WriteHandle::StagedExtentResult()>;
    mutable Mutex extent_tasks_mutex_;
    std::condition_variable_any extent_tasks_cv_;
    std::deque<std::shared_ptr<ExtentTask>> extent_tasks_ MACHA_GUARDED_BY(extent_tasks_mutex_);
    // Fixed at construction.
    size_t extent_worker_limit_{};
    size_t extent_task_limit_{};
    std::atomic_uint64_t extent_tasks_active_{};
    std::atomic_uint64_t extent_tasks_peak_queued_{};
    std::atomic_uint64_t extent_tasks_peak_active_{};
    std::atomic_uint64_t extent_tasks_submitted_{};
    // Counts releases of MemoryOwner::publication leases (an extent retiring
    // into the manifest, a handle committing): the only events that can end a
    // retained-memory admission wait. Admitted quanta must not count, or a
    // failing node re-arms every no-progress deadline and never parks.
    std::atomic_uint64_t write_progress_{};
    std::vector<std::jthread> extent_workers_ MACHA_GUARDED_BY(extent_tasks_mutex_);
    void extent_worker(std::stop_token);
    std::future<WriteHandle::StagedExtentResult> submit_extent_task(
        std::function<WriteHandle::StagedExtentResult()>);
    // A path as the namespace stores it. `ambiguous` when two stored names
    // are canonically equivalent to the one asked for.
    struct ResolvedPath {
        std::optional<std::string> path;
        bool ambiguous{};
    };
    static ResolvedPath resolve_in(const MetadataSnapshot&, const NamespaceNodeStore&,
                                   const std::string& normalized);
    std::optional<std::string> resolve_existing_path(const std::string&);
    std::string resolve_new_path(const std::string&);

    // Held across namespace walks and entry lookups that read tree nodes
    // from the control store, which may fetch them from peers.
    IoMutex media_index_mutex_;
    uint64_t media_index_namespace_revision_ MACHA_GUARDED_BY(media_index_mutex_){};
    bool media_index_valid_ MACHA_GUARDED_BY(media_index_mutex_){};
    // Owns the entries media_index_ refers to. Content-addressed media ids stay
    // valid across generations; only a miss inspects newer metadata.
    std::shared_ptr<const MetadataSnapshot> media_index_snapshot_
        MACHA_GUARDED_BY(media_index_mutex_);
    // Media id to every path holding that content.
    std::map<std::string, std::vector<std::string>, std::less<>> media_index_
        MACHA_GUARDED_BY(media_index_mutex_);
    // The tree the index was built from, when the namespace is a tree.
    std::optional<ObjectId> media_index_root_ MACHA_GUARDED_BY(media_index_mutex_);
    void refresh_media_index(const MetadataSnapshotView&) MACHA_REQUIRES(media_index_mutex_);
    // What the namespace refers to, one item per reference and in order, at
    // the tree it was last counted at. Guarded by maintenance_build_mutex_.
    struct NamespaceCensus {
        std::optional<ObjectId> root;
        // DATA extents the entries name; one that two files share is here twice.
        std::vector<ObjectId> extents;
        // The tree's own nodes: branches and leaves once, extent spine nodes
        // once for each entry that holds them.
        std::vector<ObjectId> nodes;
        size_t entries{};
        size_t extent_count{};
    };
    // Held across a census: a namespace walk the first time, the tree diff
    // after.
    IoMutex maintenance_build_mutex_;
    NamespaceCensus maintenance_census_ MACHA_GUARDED_BY(maintenance_build_mutex_);
    void census_walk(const MetadataSnapshot&, NamespaceCensus&);
    // False when the census cannot be brought to `root` from where it stands.
    bool census_follow(const ObjectId& root, NamespaceCensus&);
    MaintenanceObjects maintenance_objects_from(const MetadataSnapshotView&,
                                                const NamespaceCensus&);
    Mutex maintenance_index_mutex_;
    uint64_t maintenance_index_generation_ MACHA_GUARDED_BY(maintenance_index_mutex_){};
    std::shared_ptr<const MaintenanceObjects> maintenance_index_
        MACHA_GUARDED_BY(maintenance_index_mutex_);
    // Decoded local replica record, cached apart from MetadataManager's
    // authoritative read path, which local snapshot views must never enter.
    // Held across the replica's materialisation, which reads history frames
    // from disk.
    IoMutex local_snapshot_mutex_;
    uint64_t local_snapshot_generation_ MACHA_GUARDED_BY(local_snapshot_mutex_){};
    Hash256 local_snapshot_hash_ MACHA_GUARDED_BY(local_snapshot_mutex_){};
    std::shared_ptr<const MetadataSnapshot> local_snapshot_cache_
        MACHA_GUARDED_BY(local_snapshot_mutex_);
    void commit_write(WriteHandle&, const FsEntry&, uint64_t,
                      const std::vector<ExtentRef>&, FsEntry*,
                      std::optional<int64_t> mtime_override = {});
    static void require_parent(const NamespaceWorkingSet&, const std::string&);
    static std::optional<FsEntry> apply_namespace_mutation(
        NamespaceWorkingSet&, MetadataSnapshot&, MetadataDelta&,
        const FilesystemNamespaceMutation&);

  public:
    FileSystem(const Config&, NodeId, const Membership&, LocalState&, MetadataServer&,
               DistributedStore&, MetadataView&, RetainedMemoryLedger&,
               PlaybackTracker* = nullptr);
    FsEntry getattr(const std::string&);
    std::vector<std::pair<std::string, FsEntry>> readdir(const std::string&);
    void mkdir(const std::string&, uint32_t, uint32_t, uint32_t);
    void rmdir(const std::string&);
    FsEntry create_file(const std::string&, uint32_t, uint32_t, uint32_t);
    void unlink(const std::string&);
    void rename(const std::string&, const std::string&, bool = false);
    void chmod(const std::string&, uint32_t);
    void chown(const std::string&, uint32_t, uint32_t, bool, bool);
    void utimens(const std::string&, int64_t);
    // `atomic`: all or nothing, so with an `identity` "clock advanced" means
    // the whole batch applied. Otherwise the largest valid prefix commits and
    // the failure is reported in the result.
    FilesystemNamespaceBatchResult apply_namespace_batch(
        std::span<const FilesystemNamespaceMutation>,
        std::optional<MetadataMutationIdentity> identity = {}, bool atomic = false);
    void truncate_file(const std::string&, uint64_t);
    std::shared_ptr<ReadHandle> open_read(const std::string&);
    // Opens a resolved immutable entry, so a path replacement cannot change
    // the bytes under a playback session.
    std::shared_ptr<ReadHandle> open_read(const FsEntry&, const std::string& logical_path,
                                          bool track_playback = true,
                                          FrameType frame_type = FrameType::foreground);
    std::optional<std::pair<std::string, FsEntry>> find_media(std::string_view);
    // The media id of every file in this node's own head, in order.
    std::vector<std::string> media_ids();
    // Every file of this node's own head that has content, as (path, media
    // id) in path order, and the head they are of. From the index: no
    // namespace walk once it is built.
    std::vector<std::pair<std::string, std::string>> media_files(MetadataSnapshotView& of);
    std::shared_ptr<WriteHandle> open_write(const std::string&, bool, bool cache_puts = false,
                                            WriteDurability = WriteDurability::immediate,
                                            uint64_t publication_pipeline_bytes = 0,
                                            DataWorkContext work_context = DataWorkContext{});
    // open_write() for a publication generation, never truncating.
    uint64_t reachability_epoch() const noexcept override;
    std::shared_ptr<PublicationWriter> open_publication(const std::string& path, bool cache_puts,
                                                        uint64_t pipeline_bytes,
                                                        DataWorkContext work_context) override;
    std::optional<uint64_t> active_write_size(const std::string&);
    std::vector<WriteHandleDiagnostics> active_write_diagnostics(const std::string&);
    // `stale_basis_is_replayable`: a mismatched basis is ESTALE, not EAGAIN. A
    // publication writer's basis is then permanently wrong, so the generation
    // must be replayed from the spool against a fresh writer. Foreground
    // handles keep EAGAIN, where retrying is meaningful.
    void commit_file(const std::string&, const FsEntry&, uint64_t,
                     const std::vector<ExtentRef>&, FsEntry*,
                     std::optional<int64_t> mtime_override = {},
                     bool stale_basis_is_replayable = false);
    std::pair<uint64_t, uint64_t> logical_capacity() const;
    MetadataSnapshot local_snapshot() const;
    MetadataSnapshotView local_snapshot_view();
    std::optional<MetadataSnapshotView> available_snapshot_view() const;
    uint64_t available_snapshot_generation() const noexcept {
        return m_.current_generation();
    }
    uint64_t available_namespace_revision() const noexcept {
        return m_.current_namespace_revision();
    }
    uint64_t local_committed_metadata_generation() const noexcept {
        return local_.replica().committed_generation();
    }
    uint64_t known_metadata_generation() const noexcept { return metadata_server_.known_generation(); }
    std::vector<ObjectId> live_objects();
    // Hashes namespace/content identity, excluding catalogue metadata, so the
    // catalogue scanner's own commits cannot trigger a rescan loop.
    Hash256 namespace_signature(uint64_t* metadata_generation = nullptr);
    std::optional<Hash256> available_namespace_signature(
        uint64_t* metadata_generation = nullptr) const;
    std::shared_ptr<const MaintenanceObjects> maintenance_objects_cached();
    // The same, from a walk of the whole namespace.
    MaintenanceObjects maintenance_objects();
    // What the namespace of `snapshot` refers to, from the census brought to
    // its tree. None for a namespace that is not a tree.
    std::optional<NamespaceReferences> namespace_references(const MetadataSnapshot& snapshot);
    DistributedStore& store() {
        return s_;
    }
    // Read-only view of the namespace tree nodes. Cheap, so made per call
    // rather than cached, which also prevents writing through it.
    ControlNamespaceNodeStore namespace_nodes() {
        return ControlNamespaceNodeStore::for_reading(local_.control(), s_);
    }
    void note_interactive_activity(uint64_t bytes = 0) { s_.interactive_activity(bytes); }
    void note_foreground_activity(uint64_t bytes = 0) { s_.foreground_activity(bytes); }
    std::chrono::milliseconds foreground_idle_for() const { return s_.foreground_idle_for(); }
    std::chrono::milliseconds interactive_idle_for() const { return s_.interactive_idle_for(); }
    std::chrono::milliseconds loader_idle_for() const { return s_.loader_idle_for(); }
    void reset_io_cancellation() { io_cancelled_.store(false, std::memory_order_relaxed); }
    void request_io_cancellation() {
        io_cancelled_.store(true, std::memory_order_relaxed);
        extent_tasks_cv_.notify_all();
    }
    bool io_cancellation_requested() const {
        return io_cancelled_.load(std::memory_order_relaxed);
    }
    ExtentExecutorDiagnostics extent_executor_diagnostics() const;
    std::atomic_bool* io_cancellation_flag() { return &io_cancelled_; }
    const Config& config() const {
        return config_;
    }
    NodeId node_id() const {
        return node_id_;
    }
    size_t extent_size() const {
        return config_.extent_size;
    }
    // Process-wide: memory one writer releases can admit another, so any
    // writer's progress re-arms every no-progress window.
    const std::atomic_uint64_t& write_progress() const noexcept { return write_progress_; }
    void note_write_progress() noexcept {
        write_progress_.fetch_add(1, std::memory_order_relaxed);
    }
};

// Derived only from logical size and the ordered extent manifest, so stable
// across renames.
std::string file_media_id(const FsEntry&);

} // namespace macha
