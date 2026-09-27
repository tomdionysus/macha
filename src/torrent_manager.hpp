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
#include <functional>
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
    // SubsystemSupervisor's fault sink, through TorrentSubsystem: a fault the
    // worker cannot contain to one job rebuilds the manager from its durable
    // state. Guarded by mutex_.
    std::function<void(std::string)> fault_sink_;

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
    // torrent's own bitfield. Repeats are harmless. Caller holds mutex_.
    void report_held_pieces_locked(const TorrentJob&, const libtorrent::torrent_handle&);
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
    // The job a session handle belongs to, if any. Caller holds mutex_.
    TorrentJob* job_of_locked(const libtorrent::torrent_handle&);
    // The job, in any state, that already holds this info hash (0.63.0). One
    // job per torrent: libtorrent keys a torrent by its info hash and hands a
    // second add the first one's handle, so two jobs for one hash were two
    // owners of one torrent, and cancelling either removed the other's
    // torrent and deleted its payload (gbni-1, 2026-09-26).
    std::optional<std::string> job_holding_locked(std::string_view info_hash) const;
    // The one way a torrent leaves the session and impl_->handles, so no
    // handle is ever left naming a removed torrent. Caller holds mutex_.
    void retire_torrent_locked(const std::string& id, bool delete_payload);
    // A libtorrent or bookkeeping fault on one job fails that job and retires
    // its torrent; the worker carries on with every other job. Until 0.63.0
    // one such fault ended the worker for all of them. Caller holds mutex_.
    void isolate_fault_locked(const std::string& id, std::string_view what);
    void update_jobs();
    bool has_active_jobs_locked() const;
    // A failed job whose ingest has been resumed (through the ingest's own
    // route, a peer, or retry) and is running again. It must follow the
    // ingest back rather than stay failed with its staging held.
    bool linked_ingest_revived(const TorrentJob&) const;
    std::string add_impl(std::string uri, bool allow_fetch);
    // A parsed add: libtorrent's params and the canonical magnet persisted
    // for it. Defined in the .cpp, which alone sees libtorrent's types.
    struct ParsedAdd;
    void parse_add_uri(std::string uri, bool allow_fetch, ParsedAdd& out);
    std::string add_parsed(std::string id, ParsedAdd& parsed, bool held);

    // NodeRuntime::set_torrent_bridge() handler bodies: this node's own jobs
    // only. Every node's view of the cluster (ClusterJobView) is built from
    // these replies.
    Bytes handle_jobs_query(std::span<const uint8_t> request_payload) const;
    Bytes handle_job_action(std::span<const uint8_t> request_payload);

  public:
    TorrentManager(NodeRuntime&, IngestManager&, TorrentConfig,
                   const std::filesystem::path& state_path);
    ~TorrentManager() override;

    // Installed before start().
    void set_fault_sink(std::function<void(std::string)>);
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

    Placement place(std::string_view magnet_or_uri, bool search_result) override;
    Resolved resolve(std::string_view uri, bool search_result) override;
    std::string adopt(std::string_view id, std::string_view magnet, bool held) override;
    Offer offer() const override;
};

} // namespace macha
