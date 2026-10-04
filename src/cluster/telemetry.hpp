// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "types.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// Same vocabulary as the Status API's "startup.phase". Defaults to ready, so
// a record that omits it is not read as perpetually recovering.
enum class NodePhase : uint8_t { starting, recovering, ready };

std::string_view node_phase_name(NodePhase);

// Median sustained transcode rate for one source class: produced / producing
// media time of recent finished generations, parked time excluded.
struct TranscodeRate {
    std::string kind;        // "video" or "audio"
    std::string codec;       // the source stream's codec
    uint32_t bit_depth{};    // video only
    uint32_t height_class{}; // video only: 576, 720, 1080, 1440, 2160, 4320
    uint32_t rate_milli{};   // median rate x1000
    uint32_t observations{};
    uint32_t concurrent{};   // median transcodes running on the node when observed
    auto operator<=>(const TranscodeRate&) const = default;
};

// This node's own cluster traffic for one frame class (FrameType wire value):
// bytes since start, and rate over the last telemetry interval.
struct TrafficClass {
    uint8_t frame_class{};
    uint64_t in_bytes{};
    uint64_t out_bytes{};
    uint32_t in_bytes_per_s{};
    uint32_t out_bytes_per_s{};
    auto operator<=>(const TrafficClass&) const = default;
};

// Cumulative per-class totals, indexed by FrameType wire value.
struct TrafficTotals {
    std::array<uint64_t, 6> in_bytes{};
    std::array<uint64_t, 6> out_bytes{};
};

struct NodeTelemetry {
    NodeId node_id{};
    NodeId boot_id{};
    uint64_t sequence{};
    uint64_t observed_unix_ms{};
    std::string version;
    std::string host;
    std::string failure_domain;
    uint16_t port{};
    uint64_t storage_capacity{};
    uint64_t storage_used{};
    uint64_t cache_capacity{};
    uint64_t cache_used{};
    // Block-cache activity, monotonic since start; consumers diff them.
    // cache_used reflects writes only; these show whether the cache serves,
    // which is all an edge node (hosts_extents false) serves from.
    uint64_t cache_hits{};
    uint64_t cache_misses{};
    uint64_t cache_evictions{};
    uint64_t metadata_generation{};
    uint64_t uptime_ms{};
    uint64_t rss_bytes{};
    uint32_t process_cpu_milli_percent{};
    uint32_t load1_milli{};
    uint32_t storage_backends_online{};
    uint32_t peers_known{};
    uint32_t peers_active{};
    uint64_t rpc_connections_created{};
    uint64_t rpc_connections_reused{};
    uint64_t rpc_connections_canonical{};
    NodePhase phase{NodePhase::ready};
    // Client-facing HTTP API URL, distinct from the RPC host/port. Carries a
    // scheme because the API may sit behind a TLS proxy. Empty: none reported.
    std::string api_endpoint;
    // Hardware threads; zero means unreported. load1 and CPU percent are
    // per-core, so cross-node comparison needs it.
    uint32_t cpu_cores{};
    // Physical RAM, total (Linux "available" is dominated by page cache).
    // Zero means unknown. Display only; nothing schedules on it.
    uint64_t memory_total_bytes{};
    // The node's own playback budgets: `streaming.startup_timeout_ms` (first
    // transformed fragment) and `streaming.segment_timeout_ms` (hold for an
    // unready fragment). Self-reported so a client bounds its attempt against
    // this node; a shorter client budget abandons work about to succeed.
    // Zero means unreported ("cannot say"), never licence to shorten.
    uint32_t playback_startup_timeout_ms{};
    uint32_t playback_segment_timeout_ms{};
    // Idle lifetimes of a pipeline and of a session. A standby held past the
    // first cannot serve; a deferred close is worth retrying until the second.
    uint32_t playback_pipeline_idle_ms{};
    uint32_t playback_session_idle_ms{};
    // Per-account session cap; published here so a client can judge a standby
    // candidate other than the node it is talking to.
    uint32_t playback_max_sessions_per_account{};
    // How many of one account's sessions may hold a transcode here at once.
    uint32_t playback_max_transcodes_per_account{};
    // Node-wide, all accounts: lets a client tell "node full" from "another
    // screen on this account".
    uint32_t playback_max_sessions{};
    // Idle time after which a session's transcode entitlement is released; a
    // paused client keeps its slot by requesting a stream object (a playlist
    // suffices) within it.
    uint32_t playback_transcode_entitlement_idle_ms{};
    // `start=async`: a start fails only when progress stalls for
    // startup_no_progress_ms; a long-poll waits at most start_wait_max_ms; a
    // failed start stays readable for start_failed_retention_ms. Zero: not offered.
    uint32_t playback_startup_no_progress_ms{};
    uint32_t playback_start_wait_max_ms{};
    uint32_t playback_start_failed_retention_ms{};
    std::vector<TranscodeRate> playback_transcode_rates;
    // The operator's `node_name`, or empty.
    std::string node_name;
    // Per-class cluster traffic; empty when unreported. Rates are over
    // traffic_window_ms since the sender's previous sample (zero on its first).
    std::vector<TrafficClass> traffic;
    uint32_t traffic_window_ms{};

    auto operator<=>(const NodeTelemetry&) const = default;
};

// Grouped so adjacent integers in refresh_local cannot be silently transposed.
struct PlaybackBudgets {
    uint32_t startup_timeout_ms{};
    uint32_t segment_timeout_ms{};
    uint32_t pipeline_idle_ms{};
    uint32_t session_idle_ms{};
    uint32_t max_sessions_per_account{};
    uint32_t max_transcodes_per_account{};
    uint32_t max_sessions{};
    uint32_t transcode_entitlement_idle_ms{};
    uint32_t startup_no_progress_ms{};
    uint32_t start_wait_max_ms{};
    uint32_t start_failed_retention_ms{};
    std::vector<TranscodeRate> transcode_rates;
};

struct CacheActivity {
    uint64_t hits{};
    uint64_t misses{};
    uint64_t evictions{};
};

struct TelemetryView {
    NodeTelemetry telemetry;
    std::chrono::milliseconds age{};
    bool fresh{};
};

Bytes encode_node_telemetry(const NodeTelemetry&);
NodeTelemetry decode_node_telemetry(std::span<const uint8_t>);
Bytes encode_telemetry_set(const std::vector<NodeTelemetry>&);
std::vector<NodeTelemetry> decode_telemetry_set(std::span<const uint8_t>);

class TelemetryStore {
  public:
    // The steady time a record's age is measured by; empty reads Clock::now().
    using Now = std::function<Clock::time_point()>;

  private:
    struct Record {
        NodeTelemetry telemetry;
        Clock::time_point received;
    };

    Now now_;
    mutable Mutex mutex_;
    std::map<NodeId, Record> records_ MACHA_GUARDED_BY(mutex_);
    std::map<NodeId, NodeTelemetry> persisted_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_resets_ MACHA_GUARDED_BY(mutex_);
    // Fixed at construction.
    NodeId self_{};
    NodeId boot_id_{};
    Clock::time_point started_{Clock::now()};
    std::filesystem::path persisted_path_;
    // The telemetry thread's own, written by refresh_local alone.
    Clock::time_point previous_cpu_wall_{Clock::now()};
    std::clock_t previous_cpu_{std::clock()};
    uint64_t sequence_{};
    std::string node_name_ MACHA_GUARDED_BY(mutex_);
    std::optional<TrafficTotals> previous_traffic_ MACHA_GUARDED_BY(mutex_);
    Clock::time_point previous_traffic_at_ MACHA_GUARDED_BY(mutex_){};

  public:
    // Takes effect from the next sample.
    void set_node_name(std::string name) {
        Lock lock(mutex_);
        node_name_ = std::move(name);
    }
    TelemetryStore(NodeId self, std::filesystem::path persisted_path = {}, Now now = {});
    NodeId boot_id() const { return boot_id_; }
    NodeTelemetry refresh_local(const NodeInfo&, std::string version, uint64_t cache_capacity,
                                uint64_t cache_used, uint32_t storage_backends_online,
                                uint32_t peers_known, uint32_t peers_active,
                                uint64_t rpc_connections_created, uint64_t rpc_connections_reused,
                                uint64_t rpc_connections_canonical, NodePhase phase,
                                std::string api_endpoint, PlaybackBudgets playback = {},
                                CacheActivity cache = {},
                                std::optional<TrafficTotals> traffic = {});
    void observe(NodeTelemetry, bool direct = false);
    void apply_identity_reset(const IdentityAssociationReset&);
    std::optional<NodeTelemetry> local() const;
    std::vector<NodeTelemetry> all() const;
    std::vector<NodeTelemetry> recent(std::chrono::milliseconds max_age, size_t max_records = 64) const;
    std::vector<NodeTelemetry> persisted() const;
    void persist();
    std::vector<TelemetryView> views(std::chrono::milliseconds fresh_for) const;
};

} // namespace macha
