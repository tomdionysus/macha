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
// This is the substrate for SM14 (see TODO/2026-09-17-namespace-merkle-root-plan.md).
// It is not yet authoritative for anything: nothing here is reachable from a
// MetadataRecord, and `encode_snapshot` is untouched. What it establishes is the
// structure the record will eventually point at instead of inlining.
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
