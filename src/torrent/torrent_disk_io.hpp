// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// libmacha-torrent plugin only: macha's libtorrent disk_interface (set via
// session_params::disk_io_constructor). Every read, write and hash is admitted
// at loader class and timed into the disk service monitor, so a torrent is a
// loader under the laws, not an unmetered writer beside them.
//
// Payload is written in the torrent's file layout under the save path. Each
// file-relative extent is published to the store once every piece covering it
// verifies, and recorded in the job's TorrentExtentJournal, so the ingest
// commits files by naming extents instead of copying. Extents are read back
// only through read_extent.

#include "cluster/data_work.hpp"
#include "contract/thread_safety.hpp"
#include "types.hpp"

#include <libtorrent/session_params.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace macha {

// How many of a torrent's planned extents are published so far.
struct TorrentPublicationProgress {
    size_t published{};
    size_t extents{};
    bool complete() const { return published >= extents; }
};

// Verified pieces routed from the session's alerts (manager thread) to the
// disk backend owning the storage, and its publication progress back. A
// torrent is named by its save path, unique per job.
class TorrentPieceVerifications {
  public:
    using Sink = std::function<void(const std::string&, int)>;
    using Progress = std::function<std::optional<TorrentPublicationProgress>(const std::string&)>;

    void piece_verified(const std::string& save_path, int piece) {
        Lock lock(mutex_);
        if (sink_) sink_(save_path, piece);
    }
    // Empty when no backend is publishing or none holds this torrent.
    std::optional<TorrentPublicationProgress> publication(const std::string& save_path) {
        Lock lock(mutex_);
        if (!progress_) return std::nullopt;
        return progress_(save_path);
    }
    void attach(Sink sink, Progress progress) {
        Lock lock(mutex_);
        sink_ = std::move(sink);
        progress_ = std::move(progress);
    }
    void detach() {
        Lock lock(mutex_);
        sink_ = nullptr;
        progress_ = nullptr;
    }

  private:
    // Held across the attached backend's sink and progress callbacks.
    IoMutex mutex_;
    Sink sink_ MACHA_GUARDED_BY(mutex_);
    Progress progress_ MACHA_GUARDED_BY(mutex_);
};

struct TorrentDiskHooks {
    // Blocks until `bytes` of loader-class DATA credit is held and returns it,
    // released on destruction. Returns nullptr promptly once `aborting` is
    // set. Empty: no admission.
    std::function<std::shared_ptr<void>(uint64_t bytes, const std::atomic_bool& aborting)> admit;
    // Service time of one file operation on the DATA device. Empty when
    // staging is on another device, whose latency must not pressure DATA.
    std::function<void(std::chrono::nanoseconds elapsed, uint64_t bytes)> observe;
    // File I/O worker threads: two or three suit a four-core, one-spindle node.
    size_t threads{2};
    // With extent_size, publish and verifications all set, extents are
    // published as their pieces verify; unset, only payload files are written.
    uint64_t extent_size{};
    // Stores one extent durably and returns its id, or nothing on failure (the
    // backend retries). `abort` is set at shutdown to end a write promptly; the
    // unjournalled extent is published again on resume.
    std::function<std::optional<ObjectId>(std::span<const uint8_t>, std::atomic_bool& abort)> publish;
    std::shared_ptr<TorrentPieceVerifications> verifications;
    // How long a failed publication waits before it is tried again.
    std::chrono::milliseconds publish_retry{std::chrono::seconds(30)};
};

libtorrent::disk_io_constructor_type macha_disk_io_constructor(TorrentDiskHooks hooks);

// TorrentDiskHooks::admit on the DATA arbiter at loader class: yields only
// when a viewer would otherwise wait (law 3). Waits in one-second slices to
// see an abort; a slice ending early means the arbiter stopped or refused,
// and it returns nullptr.
std::function<std::shared_ptr<void>(uint64_t, const std::atomic_bool&)>
loader_admission(DataResourceArbiter& arbiter);

} // namespace macha
