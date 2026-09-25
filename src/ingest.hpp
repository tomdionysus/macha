// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue_hints.hpp"
#include "config.hpp"
#include "filesystem.hpp"
#include "json.hpp"
#include "torrent_extent_journal.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
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
    // Why the job is blocked or failed: `error_code` is the snake_case code
    // clients act on (see IngestError), `error` the English message beside it.
    std::string error_code;
    std::string error;
    std::vector<IngestFileProgress> files;
};

// A job failure with its code. Thrown from the import path; process_job
// records the code beside the message.
class IngestError : public std::runtime_error {
  public:
    IngestError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

// HTTP/RPC-facing JSON shape for an ingest job -- shared by the local HTTP
// handler (acquisition_api.cpp) and the cluster-wide RPC bridge below, so a
// remote job renders identically to a local one (plus a "node_id" field the
// caller tags on afterward). Distinct from job_json()/parse_job() in
// ingest.cpp, which is the on-disk jobs.json persistence shape.
Json optional_u64(const std::optional<uint64_t>&);
Json catalogue_summary_json(const IngestJob&, const CatalogueHintSummary* detail = nullptr);
Json ingest_job_json(const IngestJob&, bool include_files,
                     const CatalogueHintSummary* catalogue_detail = nullptr);

struct ClusterIngestJob {
    NodeId node_id;
    IngestJob job;
    CatalogueHintSummary catalogue;
};

struct IngestActionResult {
    bool exists{};
    bool changed{};
    std::optional<ClusterIngestJob> updated;
};

struct StagingStatus {
    std::filesystem::path path;
    uint64_t limit{};
    uint64_t disk_bytes{};
    uint64_t reserved_bytes{};
    uint64_t accounted_bytes{};
};

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
    std::condition_variable_any cv_;
    std::map<std::string, IngestJob, std::less<>> jobs_;
    // Jobs currently claimed by a worker. A job is inserted under mutex_ in
    // the same critical section that selects it, so two workers can never
    // claim the same job, and cancel() can still tell whether cleanup is its
    // own responsibility or the owning worker's.
    std::set<std::string, std::less<>> active_job_ids_;
    // High-water mark of concurrently claimed jobs. Monotonic, so it answers
    // "did this pool ever actually run jobs in parallel?" without having to
    // catch the moment in a sample.
    size_t peak_active_jobs_{};
    std::vector<std::jthread> workers_;
    // Catalogue completion is observed by polling the hint queue rather than
    // by callback. That poll owns its own thread so it cannot be starved by
    // busy import workers -- which is exactly what a single shared worker did.
    std::jthread catalogue_worker_;

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
    // file of the job uses. Jobs planned before 0.58.3 could give two source
    // files one destination; this resolves them on resume.
    void resolve_duplicate_destinations(IngestJob&);
    bool copy_file(IngestJob&, IngestFileProgress&, std::stop_token);
    // A torrent-sourced file whose every extent the torrent's disk backend has
    // already published: its manifest, from the job's TorrentExtentJournal.
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
    bool allowed_external_source(const std::filesystem::path&) const;
    void ensure_namespace_parents(std::string_view path);
    bool should_pause_or_cancel(const IngestJob&) const;
    void set_blocked(IngestJob&, std::string code, std::string message);
    void cleanup_partials(const IngestJob&);

    // NodeRuntime::set_ingest_bridge() handler bodies. Local-only -- never
    // call the *_cluster_wide() methods from here, or a peer's survey would
    // itself re-survey its own peers.
    Bytes handle_jobs_query(std::span<const uint8_t> request_payload) const;
    Bytes handle_job_action(std::span<const uint8_t> request_payload);
    IngestActionResult dispatch_action_cluster_wide(std::string_view id, std::string_view action);

  public:
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
    bool cancel(std::string_view id);
    bool clear(std::string_view id);
    CatalogueHintSummary catalogue_summary(std::string_view id) const;
    bool delete_owned_source_on_clear() const;
    bool delete_external_source_on_clear() const;
    bool delete_owned_source_on_cancel() const;
    StagingArea& staging() noexcept { return staging_; }
    // The torrent disk backend publishes extents through the same store the
    // ingest commits into.
    FileSystem& filesystem() noexcept { return fs_; }
    const StagingArea& staging() const noexcept { return staging_; }
    size_t max_concurrent_jobs() const noexcept { return config_.max_concurrent_jobs; }
    size_t active_jobs() const;
    size_t peak_active_jobs() const;

    // Cluster-wide visibility: local jobs (this node's own jobs()), plus one
    // RPC survey per active peer. An unreachable/erroring peer is logged and
    // skipped, never fails the whole call -- same partial-tolerance contract
    // as MetadataManager::discover_accepted_heads().
    std::vector<ClusterIngestJob> jobs_cluster_wide() const;
    // Local job(id) first (zero added latency for the common owned-here
    // case); only surveys peers when the job is locally absent.
    std::optional<ClusterIngestJob> job_cluster_wide(std::string_view id) const;
    // Each: local action first; only surveys peers when the job is locally
    // absent. The first peer reporting the job exists is authoritative,
    // preserving the local 404-vs-409 distinction cluster-wide.
    IngestActionResult pause_cluster_wide(std::string_view id);
    IngestActionResult resume_cluster_wide(std::string_view id);
    IngestActionResult cancel_cluster_wide(std::string_view id);
    IngestActionResult clear_cluster_wide(std::string_view id);
};

} // namespace macha
