// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent_session_policy.hpp"

#include "crypto.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace macha {
namespace lt = libtorrent;

void hold_torrent(lt::torrent_handle& handle) {
    // Order matters: while still auto-managed, the queue may resume what
    // pause() just stopped.
    handle.unset_flags(lt::torrent_flags::auto_managed);
    handle.pause();
}

void release_torrent(lt::torrent_handle& handle) {
    handle.set_flags(lt::torrent_flags::auto_managed);
    handle.resume();
}

void hold_at_add(lt::add_torrent_params& params) {
    params.flags |= lt::torrent_flags::paused;
    params.flags &= ~lt::torrent_flags::auto_managed;
}

std::string torrent_info_hash_hex(const lt::info_hash_t& hashes) {
    const auto text = [](const auto& digest) {
        return hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(digest.data()),
                                            digest.size()));
    };
    if (hashes.has_v1()) return text(hashes.v1);
    if (hashes.has_v2()) return text(hashes.v2);
    return {};
}

TorrentCheckPhase torrent_check_phase(const lt::torrent_status& status) {
    if (status.state != lt::torrent_status::checking_files &&
        status.state != lt::torrent_status::checking_resume_data)
        return TorrentCheckPhase::none;
    if (!(status.flags & lt::torrent_flags::paused)) return TorrentCheckPhase::checking;
    // Paused and auto-managed: the queue will start it. Paused and not: held
    // by Macha, and it will not be checked until released.
    return (status.flags & lt::torrent_flags::auto_managed) ? TorrentCheckPhase::queued
                                                            : TorrentCheckPhase::none;
}

std::optional<lt::add_torrent_params>
load_torrent_resume(const std::filesystem::path& path, std::string_view expected_info_hash_hex) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<char> buffer((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (buffer.empty()) return std::nullopt;
    lt::error_code error;
    auto params = lt::read_resume_data(buffer, error);
    if (error) {
        Log::warn("torrent resume data unreadable, re-checking instead path=" + path.string() +
                  " error=" + error.message());
        return std::nullopt;
    }
    const auto found = torrent_info_hash_hex(params.info_hashes);
    if (found.empty() || found != expected_info_hash_hex) {
        Log::warn("torrent resume data is for another torrent, re-checking instead path=" +
                  path.string() + " found=" + found +
                  " expected=" + std::string(expected_info_hash_hex));
        return std::nullopt;
    }
    return params;
}

void store_torrent_resume(const std::filesystem::path& path, const lt::add_torrent_params& params) {
    const auto buffer = lt::write_resume_data_buf(params);
    std::filesystem::create_directories(path.parent_path());
    durable_replace_file(path, std::string_view(buffer.data(), buffer.size()));
}

} // namespace macha
