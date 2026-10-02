// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// libmacha-torrent plugin only: core never includes this, and reaches torrents
// through TorrentService (torrent.hpp).

#include "acquisition/ingest.hpp"
#include "torrent/torrent.hpp"
#include "torrent/torrent_disk_io.hpp"

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
    LocalState& local_;
    DataResourceArbiter& data_resources_;
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
    // Non-loopback listen endpoints libtorrent reported succeeding; with none
    // the session reaches no peer and must say so.
    size_t routable_listen_endpoints_{};
    bool warned_loopback_only_{};
    // "No inbound port" is logged above debug once per session, not repeatedly.
    bool logged_portmap_{};
    bool warned_portmap_failed_{};
    std::jthread worker_;
    // SubsystemSupervisor's fault sink: a fault the worker cannot contain to
    // one job rebuilds the manager from durable state. Guarded by mutex_.
    std::function<void(std::string)> fault_sink_;

    // Each job's libtorrent resume data, so a restart does not re-hash every
    // staged byte.
    std::filesystem::path resume_dir_;
    std::filesystem::path resume_path(std::string_view id) const;
    static constexpr auto resume_save_interval = std::chrono::minutes(5);
    Clock::time_point last_resume_save_{};
    // jobs.json is saved when a job's record changes; transfer counters alone
    // are saved at most this often.
    static constexpr auto progress_save_interval = std::chrono::seconds(30);
    Clock::time_point last_progress_save_{};
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
    // The disk backend's admission and measurement: loader-class DATA credit,
    // and the DATA device's service monitor when staging shares its device.
    TorrentDiskHooks disk_hooks() const;
    // Drains libtorrent's alert queue into the journal; the only place that
    // sees whether the session bound a usable interface.
    void drain_alerts();
    // Reports every piece in the torrent's bitfield to the disk backend;
    // repeats are harmless. Caller holds mutex_.
    void report_held_pieces_locked(const TorrentJob&, const libtorrent::torrent_handle&);
    static constexpr auto held_pieces_report_interval = std::chrono::seconds(10);
    Clock::time_point last_held_pieces_report_{};
    // A downloaded torrent goes to the ingest once every extent is published,
    // so the ingest adopts rather than copies. Per job: progress last seen and
    // when it last advanced.
    struct PublicationWait {
        size_t published{};
        Clock::time_point advanced{};
    };
    std::map<std::string, PublicationWait, std::less<>> publication_waits_;
    // Per job, the published count last seen and when it last moved, for
    // `publication`.
    std::map<std::string, PublicationWait, std::less<>> publication_seen_;
    void refresh_publication_locked(const std::string& id, TorrentJob& job);
    // Publication stalled this long is given up: the ingest copies what is missing.
    static constexpr auto publication_stall_limit = std::chrono::minutes(10);
    bool publication_settled_locked(const std::string& id, const TorrentJob& job);
    // The job a session handle belongs to, if any. Caller holds mutex_.
    TorrentJob* job_of_locked(const libtorrent::torrent_handle&);
    // The job, in any state, holding this info hash. One job per torrent:
    // libtorrent hands a second add of a hash the first one's handle, so two
    // jobs would share, and cancel, one torrent.
    std::optional<std::string> job_holding_locked(std::string_view info_hash) const;
    // The one way a torrent leaves the session and impl_->handles, so no
    // handle is ever left naming a removed torrent. Caller holds mutex_.
    void retire_torrent_locked(const std::string& id, bool delete_payload);
    // A fault on one job fails that job and retires its torrent; the worker
    // carries on with the rest. Caller holds mutex_.
    void isolate_fault_locked(const std::string& id, std::string_view what);
    void update_jobs();
    bool has_active_jobs_locked() const;
    // A failed job whose ingest was resumed follows it back rather than stay
    // failed with its staging held.
    bool linked_ingest_revived(const TorrentJob&) const;
    std::string add_impl(std::string uri, bool allow_fetch);
    // A parsed add: libtorrent's params and the canonical magnet to persist.
    // Defined in the .cpp, which alone sees libtorrent's types.
    struct ParsedAdd;
    void parse_add_uri(std::string uri, bool allow_fetch, ParsedAdd& out);
    std::string add_parsed(std::string id, ParsedAdd& parsed, bool held);

  public:
    // The torrent job messages' handlers: this node's own jobs
    // only; ClusterJobView is built from these replies.
    Bytes handle_jobs_query(std::span<const uint8_t> request_payload) const;
    Bytes handle_job_action(std::span<const uint8_t> request_payload);
    TorrentManager(NodeRuntime&, LocalState&, DataResourceArbiter&, IngestManager&, TorrentConfig,
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
