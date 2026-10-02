// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A torrent the cluster was asked to download: the durable half of a job, in
// MetadataSnapshot::torrent_requests. Only rarely changing state lives here
// (request, claim, intent, phase); never progress or rates, as each metadata
// write costs seconds. History can branch (docs/metadata.md), so
// merge_torrent_request() joins deterministically and needs no operator.

#include "codec.hpp"
#include "types.hpp"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace macha {

// Where a request stands in the cluster. Ordered: a merge never moves a
// request backwards within one claim epoch.
enum class TorrentPhase : uint8_t {
    awaiting_node = 0,
    downloading = 1,
    importing = 2,
    completed = 3,
    failed = 4,
    cancelled = 5,
};
std::string_view torrent_phase_name(TorrentPhase) noexcept;
std::optional<TorrentPhase> parse_torrent_phase(std::string_view);
bool torrent_phase_terminal(TorrentPhase) noexcept;

// What the operator asked for. `cancelled` is final.
enum class TorrentDesired : uint8_t {
    active = 0,
    paused = 1,
    cancelled = 2,
};
std::string_view torrent_desired_name(TorrentDesired) noexcept;

struct TorrentClaim {
    NodeId node_id{};
    // Starts at 1. A lapsed claim is taken over at epoch + 1.
    uint64_t epoch{};
    uint64_t claimed_unix_ms{};
    auto operator<=>(const TorrentClaim&) const = default;
};

struct TorrentRequest {
    // Set once, by the node that took the add.
    std::string id;         // 32 lowercase hex, the job id clients use
    std::string info_hash;  // v1 hex, else v2; the cluster-wide duplicate key
    std::string source;     // canonical magnet, or "object:<hex>" for stored metainfo
    uint64_t created_unix_ms{};
    NodeId created_by{};

    // Operator settings, changed by PATCH: last write wins by
    // settings_changed_unix_ms, then node id.
    std::optional<NodeId> pinned_node_id;
    std::optional<uint64_t> remove_after_ms;
    uint64_t settings_changed_unix_ms{};
    NodeId settings_changed_by{};

    // Operator intent: cancelled dominates; otherwise last write wins by
    // desired_changed_unix_ms, then node id.
    TorrentDesired desired{TorrentDesired::active};
    uint64_t desired_changed_unix_ms{};
    NodeId desired_changed_by{};

    // Who runs it: the higher epoch wins; within an epoch the earlier claim,
    // then the lower node id. Empty while awaiting a node.
    std::optional<TorrentClaim> claim;

    // Written by the owner of `claim` (or by anyone for awaiting_node and
    // cancelled). Belongs to claim epoch `phase_epoch`; a merge takes the
    // record with the higher (phase_epoch, phase, progress_unix_ms).
    TorrentPhase phase{TorrentPhase::awaiting_node};
    uint64_t phase_epoch{};
    uint64_t progress_unix_ms{};
    std::string name;
    uint64_t bytes_total{};
    std::string ingest_job_id;
    std::string error_code;
    std::string error;
    uint64_t completed_unix_ms{};

    // A tombstone, so a branch that missed the removal cannot resurrect the
    // request; erased after its grace period.
    uint64_t removed_unix_ms{};

    auto operator<=>(const TorrentRequest&) const = default;
};

void encode_torrent_request(Writer&, const TorrentRequest&);
TorrentRequest decode_torrent_request(Reader&);

// The join of two replicas' versions of one request. Commutative,
// associative and idempotent.
TorrentRequest merge_torrent_request(const TorrentRequest& a, const TorrentRequest& b);

// Merges two collections from a common base. A key erased on one side stays
// erased unless the other changed it since the base. Of two live requests for
// one info hash, the earlier (created_unix_ms, id) keeps it and the other is
// cancelled as `duplicate_torrent`.
std::map<std::string, TorrentRequest, std::less<>>
merge_torrent_requests(const std::map<std::string, TorrentRequest, std::less<>>& base,
                       const std::map<std::string, TorrentRequest, std::less<>>& left,
                       const std::map<std::string, TorrentRequest, std::less<>>& right);

// Hard bounds, checked at decode.
inline constexpr uint32_t max_torrent_requests = 65536;
inline constexpr size_t max_torrent_request_text = 8192;

} // namespace macha
