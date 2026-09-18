// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <chrono>
#include <filesystem>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// Mirrors the local-node "startup.phase" vocabulary already reported by the
// Status API's root object (ClusterStatusService::status_response), so a peer
// observed via telemetry and this node's own startup phase read the same way.
// "ready" is the default: a legacy sender/record that predates this field, or
// one truncated on decode, reports itself as ready rather than perpetually
// "recovering", preserving prior behavior for anything that omits it.
enum class NodePhase : uint8_t { starting, recovering, ready };

std::string_view node_phase_name(NodePhase);

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
    // Where clients should reach this node's HTTP API, as a complete URL --
    // distinct from host/port above, which is the RPC address and is never
    // proxied. Empty means the sender doesn't run (or hasn't yet reported)
    // an API endpoint. Carries a scheme precisely because the API may sit
    // behind a TLS-terminating proxy while binding plain HTTP internally, so
    // neither scheme nor port is derivable from anything else here.
    std::string api_endpoint;
    // Hardware threads on this node. Zero means the sender did not report one,
    // which a consumer must treat as "no opinion" rather than as zero cores:
    // load1 and process_cpu_percent are both per-core quantities, and this
    // cluster is deliberately non-uniform hardware, so comparing either
    // between nodes without it compares nothing.
    uint32_t cpu_cores{};
    // Physical RAM on this node. Total rather than available: on Linux
    // "available" is dominated by page cache, so a node that has just served a
    // large file looks starved while being perfectly healthy -- and serving
    // large files is the whole workload here. Zero means the sender could not
    // determine it, which a consumer must render as unknown rather than as a
    // node with no memory. Display only; nothing schedules or ranks on it,
    // since total RAM would prefer a large thrashing node over a small idle
    // one.
    uint64_t memory_total_bytes{};
    // The budgets this node itself enforces on a playback request: how long it
    // may take to bring the first transformed fragment up
    // (`streaming.startup_timeout_ms`), and how long it holds a request for a
    // fragment that is not ready yet (`streaming.segment_timeout_ms`).
    //
    // Reported because a client has to bound its own attempt against the node
    // it is actually talking to, including nodes it has never used -- these
    // are self-reported facts, relayed like load1 and cpu_cores, not a
    // cluster-wide figure any node is entitled to compute. A client that
    // hardcodes a budget below a node's own entitlement abandons that node
    // while it is still working: measured on 2026-09-18, a 12 s client budget
    // against this 15 s one threw away an 11.7 s transcode that was about to
    // succeed and started the identical encode on the other node.
    //
    // Zero means the sender did not report one -- an older node, or one with
    // streaming disabled -- and a consumer must read that as "cannot say",
    // never as licence to shorten its own budget. A default would be
    // indistinguishable at runtime from an answer.
    uint32_t playback_startup_timeout_ms{};
    uint32_t playback_segment_timeout_ms{};

    auto operator<=>(const NodeTelemetry&) const = default;
};

// Passed as a group rather than as two more positional integers into an
// already long refresh_local signature, where a transposition would be silent.
struct PlaybackBudgets {
    uint32_t startup_timeout_ms{};
    uint32_t segment_timeout_ms{};
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
    struct Record {
        NodeTelemetry telemetry;
        Clock::time_point received{Clock::now()};
    };

    mutable std::mutex mutex_;
    std::map<NodeId, Record> records_;
    std::map<NodeId, NodeTelemetry> persisted_;
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_resets_;
    NodeId self_{};
    NodeId boot_id_{};
    Clock::time_point started_{Clock::now()};
    Clock::time_point previous_cpu_wall_{Clock::now()};
    std::clock_t previous_cpu_{std::clock()};
    uint64_t sequence_{};
    std::filesystem::path persisted_path_;

  public:
    TelemetryStore(NodeId self, std::filesystem::path persisted_path = {});
    NodeId boot_id() const { return boot_id_; }
    NodeTelemetry refresh_local(const NodeInfo&, std::string version, uint64_t cache_capacity,
                                uint64_t cache_used, uint32_t storage_backends_online,
                                uint32_t peers_known, uint32_t peers_active,
                                uint64_t rpc_connections_created, uint64_t rpc_connections_reused,
                                uint64_t rpc_connections_canonical, NodePhase phase,
                                std::string api_endpoint, PlaybackBudgets playback = {});
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
