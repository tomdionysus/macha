// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/message_routes.hpp"
#include "cluster/node_events.hpp"
#include "contract/metadata_view.hpp"
#include "contract/object_ledger.hpp"
#include "contract/published.hpp"
#include "contract/thread_safety.hpp"
#include "contract/work.hpp"
#include "ledger/availability.hpp"
#include "metadata/namespace_tree.hpp"
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

// Keeps the cluster's availability survey current, answers peers' diffs of
// this node's held trie, and tells readers what the last survey found. Each
// survey's path table is kept at `persisted`, and read back at construction:
// until its first survey a node answers with the last one it made. Built by
// the root.
class AvailabilityService {
  public:
    AvailabilityService(NodeRuntime&, LocalState&, DistributedStore&, const ObjectLedger&,
                        const NodeEvents&, MessageRoutes&, std::filesystem::path persisted);
    ~AvailabilityService();
    AvailabilityService(const AvailabilityService&) = delete;
    AvailabilityService& operator=(const AvailabilityService&) = delete;

    // Brings the survey up to `head` if something that could change the
    // answer has happened since the last. What this node lacks is the
    // inventory's referenced set joined against its held trie; each peer's
    // held trie is diffed against this node's. Peers are asked again when the
    // membership changed or a peer's storage shrank; when the inventory or
    // this node's holdings changed, held to a `share` duty cycle; at the duty
    // cycle when a peer's storage grew while something was unavailable or
    // unknown; and after a backoff when a peer could not answer. A gain alone
    // asks nobody: what this node gained was already available. Nothing is
    // surveyed while a disk is being seeded or before the first inventory.
    // Returns whether peers were asked. `pause` is called between pages and
    // rounds. Single owner: the maintenance pass.
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

    // When refresh() next has something to do without a new event: a survey
    // its duty cycle deferred, or a peer to ask again. The pass wakes for it.
    std::optional<Clock::time_point> due() const noexcept { return due_; }

    // What is referenced that this node lacked at the last survey, sorted;
    // null before the first. Repair pulls from this rather than walking every
    // referenced object. A pointer copy.
    static constexpr Waits missing_here_waits = Waits::none;
    static constexpr ThreadSafety missing_here_safety = ThreadSafety::thread_safe;
    std::shared_ptr<const std::vector<ObjectId>> missing_here() const {
        return missing_.handle();
    }

    // Per peer, the namespace's extents this node holds and that peer lacks,
    // sorted, as of the last survey; a peer missing from the map could not
    // say. Repair sends these without asking about each object.
    struct PeerLacks {
        // The inventory's generation: the lists say nothing about an object
        // referenced since.
        uint64_t generation{};
        std::map<NodeId, std::shared_ptr<const std::vector<ObjectId>>> lacks;
        // Every extent-hosting peer answered.
        bool complete{};
    };
    static constexpr Waits peer_lacks_waits = Waits::none;
    static constexpr ThreadSafety peer_lacks_safety = ThreadSafety::thread_safe;
    std::shared_ptr<const PeerLacks> peer_lacks() const { return peer_lacks_.handle(); }

    // Whether the last survey found no reachable node holding the extent.
    bool unavailable(const ObjectId& id) const {
        const auto current = snapshot_.handle();
        return current && current->survey.is_unavailable(id);
    }

    // A survey reads the whole referenced set, so the next waits this many
    // times the last one's cost: a twentieth of the pass's time.
    static constexpr int share = 20;
    // While a disk is being seeded the holdings cannot be named: the pass
    // looks again this often.
    static constexpr std::chrono::seconds seed_retry{5};
    static constexpr std::chrono::seconds retry_floor{1};
    static constexpr std::chrono::minutes retry_ceiling{5};

  private:
    RpcMessage answer_trie(const RpcMessage& request) const;

    NodeRuntime& node_;
    LocalState& local_;
    DistributedStore& store_;
    const ObjectLedger& ledger_;
    const NodeEvents& events_;
    MessageRoutes& routes_;
    const std::filesystem::path persisted_;

    // The held tries pinned for peers' diffs, by root hash, newest last: a
    // diff's every round reads the snapshot its root question pinned. One is
    // dropped once no question has used it for pinned_for.
    struct Pinned {
        Hash256 hash;
        std::shared_ptr<const TrieUnion> trie;
        Clock::time_point used;
    };
    static constexpr size_t pinned_max = 4;
    static constexpr std::chrono::minutes pinned_for{2};
    mutable Mutex pinned_mutex_;
    mutable std::vector<Pinned> pinned_ MACHA_GUARDED_BY(pinned_mutex_);
    Published<AvailabilitySnapshot> snapshot_;
    Published<PeerLacks> peer_lacks_;
    Published<std::vector<ObjectId>> missing_;

    // The maintenance pass's own: what the last survey saw.
    bool surveyed_{};
    // The inventory and holdings `missing_` was joined from.
    std::shared_ptr<const InventoryHorizon> joined_inventory_;
    HeldIdentity joined_identity_;
    // Each answering peer's held trie at the last survey; empty when one did
    // not answer. With these and this node's holdings unchanged, what is
    // unavailable changes only with what is referenced.
    std::map<NodeId, Hash256> surveyed_roots_;
    // The tree the published path table is of, when it is of a stored tree,
    // the holdings it was made from, and the peers' tries it was made with.
    std::optional<ObjectId> table_root_;
    Hash256 table_holdings_{};
    std::map<NodeId, Hash256> table_roots_;
    Clock::time_point surveyed_at_{};
    Clock::duration surveyed_cost_{};
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
