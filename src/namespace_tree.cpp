// SPDX-License-Identifier: GPL-3.0-or-later
#include "namespace_tree.hpp"

#include "codec.hpp"
#include "crypto.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace macha {
namespace {

// Node magics. Every node says what it is, so a fetched object that is not the
// kind the caller expected is a decode failure rather than a misread.
constexpr std::array<uint8_t, 4> leaf_magic{'M', 'N', 'L', '1'};
constexpr std::array<uint8_t, 4> branch_magic{'M', 'N', 'B', '1'};
constexpr std::array<uint8_t, 4> extent_leaf_magic{'M', 'N', 'X', '1'};
constexpr std::array<uint8_t, 4> extent_branch_magic{'M', 'N', 'Y', '1'};

// How the extents of one entry are carried in its leaf record.
enum class ExtentForm : uint8_t { none = 0, inlined = 1, external = 2 };

// The boundary decision. Domain-separated and level-separated so the same key
// does not land on a boundary at every level of the spine at once, which would
// produce a tower of single-child nodes.
uint64_t boundary_hash(std::string_view domain, uint8_t level, std::span<const uint8_t> key) {
    Sha256Hasher hasher;
    hasher.update({reinterpret_cast<const uint8_t*>(domain.data()), domain.size()});
    hasher.update({&level, 1});
    hasher.update(key);
    const auto digest = hasher.finish();
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | digest.bytes[i];
    return value;
}

bool is_boundary(uint64_t hash, size_t target) {
    if (target <= 1)
        return true;
    return hash < std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(target);
}

std::span<const uint8_t> key_span(std::string_view key) {
    return {reinterpret_cast<const uint8_t*>(key.data()), key.size()};
}

void encode_extent(Writer& writer, const ExtentRef& extent) {
    writer.u64(extent.offset);
    writer.u64(extent.length);
    writer.u8(extent.hole ? 1 : 0);
    writer.fixed(extent.id.bytes);
}

ExtentRef decode_extent(Reader& reader) {
    ExtentRef extent;
    extent.offset = reader.u64();
    extent.length = reader.u64();
    extent.hole = reader.u8() != 0;
    extent.id.bytes = reader.fixed<32>();
    return extent;
}

// The encoded size of one extent, used to bound reservations against what the
// remaining input could actually contain -- the same discipline 0.43.1 applied
// to `entry(Reader&)` after 10.4 MB of allocator slack turned up on es-1.
constexpr size_t encoded_extent_bytes = 8 + 8 + 1 + 32;

// A child of a spine node: what it is addressed by, and how many leaves sit
// underneath it. The count lets a reader size its result and lets a caller
// range-count a subtree without descending into it.
struct Child {
    std::string first_key; // empty for an extent spine, which is sequential
    ObjectId id{};
    uint64_t items{};
};

// Groups a run of children into spine nodes until one node remains. The
// grouping is decided by hashing each child's key (or, for the keyless extent
// spine, its content address), so the shape of the spine is a function of the
// child sequence and nothing else.
ObjectId build_spine(std::vector<Child> children, NamespaceNodeStore& store, bool keyed,
                     size_t target, size_t maximum) {
    if (children.empty())
        throw std::logic_error("namespace tree spine over no children");

    uint8_t level = 1;
    while (children.size() > 1) {
        std::vector<Child> parents;
        std::vector<Child> run;
        const auto flush = [&] {
            if (run.empty())
                return;
            Writer writer;
            writer.raw(keyed ? branch_magic : extent_branch_magic);
            writer.u8(level);
            writer.u32(static_cast<uint32_t>(run.size()));
            uint64_t items = 0;
            for (const auto& child : run) {
                if (keyed)
                    writer.string(child.first_key);
                writer.fixed(child.id.bytes);
                writer.u64(child.items);
                items += child.items;
            }
            const auto encoded = writer.take();
            parents.push_back(Child{run.front().first_key, store.put(encoded), items});
            run.clear();
        };

        // `packed` ignores the boundary test and groups purely by the count
        // cap. It is the fallback for a level where every child happened to
        // hash as a boundary, which produces one parent per child and no
        // reduction at all.
        //
        // That is not a hypothetical. Measured against es-1's real namespace
        // on 2026-09-21, the first build threw here with
        // `level=1 keyed=0 children=2 parents=2 target=256`: an extent
        // sequence of exactly two chunks whose two content addresses both hit
        // a 1-in-256 boundary. That is a 1-in-65,536 event per multi-chunk
        // file, and across 4,808 entries meeting it once is unremarkable. Six
        // tests on generated namespaces never saw it.
        //
        // It would not in fact have looped forever -- each level hashes
        // different bytes, so the next one reduces with probability
        // 1 - (1/target)^n -- but "terminates almost surely" is not a
        // guarantee, and the old code chose to abort rather than rely on it.
        // Packing by the cap makes progress unconditional: at maximum >= 2,
        // n children become at most ceil(n/maximum) < n parents for n >= 2.
        //
        // **History independence survives**, which is the property this must
        // not cost. The fallback fires on a condition computed from this
        // level's children, and those are a pure function of the sorted entry
        // set; the packing itself is positional over that same sequence.
        // Nothing here depends on insertion order or on how the namespace was
        // reached.
        const auto build_level = [&](bool packed) {
            parents.clear();
            run.clear();
            for (auto& child : children) {
                const auto hash = keyed ? boundary_hash("macha/namespace-tree/branch/v1", level,
                                                        key_span(child.first_key))
                                        : boundary_hash("macha/namespace-tree/extent-branch/v1",
                                                        level, child.id.bytes);
                run.push_back(child);
                if ((!packed && is_boundary(hash, target)) || run.size() >= maximum)
                    flush();
            }
            flush();
        };

        build_level(false);
        if (parents.size() >= children.size() && children.size() > 1)
            build_level(true);
        if (parents.size() >= children.size() && children.size() > 1)
            throw std::logic_error("namespace tree spine made no progress even packed: level=" +
                                   std::to_string(level) + " keyed=" + (keyed ? "1" : "0") +
                                   " children=" + std::to_string(children.size()) +
                                   " parents=" + std::to_string(parents.size()) +
                                   " max=" + std::to_string(maximum));
        children = std::move(parents);
        if (level < std::numeric_limits<uint8_t>::max())
            ++level;
    }
    return children.front().id;
}

// Writes one entry's extents as their own sequence of nodes and returns its
// root. Chunk boundaries are decided by the extent's own content address, which
// is what makes an append stable: adding an extent cannot move the boundaries
// of the extents already written, so an append rewrites one chunk and the
// spine rather than the whole list.
ObjectId build_extent_sequence(const std::vector<ExtentRef>& extents, NamespaceNodeStore& store,
                               const NamespaceTreeLimits& limits) {
    std::vector<Child> chunks;
    std::vector<ExtentRef> run;
    const auto flush = [&] {
        if (run.empty())
            return;
        Writer writer;
        writer.raw(extent_leaf_magic);
        writer.u32(static_cast<uint32_t>(run.size()));
        for (const auto& extent : run)
            encode_extent(writer, extent);
        const auto encoded = writer.take();
        chunks.push_back(Child{{}, store.put(encoded), static_cast<uint64_t>(run.size())});
        run.clear();
    };

    for (const auto& extent : extents) {
        run.push_back(extent);
        if (is_boundary(boundary_hash("macha/namespace-tree/extent/v1", 0, extent.id.bytes),
                        limits.extent_target_fanout) ||
            run.size() >= limits.extent_max_fanout)
            flush();
    }
    flush();
    return build_spine(std::move(chunks), store, false, limits.extent_target_fanout,
                       limits.extent_max_fanout);
}

void read_extent_sequence(const ObjectId& root, const NamespaceNodeStore& store,
                          std::vector<ExtentRef>& out) {
    auto encoded = store.get(root);
    if (!encoded)
        throw DecodeError("namespace tree extent node unavailable: " + to_string(root));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == extent_leaf_magic) {
        const auto count = reader.u32();
        out.reserve(out.size() + std::min<size_t>(count, reader.remaining() / encoded_extent_bytes));
        for (uint32_t i = 0; i < count; ++i)
            out.push_back(decode_extent(reader));
        reader.finish();
        return;
    }
    if (magic != extent_branch_magic)
        throw DecodeError("not a namespace tree extent node");
    (void)reader.u8(); // level
    const auto count = reader.u32();
    std::vector<ObjectId> children;
    children.reserve(std::min<size_t>(count, reader.remaining() / 40));
    for (uint32_t i = 0; i < count; ++i) {
        ObjectId id{reader.fixed<32>()};
        (void)reader.u64();
        children.push_back(id);
    }
    reader.finish();
    for (const auto& child : children)
        read_extent_sequence(child, store, out);
}

void encode_leaf_entry(Writer& writer, const std::string& path, const FsEntry& entry,
                       NamespaceNodeStore& store, const NamespaceTreeLimits& limits) {
    writer.string(path);
    writer.u8(static_cast<uint8_t>(entry.type));
    writer.u32(entry.mode);
    writer.u32(entry.uid);
    writer.u32(entry.gid);
    writer.u64(entry.size);
    writer.i64(entry.ctime_ns);
    writer.i64(entry.mtime_ns);
    writer.u64(entry.version);
    if (entry.extents.empty()) {
        writer.u8(static_cast<uint8_t>(ExtentForm::none));
        return;
    }
    if (entry.extents.size() <= limits.extent_inline_max) {
        writer.u8(static_cast<uint8_t>(ExtentForm::inlined));
        writer.u32(static_cast<uint32_t>(entry.extents.size()));
        for (const auto& extent : entry.extents)
            encode_extent(writer, extent);
        return;
    }
    writer.u8(static_cast<uint8_t>(ExtentForm::external));
    writer.u64(static_cast<uint64_t>(entry.extents.size()));
    writer.fixed(build_extent_sequence(entry.extents, store, limits).bytes);
}

// Reads one leaf record. Extents are fetched only when the caller says so, and
// the decision is made AFTER the key is parsed so that a scan pays nothing for
// the entries it walks past.
//
// `load_all` is for a full materialisation. `load_only_for` is for a lookup:
// at most one key in the leaf is the one asked for, and the rest are compared
// and discarded. Both false and empty is a stat-only read, which is every FUSE
// path lookup and directory listing -- the whole reason the extents are
// addressed rather than inlined.
//
// Until 2026-09-21 the lookup passed `true` unconditionally, so a stat fetched
// the extent spine of every entry it skipped past on the way to the one it
// wanted. On a leaf of 32 entries holding films, that is dozens of node reads
// to answer a getattr that needs none of them.
std::pair<std::string, FsEntry> decode_leaf_entry(Reader& reader, const NamespaceNodeStore& store,
                                                  bool load_all,
                                                  std::string_view load_only_for = {}) {
    std::pair<std::string, FsEntry> item;
    item.first = reader.string(8192);
    const bool load_extents = load_all || (!load_only_for.empty() && item.first == load_only_for);
    auto& entry = item.second;
    const auto type = reader.u8();
    if (type < 1 || type > 2)
        throw DecodeError("bad namespace tree entry type");
    entry.type = static_cast<EntryType>(type);
    entry.mode = reader.u32();
    entry.uid = reader.u32();
    entry.gid = reader.u32();
    entry.size = reader.u64();
    entry.ctime_ns = reader.i64();
    entry.mtime_ns = reader.i64();
    entry.version = reader.u64();
    switch (static_cast<ExtentForm>(reader.u8())) {
    case ExtentForm::none:
        break;
    case ExtentForm::inlined: {
        const auto count = reader.u32();
        entry.extents.reserve(std::min<size_t>(count, reader.remaining() / encoded_extent_bytes));
        for (uint32_t i = 0; i < count; ++i)
            entry.extents.push_back(decode_extent(reader));
        break;
    }
    case ExtentForm::external: {
        // The count is a hint from the node and nothing here can check it:
        // unlike every other reserve in this file, the extents live in OTHER
        // nodes, so `reader.remaining()` is not a bound on them.
        //
        // It used to be reserved directly, capped at 10,000,000 — which is
        // 560 MB at the 56 bytes an ExtentRef occupies on aarch64, sized from
        // an unvalidated integer in a node that may be corrupt or forged. That
        // is exactly the pathology Stage A removed from `entry(Reader&)`, and
        // a fixed cap is a guess with an expiry date besides: the whole live
        // cluster holds 445,959 extents, so 10,000,000 was never a bound on
        // anything real.
        //
        // It is simply dropped. `read_extent_sequence` reserves per chunk
        // against that chunk's own remaining bytes, so growth is already
        // bounded by data that exists, and the only thing the outer reserve
        // bought was a few reallocations.
        (void)reader.u64();
        ObjectId root{reader.fixed<32>()};
        if (load_extents)
            read_extent_sequence(root, store, entry.extents);
        break;
    }
    default:
        throw DecodeError("bad namespace tree extent form");
    }
    return item;
}

} // namespace

ObjectId MemoryNamespaceNodeStore::put(std::span<const uint8_t> node) {
    const auto id = object_id(node);
    auto [it, inserted] = nodes_.try_emplace(id, Bytes(node.begin(), node.end()));
    if (inserted) {
        bytes_ += node.size();
        written_.push_back(id);
    }
    return id;
}

std::optional<Bytes> MemoryNamespaceNodeStore::get(const ObjectId& id) const {
    ++reads_;
    auto found = nodes_.find(id);
    if (found == nodes_.end())
        return {};
    return found->second;
}

void MemoryNamespaceNodeStore::put_at(const ObjectId& id, Bytes node) {
    bytes_ += node.size();
    nodes_[id] = std::move(node);
}

ObjectId build_namespace_tree(const std::map<std::string, FsEntry>& entries, NamespaceNodeStore& store,
                              const NamespaceTreeLimits& limits) {
    std::vector<Child> leaves;
    std::vector<std::pair<const std::string*, const FsEntry*>> run;

    const auto flush = [&] {
        if (run.empty())
            return;
        Writer writer;
        writer.raw(leaf_magic);
        writer.u32(static_cast<uint32_t>(run.size()));
        for (const auto& [path, entry] : run)
            encode_leaf_entry(writer, *path, *entry, store, limits);
        const auto encoded = writer.take();
        leaves.push_back(Child{*run.front().first, store.put(encoded),
                               static_cast<uint64_t>(run.size())});
        run.clear();
    };

    for (const auto& [path, entry] : entries) {
        run.emplace_back(&path, &entry);
        if (is_boundary(boundary_hash("macha/namespace-tree/entry/v1", 0, key_span(path)),
                        limits.entry_target_fanout) ||
            run.size() >= limits.entry_max_fanout)
            flush();
    }
    flush();

    // An empty namespace is still a tree: one empty leaf, so a root always
    // exists and `read_namespace_tree` of a fresh cluster is not a special case.
    if (leaves.empty()) {
        Writer writer;
        writer.raw(leaf_magic);
        writer.u32(0);
        const auto encoded = writer.take();
        return store.put(encoded);
    }
    return build_spine(std::move(leaves), store, true, limits.branch_target_fanout,
                       limits.branch_max_fanout);
}

namespace {

void walk_subtree(const ObjectId& id, const NamespaceNodeStore& store,
                  const NamespaceVisitor& visit) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            auto item = decode_leaf_entry(reader, store, true);
            visit(item.first, item.second);
        }
        reader.finish();
        return;
    }
    if (magic != branch_magic)
        throw DecodeError("not a namespace tree node");
    (void)reader.u8(); // level
    const auto count = reader.u32();
    std::vector<ObjectId> children;
    children.reserve(std::min<size_t>(count, reader.remaining() / 44));
    for (uint32_t i = 0; i < count; ++i) {
        (void)reader.string(8192);
        children.push_back(ObjectId{reader.fixed<32>()});
        (void)reader.u64();
    }
    reader.finish();
    for (const auto& child : children)
        walk_subtree(child, store, visit);
}

} // namespace

void walk_namespace_tree(const ObjectId& root, const NamespaceNodeStore& store,
                         const NamespaceVisitor& visit) {
    walk_subtree(root, store, visit);
}

std::map<std::string, FsEntry> read_namespace_tree(const ObjectId& root, const NamespaceNodeStore& store,
                                                   const NamespaceTreeLimits&) {
    std::map<std::string, FsEntry> out;
    walk_subtree(root, store, [&](const std::string& path, const FsEntry& entry) {
        if (!out.emplace(path, entry).second)
            throw DecodeError("namespace tree contains a duplicate path");
    });
    return out;
}

std::optional<FsEntry> namespace_entry(const MetadataSnapshot& snapshot,
                                       const NamespaceNodeStore* store, std::string_view path,
                                       bool with_extents) {
    if (!snapshot.namespace_root) {
        const auto found = snapshot.entries.find(std::string(path));
        if (found == snapshot.entries.end())
            return {};
        return found->second;
    }
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    return namespace_tree_lookup(*snapshot.namespace_root, path, *store, with_extents);
}

void for_each_namespace_entry(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              const NamespaceVisitor& visit) {
    if (!snapshot.namespace_root) {
        for (const auto& [path, entry] : snapshot.entries)
            visit(path, entry);
        return;
    }
    // A detached namespace with no store is the silent-empty case this exists
    // to prevent: iterating `entries` here would visit nothing and report
    // success. Refuse instead, and name what is missing.
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    walk_namespace_tree(*snapshot.namespace_root, *store, visit);
}

std::optional<FsEntry> namespace_tree_lookup(const ObjectId& root, std::string_view path,
                                             const NamespaceNodeStore& store, bool with_extents) {
    ObjectId current = root;
    for (;;) {
        auto encoded = store.get(current);
        if (!encoded)
            throw DecodeError("namespace tree node unavailable: " + to_string(current));
        Reader reader(*encoded);
        const auto magic = reader.fixed<4>();
        if (magic == leaf_magic) {
            const auto count = reader.u32();
            for (uint32_t i = 0; i < count; ++i) {
                auto item = decode_leaf_entry(reader, store, false,
                                              with_extents ? path : std::string_view{});
                if (item.first == path)
                    return item.second;
                // Leaf records are in path order, so a key past the one asked
                // for settles the question without reading the rest.
                if (item.first > path)
                    return {};
            }
            return {};
        }
        if (magic != branch_magic)
            throw DecodeError("not a namespace tree node");
        (void)reader.u8(); // level
        const auto count = reader.u32();
        // Descend into the last child whose first key is at or before the path.
        // A path below the first child's key is not in the tree at all.
        std::optional<ObjectId> next;
        for (uint32_t i = 0; i < count; ++i) {
            auto first_key = reader.string(8192);
            ObjectId child{reader.fixed<32>()};
            (void)reader.u64();
            if (first_key <= path)
                next = child;
            else if (next)
                break;
        }
        if (!next)
            return {};
        current = *next;
    }
}

namespace {

void walk_stats(const ObjectId& id, const NamespaceNodeStore& store, NamespaceTreeStats& stats,
                size_t depth);

void walk_extent_stats(const ObjectId& id, const NamespaceNodeStore& store, NamespaceTreeStats& stats) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree extent node unavailable: " + to_string(id));
    ++stats.extent_nodes;
    stats.bytes += encoded->size();
    stats.largest_node_bytes = std::max<uint64_t>(stats.largest_node_bytes, encoded->size());
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == extent_leaf_magic)
        return; // counted through the entry's own extent total
    if (magic != extent_branch_magic)
        throw DecodeError("not a namespace tree extent node");
    (void)reader.u8();
    const auto count = reader.u32();
    std::vector<ObjectId> children;
    for (uint32_t i = 0; i < count; ++i) {
        children.push_back(ObjectId{reader.fixed<32>()});
        (void)reader.u64();
    }
    for (const auto& child : children)
        walk_extent_stats(child, store, stats);
}

void walk_stats(const ObjectId& id, const NamespaceNodeStore& store, NamespaceTreeStats& stats,
                size_t depth) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    stats.bytes += encoded->size();
    stats.largest_node_bytes = std::max<uint64_t>(stats.largest_node_bytes, encoded->size());
    stats.depth = std::max(stats.depth, depth);
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == leaf_magic) {
        ++stats.leaves;
        const auto count = reader.u32();
        stats.entries += count;
        std::vector<ObjectId> extent_roots;
        for (uint32_t i = 0; i < count; ++i) {
            (void)reader.string(8192);
            (void)reader.u8();
            (void)reader.u32();
            (void)reader.u32();
            (void)reader.u32();
            (void)reader.u64();
            (void)reader.i64();
            (void)reader.i64();
            (void)reader.u64();
            switch (static_cast<ExtentForm>(reader.u8())) {
            case ExtentForm::none:
                break;
            case ExtentForm::inlined: {
                const auto inlined = reader.u32();
                stats.extents += inlined;
                for (uint32_t x = 0; x < inlined; ++x)
                    (void)decode_extent(reader);
                break;
            }
            case ExtentForm::external:
                stats.extents += reader.u64();
                extent_roots.push_back(ObjectId{reader.fixed<32>()});
                break;
            default:
                throw DecodeError("bad namespace tree extent form");
            }
        }
        for (const auto& root : extent_roots)
            walk_extent_stats(root, store, stats);
        return;
    }
    if (magic != branch_magic)
        throw DecodeError("not a namespace tree node");
    ++stats.branches;
    (void)reader.u8();
    const auto count = reader.u32();
    std::vector<ObjectId> children;
    for (uint32_t i = 0; i < count; ++i) {
        (void)reader.string(8192);
        children.push_back(ObjectId{reader.fixed<32>()});
        (void)reader.u64();
    }
    for (const auto& child : children)
        walk_stats(child, store, stats, depth + 1);
}

} // namespace

MetadataSnapshot detach_namespace(MetadataSnapshot snapshot, NamespaceNodeStore& store,
                                  const NamespaceTreeLimits& limits) {
    if (snapshot.namespace_root)
        throw std::runtime_error("namespace is already detached");
    snapshot.namespace_root = build_namespace_tree(snapshot.entries, store, limits);
    snapshot.entries.clear();
    return snapshot;
}

MetadataSnapshot attach_namespace(MetadataSnapshot snapshot, const NamespaceNodeStore& store,
                                  const NamespaceTreeLimits& limits) {
    if (!snapshot.namespace_root)
        throw std::runtime_error("snapshot carries no namespace root");
    if (!snapshot.entries.empty())
        throw std::runtime_error("snapshot already carries its entries");
    snapshot.entries = read_namespace_tree(*snapshot.namespace_root, store, limits);
    // The check SM13's decoder makes on every snapshot it reads, made here
    // instead: a namespace without a root directory is not a filesystem. SM14
    // cannot make it -- `decode_snapshot` has no node store and materialising
    // one to run a sanity check is exactly the cost the tree removes -- so it
    // belongs to whoever does hold the store and does read the tree.
    const auto root = snapshot.entries.find("/");
    if (root == snapshot.entries.end() || root->second.type != EntryType::directory)
        throw DecodeError("missing root");
    snapshot.namespace_root.reset();
    return snapshot;
}

NamespaceTreeStats namespace_tree_stats(const ObjectId& root, const NamespaceNodeStore& store) {
    NamespaceTreeStats stats;
    walk_stats(root, store, stats, 1);
    return stats;
}

} // namespace macha
