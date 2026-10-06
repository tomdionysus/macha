// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/message_routes.hpp"
#include "cluster/node_events.hpp"
#include "contract/metadata_view.hpp"
#include "contract/object_ledger.hpp"
#include "contract/published.hpp"
#include "contract/work.hpp"
#include "ledger/availability.hpp"
#include "types.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace macha {

class DistributedStore;
class LocalState;
class NodeRuntime;

// What is known of one path's extents. A directory sums what is beneath it.
struct PathAvailability {
    bool directory{};
    uint64_t size{};
    uint64_t extents{};
    uint64_t extents_local{};
    // Held by no reachable node.
    uint64_t extents_unavailable{};
    // Not held here, and a peer that might hold them could not be asked.
    uint64_t extents_unknown{};
    // A file's content identity ("macha:<hash>"); empty for a directory.
    std::string hash;
    auto operator<=>(const PathAvailability&) const = default;
};

// One survey's result: immutable once published.
struct AvailabilitySnapshot {
    uint64_t generation{};
    uint64_t surveyed_unix_ms{};
    AvailabilitySurvey survey;
    // Every file and directory, by path.
    std::map<std::string, PathAvailability, std::less<>> paths;
    // Each file's paths, by content identity.
    std::multimap<std::string, std::string, std::less<>> by_hash;
};

// The path table of a namespace against a survey: one walk of the namespace,
// one `held` per extent reference. A file with extents the survey could not
// decide takes its count from `last_known`, by content identity, when that
// decided it: the best this node knows. Throws DecodeError if the namespace
// cannot be read.
void fill_path_table(AvailabilitySnapshot&, const MetadataSnapshot&, const NamespaceNodeStore&,
                     const HeldFn& held, const AvailabilitySnapshot* last_known = nullptr,
                     const std::function<void()>& pause = {});

// The path table of `snapshot` from the table of the namespace it was changed
// from: `previous` with `changes` (before -> after) applied. Equal to
// fill_path_table when neither survey left anything unknown and no extent of
// an unchanged file has changed hands. Costs the changes and one copy of the
// table.
void update_path_table(AvailabilitySnapshot&, const AvailabilitySnapshot& previous,
                       const NamespaceDifferences& changes, const MetadataSnapshot&,
                       const NamespaceNodeStore&, const HeldFn& held);

// A snapshot's path table, generation and survey time as kept on disk; the
// extent lists are not kept. Decoding throws DecodeError.
Bytes encode_availability_paths(const AvailabilitySnapshot&);
AvailabilitySnapshot decode_availability_paths(std::span<const uint8_t>);

// Keeps this node's holdings roll-up and the cluster's availability survey
// current, answers peers' questions about this node's holdings, and tells
// readers what the last survey found. Each survey's path table is kept at
// `persisted`, and read back at construction: until its first survey a node
// answers with the last one it made. Built by the root.
class AvailabilityService {
  public:
    AvailabilityService(NodeRuntime&, LocalState&, DistributedStore&, const ObjectLedger&,
                        const NodeEvents&, MessageRoutes&, std::filesystem::path persisted);
    ~AvailabilityService();
    AvailabilityService(const AvailabilityService&) = delete;
    AvailabilityService& operator=(const AvailabilityService&) = delete;

    // Brings the roll-up and the survey up to `head` if something that could
    // change the answer has happened since the last. The roll-up is rebuilt
    // when the namespace or this node's holdings changed, held to a `share`
    // duty cycle and not begun while the store's presence index is still
    // filling. Peers are asked again when the namespace changed or this node
    // lost something, the membership changed, or a peer's storage shrank; at
    // the duty cycle when a peer's storage grew while something was
    // unavailable or unknown; and when a peer that could not answer answers
    // a one-node probe. A roll-up after a gain alone asks nobody: what this
    // node gained was already available. Returns
    // whether a survey ran. `pause` is called between tree nodes. Reads tree
    // nodes, fetching from peers those this node lacks, and asks peers about
    // theirs. Single owner: the maintenance pass.
    static constexpr Waits refresh_waits =
        Waits::state_device | Waits::data_device | Waits::network | Waits::locks;
    static constexpr ThreadSafety refresh_safety = ThreadSafety::single_owner;
    bool refresh(const MetadataSnapshotView& head, Clock::time_point now, uint64_t now_unix_ms,
                 const std::function<void()>& pause = {});

    // The last survey, or the one read back at construction, or null when
    // there is neither. A pointer copy.
    static constexpr Waits snapshot_waits = Waits::none;
    static constexpr ThreadSafety snapshot_safety = ThreadSafety::thread_safe;
    std::shared_ptr<const AvailabilitySnapshot> snapshot() const { return snapshot_.handle(); }

    // When refresh() next has something to do without a new event: a roll-up
    // its duty cycle deferred, or a peer to ask again. The pass wakes for it.
    std::optional<Clock::time_point> due() const noexcept { return due_; }

    // The namespace's extents this node lacked at the last roll-up, sorted;
    // null before the first. Repair pulls from this rather than walking every
    // referenced extent. A pointer copy.
    static constexpr Waits missing_here_waits = Waits::none;
    static constexpr ThreadSafety missing_here_safety = ThreadSafety::thread_safe;
    std::shared_ptr<const std::vector<ObjectId>> missing_here() const {
        const auto holdings = holdings_.handle();
        return holdings ? holdings->missing : nullptr;
    }

    // Per peer, the namespace's extents this node holds and that peer lacks,
    // sorted, as of the last survey; a peer missing from the map could not
    // say. Repair sends these without asking about each object.
    struct PeerLacks {
        // The metadata generation of the tree the peers were asked about: the
        // lists say nothing about an object added since.
        uint64_t generation{};
        std::map<NodeId, std::shared_ptr<const std::vector<ObjectId>>> lacks;
    };
    static constexpr Waits peer_lacks_waits = Waits::none;
    static constexpr ThreadSafety peer_lacks_safety = ThreadSafety::thread_safe;
    std::shared_ptr<const PeerLacks> peer_lacks() const { return peer_lacks_.handle(); }

    // Whether the last survey found no reachable node holding the extent.
    bool unavailable(const ObjectId& id) const {
        const auto current = snapshot_.handle();
        return current && current->survey.is_unavailable(id);
    }

    // A roll-up or a survey walks the namespace, so the next of each waits
    // this many times the last one's cost: a twentieth of the pass's time.
    static constexpr int share = 20;
    // While the presence index fills, a roll-up would read the device once
    // per extent: it waits, checking this often, for at most this long.
    static constexpr std::chrono::seconds cold_retry{5};
    static constexpr std::chrono::minutes cold_patience{30};
    static constexpr std::chrono::seconds retry_floor{1};
    static constexpr std::chrono::minutes retry_ceiling{5};

  private:
    RpcMessage answer(const RpcMessage& request) const;

    NodeRuntime& node_;
    LocalState& local_;
    DistributedStore& store_;
    const ObjectLedger& ledger_;
    const NodeEvents& events_;
    MessageRoutes& routes_;
    const std::filesystem::path persisted_;

    // This node's holdings, and for a namespace kept inline in its snapshot
    // the tree built from it: every node derives the same tree from the same
    // entries, so its node ids mean the same on a peer.
    struct Holdings {
        HoldingsRollup rollup;
        std::shared_ptr<const MemoryNamespaceNodeStore> built;
        // The DATA store's losses() when the roll-up began: once it moves,
        // a subtree this roll-up calls whole may not be.
        uint64_t losses{};
        // The head it was rolled up at; the survey and the path table read
        // the same one.
        uint64_t generation{};
        std::shared_ptr<const MetadataSnapshot> snapshot;
        // The namespace's extents this node lacked at the roll-up, sorted.
        std::shared_ptr<const std::vector<ObjectId>> missing;
    };
    // What peers are answered from; replaced whole by each roll-up.
    Published<Holdings> holdings_;
    Published<AvailabilitySnapshot> snapshot_;
    Published<PeerLacks> peer_lacks_;

    // The maintenance pass's own: what the last roll-up and survey saw.
    bool surveyed_{};
    // The tree the last survey was of.
    ObjectId surveyed_root_{};
    // The last survey's memo, when every peer answered it.
    std::optional<SurveyMemo> memo_;
    // The tree the published path table is of, when it is of a stored tree,
    // and the storage events seen when it was made.
    std::optional<ObjectId> table_root_;
    uint64_t table_storage_events_{};
    // The head the roll-up was built at: its namespace root, or its record
    // hash when the namespace is inline.
    Hash256 rolled_head_{};
    uint64_t rolled_storage_events_{};
    Clock::time_point rolled_at_{};
    Clock::duration rolled_cost_{};
    Clock::time_point surveyed_at_{};
    Clock::duration surveyed_cost_{};
    std::optional<Clock::time_point> cold_since_;
    // A peer that could not answer is asked again after this, doubling to
    // retry_ceiling; an answer clears it.
    Clock::duration retry_backoff_{};
    std::optional<Clock::time_point> retry_at_;
    std::optional<Clock::time_point> due_;
    uint64_t surveyed_topology_events_{};
    // Each extent-hosting peer and the storage it advertised.
    std::vector<std::pair<NodeId, uint64_t>> surveyed_peers_;
};

} // namespace macha
