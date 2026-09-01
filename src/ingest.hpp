// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue_hints.hpp"
#include "config.hpp"
#include "filesystem.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
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
    std::string error;
    std::vector<IngestFileProgress> files;
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
    std::string active_job_id_;
    std::jthread worker_;

    void load_state();
    void save_state_locked() const;
    void loop(std::stop_token);
    void process_job(const std::string&, std::stop_token);
    bool plan_job(IngestJob&, std::stop_token);
    bool import_job(IngestJob&, std::stop_token);
    bool copy_file(IngestJob&, IngestFileProgress&, std::stop_token);
    void refresh_progress(IngestJob&, uint64_t sample_bytes = 0,
                          std::chrono::steady_clock::duration sample_time = {});
    void refresh_catalogue_jobs();
    void enqueue_catalogue_hints(IngestJob&);
    void cleanup_source(const IngestJob&);
    std::string choose_destination(const std::filesystem::path&, uint64_t);
    bool allowed_external_source(const std::filesystem::path&) const;
    void ensure_namespace_parents(std::string_view path);
    bool should_pause_or_cancel(const IngestJob&) const;
    void set_blocked(IngestJob&, std::string);
    void cleanup_partials(const IngestJob&);

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
    const StagingArea& staging() const noexcept { return staging_; }
};

} // namespace macha
