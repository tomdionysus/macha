// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "data_work.hpp"
#include "distributed_store.hpp"
#include "metadata_manager.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <stdexcept>
#include <vector>
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

struct MaintenanceObjects {
    // Sorted/unique compact indexes. A vector is materially smaller than a
    // tree node per extent on media namespaces containing millions of objects.
    std::vector<ObjectId> live;
    std::vector<GarbageRef> garbage;
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
    FsEntry e_;
    PlaybackTracker* playback_{};
    uint64_t playback_session_{};
    std::atomic<FrameType> frame_type_{FrameType::read_ahead};
    std::mutex m_;
    uint64_t last_{};
    size_t cached_index_{static_cast<size_t>(-1)};
    Bytes cached_extent_;
    const Bytes& extent(size_t, Clock::time_point, std::atomic_bool*);

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

class WriteHandle {
    friend class FileSystem;
class PlaybackTracker;

    FileSystem& fs_;
    std::string path_;
    FsEntry base_;
    uint64_t expected_{};
    bool sequential_{}, dirty_{};
    bool cache_puts_{};
    WriteDurability durability_{WriteDurability::immediate};
    DataWorkContext work_context_{};
    DistributedStore::DurabilityBatch durability_batch_;
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
    };
    std::deque<PendingExtent> pending_extents_;
    uint64_t pending_extent_bytes_{};
    uint64_t publication_pipeline_bytes_{};
    size_t peak_pending_extents_{};
    uint64_t logical_{}, staged_{};
    std::vector<ExtentRef> extents_;
    Bytes buffer_;
    std::optional<ExtentRef> append_tail_;
    int temp_{-1};
    std::filesystem::path temp_path_;
    mutable std::mutex m_;
    uint64_t diagnostic_id_{};
    uint64_t diagnostic_write_sequence_{};
    size_t diagnostic_completed_extents_{};
    size_t append_tail_fetches_{};
    size_t materialize_source_reads_{};
    uint64_t materialize_source_bytes_{};
    size_t materialize_steps_{};
    bool materializing_{};
    uint64_t materialize_offset_{};
    size_t materialize_extent_index_{};
    size_t new_extent_puts_{};
    size_t rebuild_reused_extents_{};
    size_t rebuild_put_extents_{};
    uint64_t rebuild_source_bytes_{};
    size_t rebuild_steps_{};
    bool rebuilding_{};
    bool rebuild_prepared_{};
    // Canonical committed manifests can be edited as a sparse changed-range
    // overlay. Unchanged extents remain immutable references and are never
    // copied into the temporary file merely to discover they are unchanged.
    bool sparse_overlay_{};
    struct ChangedRange {
        uint64_t begin{};
        uint64_t end{};
    };
    std::vector<ChangedRange> changed_ranges_;
    uint64_t rebuild_offset_{};
    size_t rebuild_index_{};
    std::vector<ExtentRef> rebuild_handle_extents_;
    struct DiagnosticWriteRange {
        uint64_t sequence{};
        uint64_t offset{};
        size_t length{};
        Hash256 hash{};
    };
    std::map<std::pair<uint64_t, size_t>, std::pair<uint64_t, Hash256>> diagnostic_exact_writes_;
    std::vector<DiagnosticWriteRange> diagnostic_writes_;
    std::chrono::milliseconds flush();
    std::chrono::milliseconds drain_one_extent();
    std::chrono::milliseconds drain_staging_locked();
    void prepare_append_tail();
    bool canonical_base() const;
    void begin_sparse_overlay();
    void note_changed_range(uint64_t, uint64_t);
    bool range_changed(uint64_t, uint64_t) const;
    WritePreparation materialize_step(uint64_t);
    void materialize();
    WritePreparation rebuild_step(uint64_t);
    void rebuild();
    void cleanup();
    void diagnostic_stage_extent(const char*, size_t, uint64_t, size_t);
    void diagnostic_stage_checkpoint(const char*);
    void launch_pending_extent(PendingExtent&);

  public:
    WriteHandle(FileSystem&, std::string, FsEntry, bool, bool cache_puts = false,
                WriteDurability = WriteDurability::immediate,
                uint64_t publication_pipeline_bytes = 0,
                DataWorkContext work_context = DataWorkContext{});
    ~WriteHandle();
    // Prepare any existing generation needed for a write at `offset`. A zero
    // budget preserves the synchronous API; a non-zero budget is a hard DATA
    // byte quantum and may return !ready so an outer scheduler can yield.
    WritePreparation prepare_write(uint64_t offset, uint64_t byte_budget = 0);
    WritePreparation prepare_commit(uint64_t byte_budget = 0);
    size_t write(uint64_t, std::span<const uint8_t>);
    void truncate(uint64_t);
    void commit();
    void drain_staging();
    WriteHandleDiagnostics diagnostics() const;
    FsEntry committed_entry() const { std::lock_guard lock(m_); return base_; }
    uint64_t diagnostic_id() const noexcept {
        return diagnostic_id_;
    }
    uint64_t size() const {
        std::lock_guard lock(m_);
        return logical_;
    }
};
class FileSystem {
    friend class WriteHandle;

    NodeRuntime& n_;
    DistributedStore& s_;
    MetadataManager& m_;
    PlaybackTracker* playback_{};
    std::mutex open_writes_mutex_;
    std::vector<std::weak_ptr<WriteHandle>> open_writes_;
    // Cooperative cancellation for mounted MachaDFS reads/writes during
    // daemon shutdown. Namespace-only operations are short metadata calls;
    // extent transfers carry this token into DistributedStore.
    std::atomic_bool io_cancelled_{};
    // Immutable media ids are used heavily by catalogue/playback resolution.
    // Cache their namespace lookup by metadata generation so playback startup
    // does not linearly re-hash every file for every candidate representation.
    struct NamespaceIndex {
        uint64_t generation{};
        Hash256 hash{};
        std::shared_ptr<const MetadataSnapshot> snapshot;
        // Store names/paths only. The immutable snapshot already owns FsEntry
        // manifests; duplicating every extent into the directory index would make
        // cache memory proportional to the namespace twice over.
        std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>> children;
        // macOS can present canonically-equivalent UTF-8 path spellings across
        // different VFS/FUSE operations. Keep persisted keys byte-preserving and
        // resolve only the runtime alias back to the actual stored path.
        std::map<std::string, std::string, std::less<>> canonical_paths;
        std::set<std::string, std::less<>> ambiguous_canonical_paths;
    };
    std::mutex namespace_index_mutex_;
    std::shared_ptr<const NamespaceIndex> namespace_index_;
    std::shared_ptr<const NamespaceIndex> namespace_index();
    std::optional<std::string> resolve_existing_path(const std::string&);
    std::string resolve_new_path(const std::string&);

    std::mutex media_index_mutex_;
    uint64_t media_index_namespace_revision_{};
    bool media_index_valid_{};
    // Keep the immutable snapshot that owns entries referenced by media_index_.
    // Existing content-addressed media ids remain valid across unrelated
    // namespace generations; only a cache miss needs to inspect newer metadata.
    std::shared_ptr<const MetadataSnapshot> media_index_snapshot_;
    // Media ids map to paths only; FsEntry remains owned by media_index_snapshot_.
    std::map<std::string, std::string> media_index_;
    std::mutex maintenance_index_mutex_;
    uint64_t maintenance_index_generation_{};
    std::shared_ptr<const MaintenanceObjects> maintenance_index_;
    // A local snapshot view must never enter MetadataManager's authoritative
    // read/discovery path. Cache the decoded content-addressed replica record
    // independently so repeated local consumers only share immutable state.
    std::mutex local_snapshot_mutex_;
    uint64_t local_snapshot_generation_{};
    Hash256 local_snapshot_hash_{};
    std::shared_ptr<const MetadataSnapshot> local_snapshot_cache_;
    MetadataSnapshot snap();
    void commit_write(WriteHandle&, const FsEntry&, uint64_t,
                      const std::vector<ExtentRef>&, FsEntry*);
    static void require_parent(const MetadataSnapshot&, const std::string&);
    static std::optional<FsEntry> apply_namespace_mutation(
        MetadataSnapshot&, MetadataDelta&, const FilesystemNamespaceMutation&);

  public:
    FileSystem(NodeRuntime&, DistributedStore&, MetadataManager&, PlaybackTracker* = nullptr);
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
    FilesystemNamespaceBatchResult apply_namespace_batch(
        std::span<const FilesystemNamespaceMutation>);
    void truncate_file(const std::string&, uint64_t);
    std::shared_ptr<ReadHandle> open_read(const std::string&);
    // Open an already-resolved immutable metadata snapshot. Playback uses this
    // so a pathname replacement cannot change the bytes underneath a session.
    std::shared_ptr<ReadHandle> open_read(const FsEntry&, const std::string& logical_path,
                                          bool track_playback = true,
                                          FrameType frame_type = FrameType::foreground);
    std::optional<std::pair<std::string, FsEntry>> find_media(std::string_view);
    std::shared_ptr<WriteHandle> open_write(const std::string&, bool, bool cache_puts = false,
                                            WriteDurability = WriteDurability::immediate,
                                            uint64_t publication_pipeline_bytes = 0,
                                            DataWorkContext work_context = DataWorkContext{});
    std::optional<uint64_t> active_write_size(const std::string&);
    std::vector<WriteHandleDiagnostics> active_write_diagnostics(const std::string&);
    void commit_file(const std::string&, const FsEntry&, uint64_t,
                     const std::vector<ExtentRef>&, FsEntry*);
    std::pair<uint64_t, uint64_t> logical_capacity() const;
    MetadataSnapshot local_snapshot() const;
    MetadataSnapshotView local_snapshot_view();
    std::optional<MetadataSnapshotView> available_snapshot_view() const;
    uint64_t available_snapshot_generation() const noexcept {
        return m_.available_snapshot_generation();
    }
    uint64_t available_namespace_revision() const noexcept {
        return m_.available_namespace_revision();
    }
    uint64_t local_committed_metadata_generation() const noexcept {
        return n_.metadata_replica().committed_generation();
    }
    uint64_t known_metadata_generation() const noexcept { return n_.known_metadata_generation(); }
    std::vector<ObjectId> live_objects();
    // Hash only namespace/content identity, deliberately excluding catalogue
    // metadata. CatalogueScanner uses this after a metadata-generation debounce
    // so its own catalogue commits cannot cause a rescan loop.
    Hash256 namespace_signature(uint64_t* metadata_generation = nullptr);
    std::optional<Hash256> available_namespace_signature(
        uint64_t* metadata_generation = nullptr) const;
    std::shared_ptr<const MaintenanceObjects> maintenance_objects_cached();
    MaintenanceObjects maintenance_objects();
    DistributedStore& store() {
        return s_;
    }
    void note_interactive_activity(uint64_t bytes = 0) { s_.interactive_activity(bytes); }
    void note_foreground_activity(uint64_t bytes = 0) { s_.foreground_activity(bytes); }
    std::chrono::milliseconds foreground_idle_for() const { return s_.foreground_idle_for(); }
    std::chrono::milliseconds interactive_idle_for() const { return s_.interactive_idle_for(); }
    void reset_io_cancellation() { io_cancelled_.store(false, std::memory_order_relaxed); }
    void request_io_cancellation() { io_cancelled_.store(true, std::memory_order_relaxed); }
    bool io_cancellation_requested() const {
        return io_cancelled_.load(std::memory_order_relaxed);
    }
    std::atomic_bool* io_cancellation_flag() { return &io_cancelled_; }
    NodeRuntime& node() {
        return n_;
    }
    size_t extent_size() const {
        return n_.config().extent_size;
    }
};

// Stable across namespace renames: identity is derived only from the logical
// file size and ordered content-addressed extent manifest.
std::string file_media_id(const FsEntry&);

} // namespace macha
