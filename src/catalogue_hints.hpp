// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <stop_token>
#include <string_view>
#include <vector>

namespace macha {

namespace CatalogueHintPriority {
inline constexpr int periodic_scan = 10;
inline constexpr int namespace_mutation = 50;
inline constexpr int manual_rescan = 80;
inline constexpr int ingest = 100;
}

enum class CatalogueHintState {
    queued,
    processing,
    deferred,
    catalogued,
    no_match,
    failed,
};

std::string catalogue_hint_state_name(CatalogueHintState);
std::optional<CatalogueHintState> parse_catalogue_hint_state(std::string_view);

struct CatalogueHintOrigin {
    std::string source;
    std::string source_ref;
    int priority{};
    auto operator<=>(const CatalogueHintOrigin&) const = default;
};

struct CatalogueHintSubmission {
    std::string path;
    std::string source;
    std::string source_ref;
    int priority{};
};

struct CatalogueHint {
    std::string id;
    std::string path;
    int priority{};
    CatalogueHintState state{CatalogueHintState::queued};
    unsigned attempts{};
    unsigned failures{};
    size_t candidate_cursor{};
    uint64_t created_unix_ms{};
    uint64_t updated_unix_ms{};
    uint64_t ready_after_unix_ms{};
    std::string provider;
    std::string media_id;
    std::vector<std::string> catalogue_item_ids;
    std::string result;
    std::string error;
    std::vector<CatalogueHintOrigin> origins;
};

struct CatalogueHintSummary {
    size_t total{};
    size_t pending{};
    size_t catalogued{};
    size_t no_match{};
    size_t failed{};
    std::vector<CatalogueHint> hints;

    bool terminal() const noexcept { return total != 0 && pending == 0; }
};

class CatalogueHintQueue {
    std::filesystem::path state_file_;
    mutable std::mutex mutex_;
    std::condition_variable_any change_cv_;
    uint64_t revision_{};
    std::map<std::string, CatalogueHint, std::less<>> hints_; // canonical path -> hint
    std::map<std::string, uint64_t, std::less<>> lane_served_;
    uint64_t schedule_sequence_{};

    void load_state();
    void save_state_locked() const;
    static bool terminal(CatalogueHintState) noexcept;
    static bool has_origin(const CatalogueHint&, std::string_view, std::string_view);
    void changed_locked();

  public:
    explicit CatalogueHintQueue(const std::filesystem::path& state_path);

    std::string submit(std::string path, std::string source, std::string source_ref,
                       int priority);
    std::vector<std::string> submit_many(std::vector<CatalogueHintSubmission>);
    std::optional<CatalogueHint> claim_next();
    std::optional<std::chrono::milliseconds> next_ready_delay() const;
    uint64_t revision() const;
    bool wait_for_change(std::stop_token, uint64_t observed_revision,
                         std::chrono::milliseconds timeout);
    void mark_catalogued(std::string_view id, std::string provider, std::string media_id,
                         std::vector<std::string> catalogue_item_ids,
                         std::string result = {});
    void mark_no_match(std::string_view id, std::string provider, std::string media_id,
                       std::string result = {});
    void advance_candidate(std::string_view id, size_t next_cursor);
    void defer(std::string_view id, std::string error, uint64_t retry_after_unix_ms);
    bool record_failure(std::string_view id, std::string error,
                        uint64_t retry_after_unix_ms, unsigned max_failures);
    void fail(std::string_view id, std::string error);
    void requeue_processing();

    std::vector<CatalogueHint> list() const;
    std::optional<CatalogueHint> get(std::string_view id) const;
    CatalogueHintSummary summary(std::string_view source, std::string_view source_ref) const;
    size_t erase_origin(std::string_view source, std::string_view source_ref);
};

} // namespace macha
