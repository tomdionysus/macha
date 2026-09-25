// SPDX-License-Identifier: GPL-3.0-or-later
//
// How Macha holds, releases and resumes a torrent in libtorrent's session.
// Kept apart from TorrentManager so the plugin test binary can drive it
// against real sessions.
#pragma once

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/info_hash.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace macha {

// A held torrent is paused *and* not auto-managed. libtorrent's queue manager
// resumes an auto-managed torrent regardless of pause(): measured on fi-1 on
// 2026-09-25, a torrent paused straight after add was running, fully
// re-checked and seeding three seconds later. Until 0.61.0 every Macha pause
// -- operator, staging_full, restore -- was that plain pause().
void hold_torrent(libtorrent::torrent_handle&);
void release_torrent(libtorrent::torrent_handle&);
void hold_at_add(libtorrent::add_torrent_params&);

// A torrent's identity as lowercase hex: its v1 SHA-1 when it has one,
// otherwise its v2 SHA-256; empty when neither is known yet.
std::string torrent_info_hash_hex(const libtorrent::info_hash_t&);

enum class TorrentCheckPhase { none, queued, checking };

// libtorrent checks one torrent at a time. One waiting its turn reports
// checking_files like the one being checked, but paused (it is auto-managed
// and the queue has not started it); only the unpaused one is reading.
TorrentCheckPhase torrent_check_phase(const libtorrent::torrent_status&);

// Resume data, so a restart trusts the pieces already verified instead of
// re-hashing every staged byte (gbni-1, 2026-09-25: an hour at the disk's
// full 90 MB/s after each restart, with every other torrent queued behind).
// A file that is missing, unreadable, or for a different torrent yields
// nothing and the caller falls back to the magnet.
std::optional<libtorrent::add_torrent_params>
load_torrent_resume(const std::filesystem::path&, std::string_view expected_info_hash_hex);
void store_torrent_resume(const std::filesystem::path&, const libtorrent::add_torrent_params&);

} // namespace macha
