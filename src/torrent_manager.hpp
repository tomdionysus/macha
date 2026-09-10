// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Part of the libmacha-torrent plugin, not of macha_core: this is the only
// header that names libtorrent-backed machinery, and nothing in core includes
// it. Core addresses BitTorrent acquisition through TorrentService
// (torrent.hpp) alone. See TODO/2026-09-05-subsystem-plugin-isolation-plan.md.

#include "ingest.hpp"
#include "torrent.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>

namespace macha {

class TorrentManager final : public TorrentService {
    struct Impl;

    NodeRuntime& node_;
    IngestManager& ingest_;
    TorrentConfig config_;
    std::filesystem::path state_file_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, TorrentJob, std::less<>> jobs_;
    std::unique_ptr<Impl> impl_;
    std::atomic_bool alerts_pending_{false};
    // Listen endpoints libtorrent reported succeeding, excluding loopback. A
    // session with none of these can reach no peer and must say so.
    size_t routable_listen_endpoints_{};
    bool warned_loopback_only_{};
    std::jthread worker_;

    void load_state();
    void save_state_locked() const;
    void restore_jobs();
    void loop(std::stop_token);
    // Drains libtorrent's alert queue into the journal. Also the only place
    // that can observe whether the session actually bound a usable interface.
    void drain_alerts();
    void update_jobs();
    bool has_active_jobs_locked() const;
    std::string add_impl(std::string uri, bool allow_fetch);

    // NodeRuntime::set_torrent_bridge() handler bodies. Local-only -- never
    // call the *_cluster_wide() methods from here, or a peer's survey would
    // itself re-survey its own peers.
    Bytes handle_jobs_query(std::span<const uint8_t> request_payload) const;
    Bytes handle_job_action(std::span<const uint8_t> request_payload);
    TorrentActionResult dispatch_action_cluster_wide(std::string_view id, std::string_view action);

  public:
    TorrentManager(NodeRuntime&, IngestManager&, TorrentConfig,
                   const std::filesystem::path& state_path);
    ~TorrentManager() override;

    void start();
    void request_stop();
    void stop();

    bool enabled() const noexcept override { return config_.enabled; }
    void reconfigure(TorrentConfig) override;

    std::string add(std::string magnet_uri) override;
    std::string add_search_result(std::string acquisition_uri) override;
    std::vector<TorrentJob> jobs() const override;
    std::optional<TorrentJob> job(std::string_view id) const override;
    bool pause(std::string_view id) override;
    bool resume(std::string_view id) override;
    bool retry(std::string_view id) override;
    bool cancel(std::string_view id) override;
    bool clear(std::string_view id) override;

    std::vector<ClusterTorrentJob> jobs_cluster_wide() const override;
    std::optional<ClusterTorrentJob> job_cluster_wide(std::string_view id) const override;
    TorrentActionResult pause_cluster_wide(std::string_view id) override;
    TorrentActionResult resume_cluster_wide(std::string_view id) override;
    TorrentActionResult retry_cluster_wide(std::string_view id) override;
    TorrentActionResult cancel_cluster_wide(std::string_view id) override;
    TorrentActionResult clear_cluster_wide(std::string_view id) override;
};

} // namespace macha
