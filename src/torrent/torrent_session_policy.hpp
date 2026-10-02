// SPDX-License-Identifier: GPL-3.0-or-later
//
// How Macha holds, releases and resumes a torrent in libtorrent's session;
// apart from TorrentManager so plugin tests can drive real sessions.
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

// A held torrent is paused *and* not auto-managed: libtorrent's queue resumes
// an auto-managed torrent regardless of pause().
void hold_torrent(libtorrent::torrent_handle&);
void release_torrent(libtorrent::torrent_handle&);
void hold_at_add(libtorrent::add_torrent_params&);
// Sets both flags from Macha's intent, overriding the flags resume data
// restores.
void set_hold_at_add(libtorrent::add_torrent_params&, bool held);

// A torrent's identity as lowercase hex: its v1 SHA-1 when it has one,
// otherwise its v2 SHA-256; empty when neither is known yet.
std::string torrent_info_hash_hex(const libtorrent::info_hash_t&);

enum class TorrentCheckPhase { none, queued, checking };

// libtorrent checks one torrent at a time; one awaiting its turn also reports
// checking_files, but paused.
TorrentCheckPhase torrent_check_phase(const libtorrent::torrent_status&);

// Resume data, so a restart trusts verified pieces instead of re-hashing. A
// missing, unreadable or foreign file yields nothing; the caller uses the magnet.
std::optional<libtorrent::add_torrent_params>
load_torrent_resume(const std::filesystem::path&, std::string_view expected_info_hash_hex);
void store_torrent_resume(const std::filesystem::path&, const libtorrent::add_torrent_params&);

} // namespace macha
