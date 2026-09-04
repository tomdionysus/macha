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
    // Where other nodes should reach this node's HTTP API — distinct from
    // host/port above, which is the RPC bind address. Empty/zero means the
    // sender doesn't run (or hasn't yet reported) an advertised API address.
    std::string api_host;
    uint16_t api_port{};

    auto operator<=>(const NodeTelemetry&) const = default;
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
                                std::string api_host, uint16_t api_port);
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
