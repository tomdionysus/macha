// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/walk.hpp"
#include "metadata/metadata.hpp"
#include "types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// A content-addressed Merkle tree over the namespace, keyed by path. A
// tree-backed MetadataRecord carries only its root; nodes are CONTROL objects,
// stored to the write floor before any record names the root.
//
// Two load-bearing, tested properties:
//
// 1. **History independence.** The same entry set yields the same root however
//    it was reached, so nodes that reconcile to the same namespace agree on the
//    root and root comparison is a valid divergence witness. Hence boundaries
//    are chosen by hashing keys, not by fill factor.
//
// 2. **Boundaries depend on keys only, never on values.** A value change
//    rewrites exactly one leaf and its path to the root. `max_fanout` caps the
//    oversized nodes a pathological key set could make; a cap on item *count*
//    is still a function of the sorted keys and preserves property 1, a cap on
//    encoded *bytes* would not, so there is none.
//
// A leaf holds stat data plus either a short inline extent list or the root of
// an extent sequence, so leaf size is bounded by the key set, not by extents.
struct NamespaceTreeLimits {
    // Average entries per leaf: the boundary test fires with probability
    // 1/target per key, so sizes vary geometrically.
    size_t entry_target_fanout{32};
    // Hard cap on entries per leaf. Count-based, so still key-determined.
    size_t entry_max_fanout{128};
    size_t branch_target_fanout{32};
    size_t branch_max_fanout{128};
    // At or below this many extents the list is inline in the leaf (8 x 49
    // bytes = 392), keeping an ordinary small file to a single node.
    size_t extent_inline_max{8};
    size_t extent_target_fanout{256};
    size_t extent_max_fanout{1024};
};

// Where tree nodes are read and written. The production implementation is the
// content-addressed control store, which supplies replication, repair and GC.
class NamespaceNodeStore {
  public:
    virtual ~NamespaceNodeStore() = default;
    // Returns the node's content address. Storing the same bytes twice returns
    // the same id and is not an error.
    virtual ObjectId put(std::span<const uint8_t> node) = 0;
    virtual std::optional<Bytes> get(const ObjectId& id) const = 0;
};

// An in-memory store, for tests and offline measurement.
class MemoryNamespaceNodeStore final : public NamespaceNodeStore {
  public:
    ObjectId put(std::span<const uint8_t> node) override;
    std::optional<Bytes> get(const ObjectId& id) const override;
    // Stores bytes under an id they do NOT hash to, simulating a corrupt
    // control-store object. Tests only.
    void put_at(const ObjectId& id, Bytes node);

    size_t nodes() const {
        return nodes_.size();
    }
    // Reads served since the last `forget_reads`; lets tests prove a stat-only
    // read fetches no extent node.
    size_t reads() const {
        return reads_;
    }
    void forget_reads() {
        reads_ = 0;
    }
    uint64_t bytes() const {
        return bytes_;
    }
    // Ids newly written (not already present): the dirty set a commit would
    // replicate.
    const std::vector<ObjectId>& written() const {
        return written_;
    }
    void forget_written() {
        written_.clear();
    }

  private:
    std::map<ObjectId, Bytes> nodes_;
    std::vector<ObjectId> written_;
    uint64_t bytes_{};
    mutable size_t reads_{};
};

struct NamespaceTreeStats {
    uint64_t entries{};
    uint64_t extents{};
    uint64_t leaves{};
    uint64_t branches{};
    uint64_t extent_nodes{};
    uint64_t bytes{};
    uint64_t largest_node_bytes{};
    size_t depth{};
};

// Builds the tree and returns the root id. Deterministic in the entry set alone.
ObjectId build_namespace_tree(const std::map<std::string, FsEntry>& entries, NamespaceNodeStore& store,
                              const NamespaceTreeLimits& limits = {});

// Visits entries in path order without materialising the namespace. Full-scan
// readers should use this rather than `read_namespace_tree`.
using NamespaceVisitor = std::function<void(const std::string& path, const FsEntry& entry)>;
void walk_namespace_tree(const ObjectId& root, const NamespaceNodeStore& store,
                         const NamespaceVisitor& visit);

// Every entry whose path starts with `prefix`, in path order. On a tree, a
// branch child whose key range cannot intersect the prefix is never fetched,
// so a listing costs the subtree rather than the library.
void for_each_namespace_entry_with_prefix(const MetadataSnapshot& snapshot,
                                          const NamespaceNodeStore* store,
                                          std::string_view prefix,
                                          const NamespaceVisitor& visit);

// The entries after `from`, in path order, one budget operation per entry (the
// spec's `entries(view, cursor, budget)`, B2). On a tree, subtrees ending at or
// before the cursor are skipped, so a resumed page reads only its own path. At
// the end the cursor is the start again and the page is complete. Throws if a
// node cannot be fetched.
using NamespaceItem = std::pair<std::string, FsEntry>;
Page<NamespaceItem, std::string> namespace_entries(const MetadataSnapshot& snapshot,
                                                   const NamespaceNodeStore* store,
                                                   Cursor<std::string> from, Budget& budget);

// The namespace of a snapshot in either form: the inline map, or the tree when
// the snapshot carries a root. Throws if the snapshot is detached and no store
// is supplied, so a detached namespace is never read as empty.
void for_each_namespace_entry(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              const NamespaceVisitor& visit);

// The same walk, also appending to `nodes` every tree node it read: branches,
// leaves and extent spines, which a complete walk reads all of. Nothing for an
// inline namespace. A node that cannot be read throws, so a walk that returns
// has named the whole tree.
void for_each_namespace_entry(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              const NamespaceVisitor& visit, std::vector<ObjectId>& nodes);

// One path from a snapshot in either form. On a tree this fetches at most
// `depth` nodes, and no extent node when `with_extents` is false. Throws if the
// snapshot is detached and no store is supplied.
std::optional<FsEntry> namespace_entry(const MetadataSnapshot& snapshot,
                                       const NamespaceNodeStore* store, std::string_view path,
                                       bool with_extents = true);

// Existence only, for FUSE path resolution: copies no entry and fetches no
// extent node.
bool namespace_contains(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                        std::string_view path);

// Whether two snapshots hold different namespaces: a root comparison for trees,
// a map comparison for maps. The FUSE frontend and catalogue scanner wake on
// this; comparing the (empty) entry maps of tree-backed snapshots would never
// report a change.
bool namespace_differs(const MetadataSnapshot& a, const MetadataSnapshot& b);

// One direct child of a tree node: another tree node (a branch, a leaf or an
// extent node) or a DATA extent.
struct NamespaceTreeChild {
    bool extent{};
    ObjectId id{};
    auto operator<=>(const NamespaceTreeChild&) const = default;
};

// A node's direct children in the order the node lists them. A leaf's are
// each entry's inline extents or its extent sequence's root; holes are not
// children. A function of the node's bytes alone, so every holder of the node
// numbers its children alike. Throws DecodeError on anything else.
std::vector<NamespaceTreeChild> namespace_tree_children(std::span<const uint8_t> node);

// A change set: an entry to upsert, or nullopt to delete the path.
using NamespaceChanges = std::map<std::string, std::optional<FsEntry>>;

// Applies a change set to an existing tree and returns the new root. Untouched
// entries are never read or rewritten. The result is byte-identical to
// `build_namespace_tree` over the resulting namespace (property 1).
//
// The spine above changed leaves is recomputed over the whole leaf sequence
// rather than spliced: unchanged branches re-encode to the same address, so
// written nodes are only the changed leaves and their paths; the local cost is
// O(branch nodes) reads and encodes, plus the leaves the changes fall in:
// leaves between distant changes are not read.
ObjectId update_namespace_tree(const ObjectId& root, NamespaceNodeStore& store,
                               const NamespaceChanges& changes,
                               const NamespaceTreeLimits& limits = {});

// A path whose entry differs between two trees; absent on a side is none.
struct NamespaceDifference {
    std::optional<FsEntry> before;
    std::optional<FsEntry> after;
};
using NamespaceDifferences = std::map<std::string, NamespaceDifference>;
// Every path whose entry differs, extents included. Reads both trees' branch
// nodes and the leaves only one of them has: the cost follows what differs.
// Throws DecodeError if a node cannot be read.
NamespaceDifferences diff_namespace_trees(const ObjectId& before, const ObjectId& after,
                                          const NamespaceNodeStore& store);

// The three-way merge of tree-backed snapshots, without materialising them:
// `merge_metadata_snapshots` over the paths that differ from `base` on either
// branch. `merged.snapshot` carries neither entries nor a root; its namespace
// is `changes` applied to the tree `onto` (update_namespace_tree), the same
// root a merge of the materialised namespaces would build. `onto` is the tree
// of the lower head, the merge commit's primary parent.
struct NamespaceTreeMerge {
    MetadataMergeResult merged;
    ObjectId onto{};
    NamespaceChanges changes;
};
NamespaceTreeMerge merge_tree_backed_snapshots(const MetadataSnapshot& base,
                                               const MetadataSnapshot& left,
                                               const MetadataSnapshot& right,
                                               const Hash256& left_head, const Hash256& right_head,
                                               const NamespaceNodeStore& store);

// The merge commit as a delta against its primary parent: what differs outside
// the namespace, and `changes` as the namespace's. `merged` is the merge's
// final snapshot. None when a delta cannot say it (metadata_delta).
std::optional<MetadataDelta> tree_merge_delta(const MetadataSnapshot& primary,
                                              const MetadataSnapshot& merged,
                                              const NamespaceChanges& changes);

// Applies a commit's delta to the tree: erases, upserts, then appends, in the
// order `apply_metadata_delta_in_place` uses. An append whose base extent count
// does not match the tree's entry is refused, as on the map path.
ObjectId apply_delta_to_namespace_tree(const ObjectId& root, NamespaceNodeStore& store,
                                       const MetadataDelta& delta,
                                       const NamespaceTreeLimits& limits = {});

// Materialises the whole namespace. For round-trip proofs; nothing on the hot
// path should need it.
std::map<std::string, FsEntry> read_namespace_tree(const ObjectId& root, const NamespaceNodeStore& store,
                                                   const NamespaceTreeLimits& limits = {});

// One path in O(log n) node fetches. With `with_extents` false (stat-only, what
// FUSE lookup and listing want) it fetches the path to the leaf and no extent
// spine at all.
std::optional<FsEntry> namespace_tree_lookup(const ObjectId& root, std::string_view path,
                                             const NamespaceNodeStore& store,
                                             bool with_extents = true);

NamespaceTreeStats namespace_tree_stats(const ObjectId& root, const NamespaceNodeStore& store);

// Every control object in the namespace: branches, leaves and extent spines,
// for the reachability live set. Throws if a node cannot be read: an incomplete
// live set must not be deleted against.
void collect_namespace_tree_nodes(const ObjectId& root, const NamespaceNodeStore& store,
                                  std::vector<ObjectId>& out);

// Nodes reachable from `after` that `before` does not share: what a commit must
// claim. Subtrees with the same id on both sides are pruned unread. A changed
// leaf contributes every extent spine it addresses, over-collecting unchanged
// neighbours' spines -- the safe direction. No `before` means everything is new.
void collect_namespace_tree_changes(const std::optional<ObjectId>& before, const ObjectId& after,
                                    const NamespaceNodeStore& store, std::vector<ObjectId>& out);

// A namespace being mutated, in either form. A batch must see its own earlier
// edits (mkdir /a then create /a/b), and a tree cannot be edited per entry, so
// the delta is overlaid on it: a path in `upsert_entries` reads as that entry,
// one in `erase_entries` reads as absent, anything else reads from beneath.
//
// On a map-backed snapshot it also writes through to `entries`, since the SM13
// payload is encoded from that map.
class NamespaceWorkingSet {
  public:
    NamespaceWorkingSet(MetadataSnapshot& snapshot, MetadataDelta& delta,
                        const NamespaceNodeStore* nodes)
        : snapshot_(snapshot), delta_(delta), nodes_(nodes),
          tree_backed_(snapshot.namespace_root.has_value()) {}

    bool tree_backed() const noexcept {
        return tree_backed_;
    }

    std::optional<FsEntry> get(const std::string& path, bool with_extents = true) const;
    bool contains(const std::string& path) const;
    void put(const std::string& path, const FsEntry& entry);
    void erase(const std::string& path);

    // The first path strictly after `directory` that lies under it: the
    // emptiness test, since a directory's children follow it in path order.
    std::optional<std::string> first_path_under(const std::string& directory) const;

    // Every path under `directory` with its entry, in path order, extents
    // included: what a rename moves.
    std::vector<std::pair<std::string, FsEntry>> subtree(const std::string& directory) const;

  private:
    bool erased(const std::string& path) const;

    MetadataSnapshot& snapshot_;
    MetadataDelta& delta_;
    const NamespaceNodeStore* nodes_;
    bool tree_backed_;
};

// What a migration would do to one node, built and verified but not installed.
struct NamespaceMigration {
    MetadataRecord record; // the tree-backed head to install
    ObjectId root{};
    NamespaceTreeStats stats;
    size_t entries{};
    uint64_t previous_payload_bytes{};
};

// Builds the tree for `head`'s namespace into `nodes`, verifies it entry by
// entry, and returns the record that would replace the head. Installs nothing.
// A pure function of the head, so every node computes the same root and record
// hash without coordination (what makes `--expect-hash` meaningful). Throws if
// the tree does not read back as the namespace it was built from.
NamespaceMigration plan_namespace_migration(const MetadataRecord& head, NamespaceNodeStore& nodes);

// The only conversions between namespace forms. `detach_namespace` builds the
// tree into `store` and returns the snapshot with `namespace_root` set and
// `entries` empty (the form `encode_snapshot_v14` accepts); `attach_namespace`
// materialises it back into the map. By value so callers can move the map in.
// Hot paths should use lookups and prefix scans, not `attach_namespace`.
MetadataSnapshot detach_namespace(MetadataSnapshot snapshot, NamespaceNodeStore& store,
                                  const NamespaceTreeLimits& limits = {});
MetadataSnapshot attach_namespace(MetadataSnapshot snapshot, const NamespaceNodeStore& store,
                                  const NamespaceTreeLimits& limits = {});

} // namespace macha
