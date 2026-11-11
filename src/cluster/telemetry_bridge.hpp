// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Macha's telemetry in Dannan's record: the core fields Dannan fills, and Macha's own as
// hosted fields 1024 to 1045 (first cutover, section 6), written here and read back into
// NodeTelemetry for Status and the views.

#include "cluster/telemetry.hpp"

#include <dannan/status/telemetry.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace macha {

namespace hosted_field {
inline constexpr uint16_t storage_capacity = 1024;
inline constexpr uint16_t storage_used = 1025;
inline constexpr uint16_t cache_capacity = 1026;
inline constexpr uint16_t cache_used = 1027;
inline constexpr uint16_t metadata_generation = 1028;
inline constexpr uint16_t storage_backends_online = 1029;
inline constexpr uint16_t api_endpoint = 1030;
inline constexpr uint16_t playback_startup_timeout_ms = 1031;
inline constexpr uint16_t playback_segment_timeout_ms = 1032;
inline constexpr uint16_t playback_pipeline_idle_ms = 1033;
inline constexpr uint16_t playback_session_idle_ms = 1034;
inline constexpr uint16_t playback_max_sessions_per_account = 1035;
inline constexpr uint16_t playback_max_sessions = 1036;
inline constexpr uint16_t playback_transcode_entitlement_idle_ms = 1037;
inline constexpr uint16_t cache_hits = 1038;
inline constexpr uint16_t cache_misses = 1039;
inline constexpr uint16_t cache_evictions = 1040;
inline constexpr uint16_t playback_max_transcodes_per_account = 1041;
inline constexpr uint16_t playback_startup_no_progress_ms = 1042;
inline constexpr uint16_t playback_start_wait_max_ms = 1043;
inline constexpr uint16_t playback_start_failed_retention_ms = 1044;
inline constexpr uint16_t playback_transcode_rates = 1045;
} // namespace hosted_field

// The hosted fields in their order: NodeConfig::telemetry_layout.
const std::vector<uint16_t>& hosted_telemetry_layout();

// What this node reports that Dannan does not know.
struct HostedTelemetry {
    uint64_t storage_capacity{};
    uint64_t storage_used{};
    uint64_t cache_capacity{};
    uint64_t cache_used{};
    uint64_t metadata_generation{};
    uint32_t storage_backends_online{};
    std::string api_endpoint;
    PlaybackBudgets playback;
    CacheActivity cache;
};

// Writes the hosted fields; a zero or empty value is not written, as Macha's encoder
// never wrote one. At most 256 transcode rates.
void write_hosted_telemetry(dannan::status::Telemetry& record, const HostedTelemetry& hosted);

// A record as Macha's NodeTelemetry: the core fields and the hosted ones. Nothing when a
// field cannot be read (a wrong width, a transcode rate past its bounds): one bad record
// is dropped, never the set it came in.
std::optional<NodeTelemetry> node_telemetry_from(const dannan::status::Telemetry& record);

} // namespace macha
