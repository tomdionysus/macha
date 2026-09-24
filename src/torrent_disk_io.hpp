// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Part of the libmacha-torrent plugin, not of macha_core. libtorrent's disk
// I/O is a virtual disk_interface chosen by session_params::disk_io_constructor;
// this is macha's implementation of it.
//
// Why it exists: libtorrent's default backend runs ten threads of plain file
// I/O that neither the DATA arbiter admits nor the disk service monitor
// measures. On 2026-09-23 that pool wrote 65 MB/s onto es-1's DATA spindle,
// the monitor blamed publication for the slowness it measured, and seven
// ingests died (TODO/2026-09-23-torrent-writes-starve-publication-incident.md).
// Every read, write and hash this backend performs is admitted at loader class
// and timed into the monitor, so the torrent is one more loader under the laws
// rather than an unmetered writer beside them.
//
// Stage 1 of TODO/2026-09-23-torrent-disk-backend-plan.md: payload is written
// in the torrent's own file layout under the save path.
//
// Stage 2: each file-relative extent of that payload is published to the store
// as soon as every piece covering it has verified, and recorded in the job's
// TorrentExtentJournal, so the ingest commits the file by naming its extents
// instead of copying it. The payload file is the assembly area for its
// extents; reading an extent back goes through one function (read_extent),
// which is the seam where a staging format of macha's own would replace it.

#include "data_work.hpp"
#include "types.hpp"

#include <libtorrent/session_params.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

// Pieces libtorrent has verified, routed from the session's alerts (on the
// torrent manager's thread) to the disk backend that owns the storage, and
// the backend's publication progress answered back. A torrent is named by its
// save path, which is unique per job.
class TorrentPieceVerifications {
  public:
    using Sink = std::function<void(const std::string&, int)>;
    using Progress = std::function<std::optional<TorrentPublicationProgress>(const std::string&)>;

    void piece_verified(const std::string& save_path, int piece) {
        std::lock_guard lock(mutex_);
        if (sink_) sink_(save_path, piece);
    }
    // Nothing when no backend is publishing, or the backend does not hold
    // this torrent.
    std::optional<TorrentPublicationProgress> publication(const std::string& save_path) {
        std::lock_guard lock(mutex_);
        if (!progress_) return std::nullopt;
        return progress_(save_path);
    }
    void attach(Sink sink, Progress progress) {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
        progress_ = std::move(progress);
    }
    void detach() {
        std::lock_guard lock(mutex_);
        sink_ = nullptr;
        progress_ = nullptr;
    }

  private:
    std::mutex mutex_;
    Sink sink_;
    Progress progress_;
};

struct TorrentDiskHooks {
    // Blocks until `bytes` of loader-class DATA credit is held, and returns
    // it; the credit is released when the returned object is destroyed. It
    // must return promptly with nullptr once `aborting` is set. Empty means
    // "no admission" (tests).
    std::function<std::shared_ptr<void>(uint64_t bytes, const std::atomic_bool& aborting)> admit;
    // Service time of one completed file operation on the DATA device. Empty
    // when staging is not on that device: charging another disk's latency to
    // the DATA spindle would pressure it for nothing.
    std::function<void(std::chrono::nanoseconds elapsed, uint64_t bytes)> observe;
    // Worker threads doing file I/O. Two or three on a four-core node with one
    // spindle; libtorrent's own default of ten queued thirty-odd requests deep
    // on es-1.
    size_t threads{2};
    // Stage 2. With extent_size, publish and verifications all set, every
    // extent of the payload is published once its pieces verify. Unset, the
    // backend only writes payload files (stage 1 behaviour, and tests).
    uint64_t extent_size{};
    // Stores one extent's bytes durably and returns its object id, or nothing
    // on failure (the backend retries).
    std::function<std::optional<ObjectId>(std::span<const uint8_t>)> publish;
    std::shared_ptr<TorrentPieceVerifications> verifications;
    // How long a failed publication waits before it is tried again.
    std::chrono::milliseconds publish_retry{std::chrono::seconds(30)};
};

libtorrent::disk_io_constructor_type macha_disk_io_constructor(TorrentDiskHooks hooks);

// TorrentDiskHooks::admit backed by the node's DATA arbiter at loader class:
// an acquisition is durable work the user asked for, so it yields to a slow
// device only when a viewer would otherwise wait (law 3). It waits in
// one-second slices so an abort is seen promptly; a slice that ends early is
// the arbiter stopping or refusing outright, and then it returns nullptr.
std::function<std::shared_ptr<void>(uint64_t, const std::atomic_bool&)>
loader_admission(DataResourceArbiter& arbiter);

} // namespace macha
