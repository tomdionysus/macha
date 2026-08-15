// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "distributed_store.hpp"
#include "metadata_manager.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
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
    std::vector<ObjectId> live;
    std::vector<ObjectId> garbage;
};

struct WriteHandleDiagnostics {
    uint64_t id{};
    uint64_t logical_size{};
    uint64_t staged_size{};
    size_t buffer_size{};
    bool sequential{};
    bool temp_open{};
    uint64_t temp_size{};
};

class ReadHandle {
    DistributedStore& s_;
    FsEntry e_;
    PlaybackTracker* playback_{};
    uint64_t playback_session_{};
    std::mutex m_;
    uint64_t last_{};
    size_t cached_index_{static_cast<size_t>(-1)};
    Bytes cached_extent_;
    const Bytes& extent(size_t);

  public:
    ReadHandle(DistributedStore&, FsEntry, PlaybackTracker* = nullptr,
               std::string path = {});
    ~ReadHandle();
    size_t read(uint64_t, std::span<uint8_t>);
};
class WriteHandle {
    friend class FileSystem;
class PlaybackTracker;

    FileSystem& fs_;
    std::string path_;
    FsEntry base_;
    uint64_t expected_{};
    bool sequential_{}, dirty_{};
    uint64_t logical_{}, staged_{};
    std::vector<ExtentRef> extents_;
    Bytes buffer_;
    int temp_{-1};
    std::filesystem::path temp_path_;
    mutable std::mutex m_;
    uint64_t diagnostic_id_{};
    uint64_t diagnostic_write_sequence_{};
    size_t diagnostic_completed_extents_{};
    struct DiagnosticWriteRange {
        uint64_t sequence{};
        uint64_t offset{};
        size_t length{};
        Hash256 hash{};
    };
    std::map<std::pair<uint64_t, size_t>, std::pair<uint64_t, Hash256>> diagnostic_exact_writes_;
    std::vector<DiagnosticWriteRange> diagnostic_writes_;
    void flush();
    void materialize();
    void rebuild();
    void cleanup();
    void diagnostic_stage_extent(const char*, size_t, uint64_t, size_t);
    void diagnostic_stage_checkpoint(const char*);

  public:
    WriteHandle(FileSystem&, std::string, FsEntry, bool);
    ~WriteHandle();
    size_t write(uint64_t, std::span<const uint8_t>);
    void truncate(uint64_t);
    void commit();
    WriteHandleDiagnostics diagnostics() const;
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
    MetadataSnapshot snap();
    void commit_write(WriteHandle&, const FsEntry&, uint64_t,
                      const std::vector<ExtentRef>&, FsEntry*);
    static void require_parent(const MetadataSnapshot&, const std::string&);

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
    void truncate_file(const std::string&, uint64_t);
    std::shared_ptr<ReadHandle> open_read(const std::string&);
    // Open an already-resolved immutable metadata snapshot. Playback uses this
    // so a pathname replacement cannot change the bytes underneath a session.
    std::shared_ptr<ReadHandle> open_read(const FsEntry&, const std::string& logical_path);
    std::optional<std::pair<std::string, FsEntry>> find_media(std::string_view);
    std::shared_ptr<WriteHandle> open_write(const std::string&, bool);
    std::optional<uint64_t> active_write_size(const std::string&);
    std::vector<WriteHandleDiagnostics> active_write_diagnostics(const std::string&);
    void commit_file(const std::string&, const FsEntry&, uint64_t,
                     const std::vector<ExtentRef>&, FsEntry*);
    std::pair<uint64_t, uint64_t> logical_capacity() const;
    std::vector<ObjectId> live_objects();
    MaintenanceObjects maintenance_objects();
    DistributedStore& store() {
        return s_;
    }
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
