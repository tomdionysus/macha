// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "metadata.hpp"
#include "types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace macha {

// A content-addressed Merkle tree over the namespace, keyed by path.
//
// This is SM14 (see TODO/2026-09-17-namespace-merkle-root-plan.md). A
// tree-backed MetadataRecord carries only the root of this tree instead of
// inlining the namespace; its nodes are CONTROL objects, stored to the write
// floor before any record names the root.
//
// Two properties are load-bearing and both are tested:
//
// 1. **History independence.** The same entry set produces the same root
//    whatever order it was reached in -- inserted, deleted back down to, or
//    merged into. That is not a nicety: once `metadata_namespace_signature` and
//    `cache_record`'s `entries != entries` witness become root comparisons, two
//    nodes that independently reconcile to the same namespace must agree on the
//    root, or the witness reports divergence that does not exist. An
//    incrementally built fixed-fanout B-tree does not have this property, which
//    is why node boundaries here are chosen by hashing keys rather than by fill
//    factor.
//
// 2. **Boundaries depend on keys only, never on values.** Every namespace write
//    is a value change on an existing path -- a size, an mtime, an extent
//    appended -- and under a conventional content-defined chunking rule (hash
//    the whole serialised item, as Noms and Dolt do) a value change can move a
//    boundary and rewrite its neighbours. Hashing the key alone means a value
//    change rewrites exactly one leaf and the path to the root, and nothing
//    else. The cost is that a pathological key set can make an oversized node,
//    which `max_fanout` caps -- a cap on item *count* is still a pure function
//    of the sorted key sequence, so it preserves property 1. A cap on encoded
//    *bytes* would not, and there deliberately is none.
//
// Extents do not live in the leaf. A leaf holds stat data plus either a short
// inline extent list or the root of an extent sequence, so a leaf stays bounded
// by the key set and a 12,500-extent film does not put 600 KB inside one. That
// is what makes Stage D (demand-loaded extents) a change of when a node is
// fetched rather than another format change.
struct NamespaceTreeLimits {
    // Average entries per leaf. The boundary test fires with probability
    // 1/target per key, so leaves average this and vary geometrically.
    size_t entry_target_fanout{32};
    // Hard cap on entries per leaf. Count-based, so still key-determined.
    size_t entry_max_fanout{128};
    size_t branch_target_fanout{32};
    size_t branch_max_fanout{128};
    // At or below this many extents, the list is written inside the leaf. Eight
    // extents is 392 encoded bytes at the 49 bytes/extent the snapshot already
    // costs, which keeps an ordinary small file to a single node.
    size_t extent_inline_max{8};
    size_t extent_target_fanout{256};
    size_t extent_max_fanout{1024};
};

// Where tree nodes are read and written. The intended implementation is the
// existing content-addressed control store -- the path catalogue shards already
// take, via `replicate_control`/`ensure_control_local`, which inherits
// replication, repair and GC rather than adding a second durability model. The
// interface is here so the tree can be built and measured without a cluster.
class NamespaceNodeStore {
  public:
    virtual ~NamespaceNodeStore() = default;
    // Stores the node and returns its content address. Storing the same bytes
    // twice must return the same id and is not an error.
    virtual ObjectId put(std::span<const uint8_t> node) = 0;
    virtual std::optional<Bytes> get(const ObjectId& id) const = 0;
};

// An in-memory store, for tests and for offline measurement of a real head.
class MemoryNamespaceNodeStore final : public NamespaceNodeStore {
  public:
    ObjectId put(std::span<const uint8_t> node) override;
    std::optional<Bytes> get(const ObjectId& id) const override;
    // Stores bytes under an id they do NOT hash to. Only a corruption test
    // wants this: it is how damaged content is made reachable from a parent
    // that still points at the original address, which is what a corrupt
    // control-store object looks like from here. Nothing in the build or read
    // path uses it.
    void put_at(const ObjectId& id, Bytes node);

    size_t nodes() const {
        return nodes_.size();
    }
    // Reads served since the last `forget_reads`. This is what makes the
    // stat-only claim provable rather than asserted: a getattr that fetches no
    // extent node can be shown to, by counting.
    size_t reads() const {
        return reads_;
    }
    void forget_reads() {
        reads_ = 0;
    }
    uint64_t bytes() const {
        return bytes_;
    }
    // Ids written by the most recent build that were not already present. This
    // is the dirty set a commit would have to replicate, and measuring it is
    // the point of Stage B.
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

// Builds the tree and returns the root node's id. Deterministic in the entry
// set alone; see property 1 above.
ObjectId build_namespace_tree(const std::map<std::string, FsEntry>& entries, NamespaceNodeStore& store,
                              const NamespaceTreeLimits& limits = {});

// One entry at a time, in path order, without a map of the namespace existing
// anywhere. Every full-scan reader wants this rather than `read_namespace_tree`:
// the catalogue scan, the media index rebuild and the reachability walks all
// pass over the namespace once and keep something much smaller than it.
using NamespaceVisitor = std::function<void(const std::string& path, const FsEntry& entry)>;
void walk_namespace_tree(const ObjectId& root, const NamespaceNodeStore& store,
                         const NamespaceVisitor& visit);

// Every entry whose path starts with `prefix`, in path order. On a tree this
// descends: a branch child covers the key range from its own first key to the
// next child's, so a child whose range cannot intersect the prefix is never
// fetched. That is what makes a directory listing cost the subtree rather than
// the library, and it is why the tree is keyed by path rather than by a hash
// of it.
void for_each_namespace_entry_with_prefix(const MetadataSnapshot& snapshot,
                                          const NamespaceNodeStore* store,
                                          std::string_view prefix,
                                          const NamespaceVisitor& visit);

// The namespace of a snapshot, whichever form it is in: the inline map when
// there is one, the tree when the snapshot carries a root. This is what a
// reader converted for Stage C calls, so that it works before and after the
// cutover and so that a detached namespace can never be read as an empty one.
// Throws if the snapshot is detached and no store is supplied, rather than
// visiting nothing and reporting success.
void for_each_namespace_entry(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              const NamespaceVisitor& visit);

// One path from a snapshot, whichever form its namespace is in. The map when
// there is one; a path from the root to a leaf when there is a tree, which
// fetches at most `depth` nodes and, with `with_extents` false, no extent node
// at all. Throws if the snapshot is detached and no store is supplied.
std::optional<FsEntry> namespace_entry(const MetadataSnapshot& snapshot,
                                       const NamespaceNodeStore* store, std::string_view path,
                                       bool with_extents = true);

// Existence alone, which is what FUSE path resolution asks and what it should
// cost: no entry is copied out of a map, and no extent node is fetched from a
// tree. `namespace_entry` for a membership test would copy an FsEntry --
// including a film's extent list -- to answer a question about a key.
bool namespace_contains(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                        std::string_view path);

// Do these two snapshots hold different namespaces? For a tree that is a
// comparison of two 32-byte roots, which is what the whole design is for: two
// nodes that reconcile to the same namespace agree on its root, and a reader
// asking "has the namespace changed" gets an answer without materialising
// anything. For a map it is the map comparison it always was.
//
// This is the witness the FUSE frontend and the catalogue scanner wake up on.
// Under SM14 the entry maps are both empty, so comparing them says "unchanged"
// about every namespace change there will ever be -- the mount stops seeing
// remote writes and the catalogue stops discovering them, silently and
// permanently.
bool namespace_differs(const MetadataSnapshot& a, const MetadataSnapshot& b);

// A change set against a namespace: an entry to upsert, or nothing to delete
// the path.
using NamespaceChanges = std::map<std::string, std::optional<FsEntry>>;

// Applies a change set to an existing tree and returns the new root, without
// rebuilding it. This is what a commit does instead of re-serialising the
// library: the entries a change does not touch are never read, never decoded
// and never rewritten, and the nodes that do change are the leaf holding the
// key and the path from it to the root.
//
// The result is byte-identical to `build_namespace_tree` over the namespace
// the changes produce -- not merely equivalent, the same root -- which is the
// property the whole design rests on and is asserted directly rather than
// argued: two nodes that reach the same namespace by different routes must
// agree on the root, or the signature comparison reports divergence that does
// not exist.
//
// The nodes it writes are the path: measured at 3 of 102 on a 2,520-entry
// library, being the leaf holding the key and the two branches above it. The
// spine above the changed leaf is recomputed over the whole leaf sequence
// rather than spliced, and that costs nothing in written nodes -- an unchanged
// branch re-encodes to the same bytes and so to the same content address, so
// it is not a new node and nothing replicates it.
//
// What the recompute does cost is local: reading the branch nodes to get the
// leaf sequence, and re-encoding them. That is O(branch nodes) -- 10 on es-1's
// namespace against 5,101 entries -- so it is a local CPU cost proportional to
// the tree's shape rather than to the library, and splicing the spine locally
// is an optimisation to take when the namespace grows an order of magnitude,
// not before.
ObjectId update_namespace_tree(const ObjectId& root, NamespaceNodeStore& store,
                               const NamespaceChanges& changes,
                               const NamespaceTreeLimits& limits = {});

// The change set a commit already carries, applied to the tree. This is the
// half of Stage C that matters: a mutation knows exactly which paths it
// touched, so the commit updates those and nothing else rather than
// rediscovering the change by comparing two namespaces -- the mistake the
// catalogue's commit makes, which re-shards and re-encodes everything to find
// the one shard that moved.
//
// Erases, upserts and appends, in the order `apply_metadata_delta_in_place`
// applies them so the two cannot disagree. An append reads the entry it
// extends from the tree and refuses the same base mismatch the map path
// refuses: a delta whose base extent count does not match what is there is a
// delta against a namespace this is not.
ObjectId apply_delta_to_namespace_tree(const ObjectId& root, NamespaceNodeStore& store,
                                       const MetadataDelta& delta,
                                       const NamespaceTreeLimits& limits = {});

// Materialises the whole namespace back out. This is the inverse of the build
// and exists to prove the round trip, not because anything on the hot path
// should want it -- the point of the structure is that nothing has to.
std::map<std::string, FsEntry> read_namespace_tree(const ObjectId& root, const NamespaceNodeStore& store,
                                                   const NamespaceTreeLimits& limits = {});

// One path, without materialising the namespace: O(log n) nodes fetched. This
// is what `getattr` becomes, and what makes "nothing forces materialisation"
// true rather than aspirational.
//
// `with_extents` false is the stat-only read -- the common case, and what FUSE
// path lookup and directory listing actually want. It fetches the path to the
// leaf and nothing else: not the target's extent spine, and not the spine of
// any entry it walks past inside the leaf.
std::optional<FsEntry> namespace_tree_lookup(const ObjectId& root, std::string_view path,
                                             const NamespaceNodeStore& store,
                                             bool with_extents = true);

NamespaceTreeStats namespace_tree_stats(const ObjectId& root, const NamespaceNodeStore& store);

// Every control object the namespace is made of: branches, leaves, and the
// extent spines the leaves address. This is what the reachability live set
// has to carry, and until 2026-09-22 it carried none of it -- the control-store
// live set was built from catalogue roots alone, so every tree node holding
// the namespace was, to garbage collection, an unreferenced object waiting out
// its grace. The 30-day grace set that afternoon as a migration safety net was
// the only thing between the cluster and collecting the nodes that say where
// every file lives. Throws if a node cannot be read: an incomplete live set is
// not a live set, and the caller must refuse to delete against it.
void collect_namespace_tree_nodes(const ObjectId& root, const NamespaceNodeStore& store,
                                  std::vector<ObjectId>& out);

// The nodes reachable from `after` that `before` does not share -- what a
// commit has to acquire retention claims for. A parallel walk: a subtree whose
// id appears on both sides is pruned without being read, so the cost is the
// changed leaves and the path above them, not the namespace. A changed leaf
// contributes itself and every extent spine it addresses; that over-collects
// the spines of unchanged entries that happen to share the leaf, which is the
// safe direction -- an object claimed twice costs a little work, an object
// claimed never costs the file. No `before` means everything is new.
void collect_namespace_tree_changes(const std::optional<ObjectId>& before, const ObjectId& after,
                                    const NamespaceNodeStore& store, std::vector<ObjectId>& out);

// A namespace being mutated, in whichever form it is in.
//
// A batch of filesystem operations has to see its own earlier edits: mkdir /a
// then create /a/b requires the parent check to find /a, which was created a
// moment ago and is in no snapshot yet. The map form gets that for free by
// mutating `entries` and reading it back. A tree cannot be edited in place
// entry by entry -- every edit would rewrite a path to the root -- so the
// batch's own changes have to be overlaid on it.
//
// The overlay is the delta, which the mutation was recording anyway. A path in
// `upsert_entries` reads as that entry; a path in `erase_entries` reads as
// absent, which is the part a map cannot express and a tombstone must; anything
// else reads from the namespace underneath. The delta is therefore not a
// by-product of the mutation any more, it is the mutation, and the snapshot's
// map is left alone.
//
// On a map-backed snapshot this writes through to `entries` as well, because
// SM13's payload is encoded from that map and a commit would otherwise publish
// a namespace without the edit in it. That is the one behavioural difference
// between the two forms and it is confined to this class.
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

    // The first path strictly after `directory` that lies under it, if any.
    // This is the emptiness test: namespace entries are ordered by path, so a
    // directory's children are exactly the entries immediately after it.
    std::optional<std::string> first_path_under(const std::string& directory) const;

    // Every path under `directory`, with its entry, in path order -- what a
    // rename has to move. Extents included: a rename re-keys the entry it
    // found, and dropping its extents would silently empty the file.
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
    MetadataRecord record; // the SM14 head to install
    ObjectId root{};
    NamespaceTreeStats stats;
    size_t entries{};
    uint64_t previous_payload_bytes{};
};

// Builds the tree for `head`'s namespace into `nodes`, verifies it entry by
// entry against that namespace, and returns the record that would replace the
// head. Installs nothing and touches no node state.
//
// The result is a pure function of the head: the tree's shape is determined by
// the entry set alone, so every node holding the same head computes the same
// nodes, the same root and the same record hash without exchanging anything.
// That is what makes a cluster-wide re-root possible without a coordinator,
// and what makes `--expect-hash` a real check rather than a formality.
//
// Verification is not optional and is done here rather than by the caller: a
// re-root that drops an entry drops the only record of where that file's
// extents live, and the extents themselves are untouched and unaware. Throws
// if the tree does not read back as the namespace it was built from.
NamespaceMigration plan_namespace_migration(const MetadataRecord& head, NamespaceNodeStore& nodes);

// The two halves of the SM14 record shape, and the only places a snapshot
// changes which form its namespace is in.
//
// `detach_namespace` builds the tree over `entries`, writes its nodes to
// `store`, and returns the snapshot with `namespace_root` set and `entries`
// empty -- the form `encode_snapshot_v14` will accept. `attach_namespace` is
// the inverse and materialises the whole namespace back into the map.
//
// Both take the snapshot by value so a caller that is done with its copy can
// move it in: the map being transferred is the gigabyte this plan is about,
// and a signature that quietly copied it would cost more than the work it is
// doing.
//
// Nothing on the hot path should want `attach_namespace`. A point lookup is
// `namespace_tree_lookup` and a listing is a prefix scan; materialising is for
// proving the round trip loses nothing, and for the readers Stage C has not
// converted yet.
MetadataSnapshot detach_namespace(MetadataSnapshot snapshot, NamespaceNodeStore& store,
                                  const NamespaceTreeLimits& limits = {});
MetadataSnapshot attach_namespace(MetadataSnapshot snapshot, const NamespaceNodeStore& store,
                                  const NamespaceTreeLimits& limits = {});

} // namespace macha
