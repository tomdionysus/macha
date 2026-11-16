// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/metadata_view.hpp"
#include "contract/published.hpp"
#include "contract/thread_safety.hpp"
#include "contract/work.hpp"
#include "ledger/availability.hpp"
#include "metadata/namespace_tree.hpp"
#include "types.hpp"

#include "dagda/contract/object_ledger.h"
#include "dagda/store/availability.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dagda {
class Context;
}

namespace macha {

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

// The cluster's availability as Macha's APIs answer it: the store node's
// survey (what no reachable node holds, what could not be decided) with
// Macha's path table over the namespace. Each table is kept at `persisted` and
// read back at construction: until its first refresh a node answers with the
// last one it made. Built by the root.
class AvailabilityService {
  public:
    // `ledger` is the store node's: this node's holdings.
    AvailabilityService(dagda::Context&, const dagda::contract::ObjectLedger& ledger,
                        std::filesystem::path persisted);
    AvailabilityService(const AvailabilityService&) = delete;
    AvailabilityService& operator=(const AvailabilityService&) = delete;

    // Brings the path table up to `head` and `survey` (the store node's
    // last): kept when the tree, this node's holdings and the survey's answer
    // are as the table was made; followed through the tree's change when only
    // the tree moved and the survey left nothing undecided; walked otherwise.
    // Nothing while this node's holdings cannot be named (a disk being
    // seeded) or before the first survey. Returns whether the table changed.
    // `pause` is called between the entries a walk reads. Single owner: the
    // maintenance pass.
    static constexpr Waits refresh_waits = Waits::state_device | Waits::data_device | Waits::locks;
    static constexpr ThreadSafety refresh_safety = ThreadSafety::single_owner;
    bool refresh(const MetadataSnapshotView& head,
                 const std::shared_ptr<const dagda::store::AvailabilitySnapshot>& survey,
                 const std::function<void()>& pause = {});

    // The last table, or the one read back at construction, or null when
    // there is neither. A pointer copy.
    static constexpr Waits snapshot_waits = Waits::none;
    static constexpr ThreadSafety snapshot_safety = ThreadSafety::thread_safe;
    std::shared_ptr<const AvailabilitySnapshot> snapshot() const {
        return snapshot_.handle();
    }

    // Whether the last survey found no reachable node holding the extent.
    bool unavailable(const ObjectId& id) const {
        const auto current = snapshot_.handle();
        return current && current->survey.is_unavailable(id);
    }

  private:
    dagda::Context& store_;
    const dagda::contract::ObjectLedger& ledger_;
    const std::filesystem::path persisted_;
    Published<AvailabilitySnapshot> snapshot_;

    // The maintenance pass's own: what the published table was made from.
    bool tabled_{};
    std::shared_ptr<const dagda::store::AvailabilitySnapshot> table_survey_;
    std::optional<ObjectId> table_root_;
    Hash256 table_holdings_{};
};

} // namespace macha
