// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Part of the libmacha-torrent plugin, not of macha_core: this is the only
// header that names libtorrent-backed machinery, and nothing in core includes
// it. Core addresses BitTorrent acquisition through TorrentService
// (torrent.hpp) alone. See TODO/2026-09-05-subsystem-plugin-isolation-plan.md.

#include "ingest.hpp"
#include "torrent.hpp"
#include "torrent_disk_io.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>

namespace libtorrent {
struct torrent_handle;
struct add_torrent_params;
}

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
    // Routes verified pieces from the alert drain to the disk backend.
    std::shared_ptr<TorrentPieceVerifications> verifications_ =
        std::make_shared<TorrentPieceVerifications>();
    std::atomic_bool alerts_pending_{false};
    // torrent.log_level, readable from the alert drain without the mutex.
    std::atomic<LogLevel> alert_log_level_{LogLevel::info};
    // Listen endpoints libtorrent reported succeeding, excluding loopback. A
    // session with none of these can reach no peer and must say so.
    size_t routable_listen_endpoints_{};
    bool warned_loopback_only_{};
    // Said once per session: a router with no UPnP must not become a
    // recurring complaint, but "this node has no inbound port" has to be
    // visible at least once above debug.
    bool logged_portmap_{};
    bool warned_portmap_failed_{};
    std::jthread worker_;

    // Each job's libtorrent resume data (0.61.0): without it every restart
    // re-hashed every staged byte of every torrent before any could download.
    std::filesystem::path resume_dir_;
    std::filesystem::path resume_path(std::string_view id) const;
    static constexpr auto resume_save_interval = std::chrono::minutes(5);
    Clock::time_point last_resume_save_{};
    // Asks libtorrent for a job's resume data; it arrives as an alert.
    static void request_resume_save(const libtorrent::torrent_handle&);
    // At stop: request resume data for every torrent and wait, bounded, for it.
    void save_all_resume_data();
    void write_resume_alert(const libtorrent::torrent_handle&, const libtorrent::add_torrent_params&);
    // Per job: the check's position when last sampled, for its rate and ETA.
    struct CheckSample {
        uint64_t checked{};
        Clock::time_point at{};
        double rate{};
    };
    std::map<std::string, CheckSample, std::less<>> check_samples_;

    void load_state();
    void save_state_locked() const;
    void restore_jobs();
    void loop(std::stop_token);
    // The hooks through which the torrent's disk backend is admitted and
    // measured: loader-class DATA credit, and the DATA device's service
    // monitor when staging shares its device.
    TorrentDiskHooks disk_hooks() const;
    // Drains libtorrent's alert queue into the journal. Also the only place
    // that can observe whether the session actually bound a usable interface.
    void drain_alerts();
    // Reports every piece a torrent holds to the disk backend, from the
    // torrent's own bitfield. Repeats are harmless.
    void report_held_pieces(const libtorrent::torrent_handle&);
    static constexpr auto held_pieces_report_interval = std::chrono::seconds(10);
    Clock::time_point last_held_pieces_report_{};
    // A downloaded torrent is handed to the ingest only once the disk backend
    // has published every extent of it, so the ingest adopts them rather than
    // copying. Per job: the progress last seen, and when it last advanced.
    struct PublicationWait {
        size_t published{};
        Clock::time_point advanced{};
    };
    std::map<std::string, PublicationWait, std::less<>> publication_waits_;
    // Publication that stops advancing for this long is given up on: the
    // ingest runs and copies what is missing, rather than the job waiting for
    // ever on a put that will not succeed.
    static constexpr auto publication_stall_limit = std::chrono::minutes(10);
    bool publication_settled_locked(const std::string& id, const TorrentJob& job);
    // The save path of the job a session handle belongs to, if any.
    std::optional<std::string> save_path_of(const libtorrent::torrent_handle&) const;
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

    Placement add_on(const NodeId&, std::string_view, bool search_result) override;
    std::vector<ClusterTorrentJob> jobs_cluster_wide() const override;
    std::optional<ClusterTorrentJob> job_cluster_wide(std::string_view id) const override;
    TorrentActionResult pause_cluster_wide(std::string_view id) override;
    TorrentActionResult resume_cluster_wide(std::string_view id) override;
    TorrentActionResult retry_cluster_wide(std::string_view id) override;
    TorrentActionResult cancel_cluster_wide(std::string_view id) override;
    TorrentActionResult clear_cluster_wide(std::string_view id) override;
};

} // namespace macha
