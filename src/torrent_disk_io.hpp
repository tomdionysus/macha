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
// Stage 1 of TODO/2026-09-23-torrent-disk-backend-plan.md: payload is still
// written in the torrent's own file layout under the save path, so the ingest
// that follows a finished download is unchanged.

#include "data_work.hpp"

#include <libtorrent/session_params.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace macha {

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
};

libtorrent::disk_io_constructor_type macha_disk_io_constructor(TorrentDiskHooks hooks);

// TorrentDiskHooks::admit backed by the node's DATA arbiter at loader class:
// an acquisition is durable work the user asked for, so it yields to a slow
// device only when a viewer would otherwise wait (law 2). It waits in
// one-second slices so an abort is seen promptly; a slice that ends early is
// the arbiter stopping or refusing outright, and then it returns nullptr.
std::function<std::shared_ptr<void>(uint64_t, const std::atomic_bool&)>
loader_admission(DataResourceArbiter& arbiter);

} // namespace macha
