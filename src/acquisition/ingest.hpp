// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue_hints.hpp"
#include "config.hpp"
#include "filesystem/filesystem.hpp"
#include "json.hpp"
#include "torrent/torrent_extent_journal.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace macha {

class MediaInformationService;

enum class IngestJobState {
    queued,
    scanning,
    importing,
    cataloguing,
    paused,
    blocked,
    completed,
    cancelled,
    failed,
};

std::string ingest_job_state_name(IngestJobState);
std::optional<IngestJobState> parse_ingest_job_state(std::string_view);

struct IngestFileProgress {
    std::string source_path;
    std::string destination_path;
    std::string temporary_path;
    uint64_t size{};
    uint64_t copied{};
    int64_t source_mtime_ns{};
    bool completed{};
    bool skipped{};
    bool catalogue_candidate{true};
};

struct IngestJob {
    std::string id;
    std::string source_type{"filesystem"};
    std::string source_ref;
    std::string display_name;
    std::filesystem::path source_path;
    bool source_owned{};
    bool delete_source_on_clear{};
    IngestJobState state{IngestJobState::queued};
    uint64_t bytes_total{};
    uint64_t bytes_completed{};
    size_t files_total{};
    size_t files_completed{};
    size_t catalogue_total{};
    size_t catalogue_pending{};
    size_t catalogue_catalogued{};
    size_t catalogue_no_match{};
    size_t catalogue_failed{};
    std::string current_file;
    std::string current_destination;
    uint64_t rate_bytes_per_second{};
    std::optional<uint64_t> eta_seconds;
    uint64_t created_unix_ms{};
    uint64_t updated_unix_ms{};
    // Why the job is blocked or failed: the code clients act on, and its
    // English message.
    std::string error_code;
    std::string error;
    std::vector<IngestFileProgress> files;
};

// A job failure with its code, thrown from the import path.
class IngestError : public std::runtime_error {
  public:
    IngestError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

// API JSON for an ingest job, shared by the HTTP handler and the cluster RPC
// so remote and local jobs render alike. The on-disk jobs.json shape is
// job_json()/parse_job() in ingest.cpp.
Json optional_u64(const std::optional<uint64_t>&);
Json catalogue_summary_json(const IngestJob&, const CatalogueHintSummary* detail = nullptr);
Json ingest_job_json(const IngestJob&, bool include_files,
                     const CatalogueHintSummary* catalogue_detail = nullptr);
// The cluster RPC shape: persistence plus the transient rate and ETA.
Json ingest_job_wire_json(const IngestJob&);
IngestJob parse_ingest_job_wire(const Json&);

struct ClusterIngestJob {
    NodeId node_id;
    IngestJob job;
    CatalogueHintSummary catalogue;
};

struct IngestActionResult {
    bool exists{};
    bool changed{};
    // The job's node could not be reached.
    bool unreachable{};
    std::optional<ClusterIngestJob> updated;
};

struct StagingStatus {
    std::filesystem::path path;
    uint64_t limit{};
    uint64_t disk_bytes{};
    uint64_t reserved_bytes{};
    uint64_t accounted_bytes{};
};
// Staging limit, bytes on disk, bytes reserved by running downloads, their
// sum, and what is left.
Json staging_capacity_json(const StagingStatus&);

class StagingArea {
    IngestConfig config_;
    mutable std::mutex mutex_;
    std::map<std::string, uint64_t, std::less<>> reservations_;

    uint64_t disk_usage_unlocked() const;

  public:
    explicit StagingArea(IngestConfig);
    const std::filesystem::path& path() const noexcept { return config_.staging_path; }
    uint64_t limit() const noexcept { return config_.staging_limit; }
    void reconfigure_limit(uint64_t limit);
    bool contains(const std::filesystem::path&) const;
    bool reserve(std::string owner, uint64_t bytes);
    void release(std::string_view owner);
    uint64_t reservation(std::string_view owner) const;
    StagingStatus status() const;

    // Deletes a staging payload without waiting: renames it into `.trash`
    // (instant on one filesystem) for empty_trash(); until then it counts
    // against the limit. False when outside staging or not movable.
    bool discard(const std::filesystem::path&);
    // Deletes what it can of `.trash`; returns the number of entries removed.
    size_t empty_trash();
    std::filesystem::path trash_path() const;
    // Waits for discard() to trash something, the interval, or stop.
    void wait_for_trash(std::stop_token, std::chrono::milliseconds);

  private:
    std::mutex trash_mutex_;
    std::condition_variable_any trash_cv_;
    bool trash_pending_{true};
};

class IngestManager {
    NodeRuntime& node_;
    FileSystem& fs_;
    CatalogueHintQueue& hints_;
    MediaInformationService* media_information_{};
    IngestConfig config_;
    StagingArea staging_;
    std::filesystem::path state_file_;
    mutable std::mutex mutex_;
    std::mutex resume_listener_mutex_;
    std::function<void(std::string_view)> resume_listener_;
    bool resume_locked(std::string_view id);
    std::condition_variable_any cv_;
    std::map<std::string, IngestJob, std::less<>> jobs_;
    // Jobs claimed by a worker, inserted under mutex_ in the section that
    // selects them: no job is claimed twice, and cancel() knows whether
    // cleanup is its own or the worker's.
    std::set<std::string, std::less<>> active_job_ids_;
    // Cleanup owed by a clear() of a claimed job: the worker removes its
    // partials (and, per the clear, its source) as it releases the job.
    struct ClearedWhileActive {
        IngestJob job;
        bool delete_source{};
    };
    std::map<std::string, ClearedWhileActive, std::less<>> cleared_while_active_;
    // High-water mark of concurrently claimed jobs; shows parallelism without
    // catching it in a sample.
    size_t peak_active_jobs_{};
    std::vector<std::jthread> workers_;
    // Polls the hint queue for catalogue completion; its own thread so busy
    // import workers cannot starve it.
    std::jthread catalogue_worker_;
    std::jthread trash_worker_;

    void load_state();
    void save_state_locked() const;
    void loop(std::stop_token);
    void catalogue_loop(std::stop_token);
    // Picks the next job no worker holds, or empty. Caller must hold mutex_.
    std::string select_job_locked() const;
    void process_job(const std::string&, std::stop_token);
    bool plan_job(IngestJob&, std::stop_token);
    bool import_job(IngestJob&, std::stop_token);
    // Gives every unfinished file of a planned job a destination no other
    // file of the job uses.
    void resolve_duplicate_destinations(IngestJob&);
    bool copy_file(IngestJob&, IngestFileProgress&, std::stop_token);
    // The manifest of a torrent-sourced file whose every extent the torrent
    // disk backend has published, from the job's TorrentExtentJournal.
    std::optional<std::vector<ExtentRef>> published_extents(const IngestJob&,
                                                            const IngestFileProgress&);
    std::mutex extent_journals_mutex_;
    // By job id; see process_job.
    std::map<std::string, std::map<std::string, TorrentExtentJournal::File>> extent_journals_;
    void refresh_progress(IngestJob&, uint64_t sample_bytes = 0,
                          std::chrono::steady_clock::duration sample_time = {});
    void refresh_catalogue_jobs();
    void enqueue_catalogue_hints(IngestJob&);
    void cleanup_source(const IngestJob&);
    std::string choose_destination(const std::filesystem::path&, uint64_t);
    std::string with_existing_case(const std::string& destination);
    bool exists_ignoring_case(const std::string& path);
    bool allowed_external_source(const std::filesystem::path&) const;
    void ensure_namespace_parents(std::string_view path);
    bool should_pause_or_cancel(const IngestJob&) const;
    void set_blocked(IngestJob&, std::string code, std::string message);
    void cleanup_partials(const IngestJob&);

  public:
    // The ingest job routes' handlers (JobRoutes): this node's own jobs only;
    // ClusterJobView is built from these replies.
    Bytes handle_jobs_query(std::span<const uint8_t> request_payload) const;
    Bytes handle_job_action(std::span<const uint8_t> request_payload);
    IngestManager(NodeRuntime&, FileSystem&, CatalogueHintQueue&, IngestConfig,
                  MediaInformationService* media_information = nullptr);
    ~IngestManager();

    void start();
    void request_stop();
    void stop();
    void reconfigure(IngestConfig);
    bool enabled() const noexcept { return config_.enabled; }

    std::string submit_path(const std::filesystem::path& source,
                            std::string source_type = "filesystem",
                            std::string source_ref = {},
                            std::string display_name = {},
                            std::optional<bool> delete_source_on_clear = {},
                            bool trusted_internal_source = false,
                            bool source_owned = false);
    std::vector<IngestJob> jobs() const;
    std::optional<IngestJob> job(std::string_view id) const;
    bool pause(std::string_view id);
    bool resume(std::string_view id);
    // Told the id of every job resume() brings back, by any path, so a torrent
    // job that failed with its ingest follows it back. Called without the
    // ingest lock; set once, cleared before the listener goes away.
    void set_resume_listener(std::function<void(std::string_view)>);
    bool cancel(std::string_view id);
    bool clear(std::string_view id);
    CatalogueHintSummary catalogue_summary(std::string_view id) const;
    bool delete_owned_source_on_clear() const;
    bool delete_external_source_on_clear() const;
    bool delete_owned_source_on_cancel() const;
    StagingArea& staging() noexcept { return staging_; }
    // The store the ingest commits into; the torrent disk backend publishes
    // extents through it.
    FileSystem& filesystem() noexcept { return fs_; }
    const StagingArea& staging() const noexcept { return staging_; }
    size_t max_concurrent_jobs() const noexcept { return config_.max_concurrent_jobs; }
    size_t active_jobs() const;
    size_t peak_active_jobs() const;

};

} // namespace macha
