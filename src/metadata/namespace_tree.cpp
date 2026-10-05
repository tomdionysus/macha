// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata/namespace_tree.hpp"

#include "codec.hpp"
#include "crypto.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace macha {
namespace {

// Node magics: a node of the wrong kind is a decode failure, not a misread.
constexpr std::array<uint8_t, 4> leaf_magic{'M', 'N', 'L', '1'};
constexpr std::array<uint8_t, 4> branch_magic{'M', 'N', 'B', '1'};
constexpr std::array<uint8_t, 4> extent_leaf_magic{'M', 'N', 'X', '1'};
constexpr std::array<uint8_t, 4> extent_branch_magic{'M', 'N', 'Y', '1'};

enum class ExtentForm : uint8_t { none = 0, inlined = 1, external = 2 };

// Domain- and level-separated so a key is not a boundary at every spine level
// at once, which would make a tower of single-child nodes.
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

// Encoded size of one extent; bounds reservations by what the remaining input
// could actually contain.
constexpr size_t encoded_extent_bytes = 8 + 8 + 1 + 32;

// A spine node's child. `items` counts the entries beneath it, so a subtree
// can be sized without descending.
struct Child {
    std::string first_key; // empty for an extent spine, which is sequential
    ObjectId id{};
    uint64_t items{};
};

bool branch_boundary(const Child& child, uint8_t level, bool keyed, size_t target) {
    const auto hash = keyed ? boundary_hash("macha/namespace-tree/branch/v1", level,
                                            key_span(child.first_key))
                            : boundary_hash("macha/namespace-tree/extent-branch/v1", level,
                                            child.id.bytes);
    return is_boundary(hash, target);
}

Child put_spine_node(const std::vector<Child>& run, uint8_t level, bool keyed,
                     NamespaceNodeStore& store) {
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
    return Child{run.front().first_key, store.put(encoded), items};
}

// One spine level: groups `children` into the nodes above them. Grouping
// hashes each child's key (its content address on the keyless extent spine),
// so the shape is a function of the child sequence alone.
std::vector<Child> build_spine_level(const std::vector<Child>& children, uint8_t level,
                                     NamespaceNodeStore& store, bool keyed, size_t target,
                                     size_t maximum) {
    std::vector<Child> parents;
    std::vector<Child> run;
    const auto flush = [&] {
        if (run.empty())
            return;
        parents.push_back(put_spine_node(run, level, keyed, store));
        run.clear();
    };

    // `packed` ignores the boundary test and groups by the count cap alone:
    // the fallback for a level where every child hashed as a boundary and
    // nothing reduced (e.g. two extent chunks both hitting a 1-in-256
    // boundary). It makes progress unconditional: at maximum >= 2, n >= 2
    // children become at most ceil(n/maximum) < n parents. History
    // independence holds: the trigger and the packing are both functions
    // of this level's child sequence.
    const auto build_level = [&](bool packed) {
        parents.clear();
        run.clear();
        for (const auto& child : children) {
            run.push_back(child);
            if ((!packed && branch_boundary(child, level, keyed, target)) || run.size() >= maximum)
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
    return parents;
}

uint8_t next_spine_level(uint8_t level) {
    return level < std::numeric_limits<uint8_t>::max() ? static_cast<uint8_t>(level + 1) : level;
}

// Groups children into spine nodes until one remains.
ObjectId build_spine(std::vector<Child> children, NamespaceNodeStore& store, bool keyed,
                     size_t target, size_t maximum) {
    if (children.empty())
        throw std::logic_error("namespace tree spine over no children");

    uint8_t level = 1;
    while (children.size() > 1) {
        children = build_spine_level(children, level, store, keyed, target, maximum);
        level = next_spine_level(level);
    }
    return children.front().id;
}

// Writes one entry's extents as a node sequence and returns its root. Chunk
// boundaries hash each extent's content address, so an append cannot move
// existing boundaries and rewrites only the last chunk and the spine.
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

// Steps over a leaf record's stat fields and provenance, as encode_leaf_entry
// writes them between the path and the extent form.
void skip_leaf_entry_stat(Reader& reader) {
    const auto type = reader.u8();
    (void)reader.u32(); // mode
    (void)reader.u32(); // uid
    (void)reader.u32(); // gid
    (void)reader.u64(); // size
    (void)reader.i64(); // ctime
    (void)reader.i64(); // mtime
    (void)reader.u64(); // version
    if (type & entry_type_with_provenance)
        (void)decode_entry_provenance(reader);
}

void encode_leaf_entry(Writer& writer, const std::string& path, const FsEntry& entry,
                       NamespaceNodeStore& store, const NamespaceTreeLimits& limits) {
    writer.string(path);
    const bool provenance = !entry.provenance.empty();
    writer.u8(static_cast<uint8_t>(entry.type) | (provenance ? entry_type_with_provenance : 0));
    writer.u32(entry.mode);
    writer.u32(entry.uid);
    writer.u32(entry.gid);
    writer.u64(entry.size);
    writer.i64(entry.ctime_ns);
    writer.i64(entry.mtime_ns);
    writer.u64(entry.version);
    if (provenance)
        encode_entry_provenance(writer, entry.provenance);
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

// Reads one leaf record. External extents are fetched only for an entry
// whose key `want` accepts; the decision follows the key parse, so a reader
// pays nothing for entries it walks past.
template <typename Want>
std::pair<std::string, FsEntry> decode_leaf_entry_when(Reader& reader,
                                                       const NamespaceNodeStore& store,
                                                       const Want& want) {
    std::pair<std::string, FsEntry> item;
    item.first = reader.string(8192);
    const bool load_extents = want(item.first);
    auto& entry = item.second;
    auto type = reader.u8();
    const bool provenance = (type & entry_type_with_provenance) != 0;
    type &= static_cast<uint8_t>(~entry_type_with_provenance);
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
    if (provenance)
        entry.provenance = decode_entry_provenance(reader);
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
        // The count is unverifiable here (the extents live in other nodes), so
        // it is not used to reserve; `read_extent_sequence` reserves per chunk
        // against bytes that exist.
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

// External extents for every entry when `load_all`, otherwise only for the
// entry whose key equals `load_only_for`. `false` and empty is stat-only.
std::pair<std::string, FsEntry> decode_leaf_entry(Reader& reader, const NamespaceNodeStore& store,
                                                  bool load_all,
                                                  std::string_view load_only_for = {}) {
    return decode_leaf_entry_when(reader, store, [&](const std::string& key) {
        return load_all || (!load_only_for.empty() && key == load_only_for);
    });
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

namespace {
// Whether this key ends its leaf. A function of the key alone, so values never
// move boundaries and a key set always partitions the same way.
bool ends_a_leaf(std::string_view path, const NamespaceTreeLimits& limits) {
    return is_boundary(boundary_hash("macha/namespace-tree/entry/v1", 0, key_span(path)),
                       limits.entry_target_fanout);
}

// Chunks entries into leaves exactly as a full build does. `open` is set when
// the final run did not end on a boundary key: an update window has not yet
// re-synchronised with the global partition and must absorb the next leaf.
using EntryRef = std::pair<const std::string*, const FsEntry*>;

std::vector<Child> chunk_leaves(const std::vector<EntryRef>& entries, NamespaceNodeStore& store,
                                const NamespaceTreeLimits& limits, bool* open = nullptr) {
    std::vector<Child> leaves;
    std::vector<EntryRef> run;
    bool ended_on_boundary = true;

    const auto flush = [&] {
        if (run.empty())
            return;
        Writer writer;
        writer.raw(leaf_magic);
        writer.u32(static_cast<uint32_t>(run.size()));
        for (const auto& [path, entry] : run)
            encode_leaf_entry(writer, *path, *entry, store, limits);
        const auto encoded = writer.take();
        leaves.push_back(
            Child{*run.front().first, store.put(encoded), static_cast<uint64_t>(run.size())});
        run.clear();
    };

    for (const auto& item : entries) {
        run.push_back(item);
        if (ends_a_leaf(*item.first, limits) || run.size() >= limits.entry_max_fanout) {
            ended_on_boundary = true;
            flush();
        } else {
            ended_on_boundary = false;
        }
    }
    flush();
    if (open)
        *open = !ended_on_boundary;
    return leaves;
}

ObjectId empty_leaf(NamespaceNodeStore& store) {
    Writer writer;
    writer.raw(leaf_magic);
    writer.u32(0);
    const auto encoded = writer.take();
    return store.put(encoded);
}
} // namespace

ObjectId build_namespace_tree(const std::map<std::string, FsEntry>& entries, NamespaceNodeStore& store,
                              const NamespaceTreeLimits& limits) {
    std::vector<EntryRef> ordered;
    ordered.reserve(entries.size());
    for (const auto& [path, entry] : entries)
        ordered.emplace_back(&path, &entry);
    auto leaves = chunk_leaves(ordered, store, limits);

    // An empty namespace is one empty leaf, so a root always exists.
    if (leaves.empty())
        return empty_leaf(store);
    return build_spine(std::move(leaves), store, true, limits.branch_target_fanout,
                       limits.branch_max_fanout);
}

namespace {

// Exclusive upper bound of keys starting with `prefix`: the prefix with its
// last non-0xFF byte incremented and trailing 0xFF bytes dropped. Empty means
// unbounded.
std::string prefix_upper_bound(std::string_view prefix) {
    std::string upper(prefix);
    while (!upper.empty()) {
        auto& last = reinterpret_cast<unsigned char&>(upper.back());
        if (last != 0xFF) {
            ++last;
            return upper;
        }
        upper.pop_back();
    }
    return {};
}

void walk_subtree_prefix(const ObjectId& id, const NamespaceNodeStore& store,
                         std::string_view prefix, std::string_view upper,
                         const NamespaceVisitor& visit) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            auto item = decode_leaf_entry_when(
                reader, store, [&](const std::string& key) { return key.starts_with(prefix); });
            if (item.first.starts_with(prefix))
                visit(item.first, item.second);
        }
        reader.finish();
        return;
    }
    if (magic != branch_magic)
        throw DecodeError("not a namespace tree node");
    (void)reader.u8(); // level
    const auto count = reader.u32();
    std::vector<std::pair<std::string, ObjectId>> children;
    children.reserve(std::min<size_t>(count, reader.remaining() / 44));
    for (uint32_t i = 0; i < count; ++i) {
        auto key = reader.string(8192);
        children.emplace_back(std::move(key), ObjectId{reader.fixed<32>()});
        (void)reader.u64();
    }
    reader.finish();
    for (size_t i = 0; i < children.size(); ++i) {
        // A child spans its first key up to the next child's; skip it when
        // that range lies wholly before the prefix or at/after `upper`.
        const bool last = i + 1 == children.size();
        if (!last && children[i + 1].first <= prefix)
            continue;
        if (!upper.empty() && children[i].first >= upper)
            break;
        walk_subtree_prefix(children[i].second, store, prefix, upper, visit);
    }
}

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

// Feeds entries after `after` to `take` until it returns false; returns whether
// it stopped. A child whose successor starts at or before `after` is not read.
bool walk_subtree_after(const ObjectId& id, const NamespaceNodeStore& store,
                        const std::optional<std::string>& after,
                        const std::function<bool(NamespaceItem&&)>& take) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            auto item = decode_leaf_entry(reader, store, true);
            if (after && item.first <= *after)
                continue;
            if (!take(std::move(item)))
                return true;
        }
        reader.finish();
        return false;
    }
    if (magic != branch_magic)
        throw DecodeError("not a namespace tree node");
    (void)reader.u8(); // level
    const auto count = reader.u32();
    std::vector<std::pair<std::string, ObjectId>> children;
    children.reserve(std::min<size_t>(count, reader.remaining() / 44));
    for (uint32_t i = 0; i < count; ++i) {
        auto key = reader.string(8192);
        children.emplace_back(std::move(key), ObjectId{reader.fixed<32>()});
        (void)reader.u64();
    }
    reader.finish();
    for (size_t i = 0; i < children.size(); ++i) {
        if (after && i + 1 < children.size() && children[i + 1].first <= *after)
            continue;
        if (walk_subtree_after(children[i].second, store, after, take))
            return true;
    }
    return false;
}

} // namespace

Page<NamespaceItem, std::string> namespace_entries(const MetadataSnapshot& snapshot,
                                                   const NamespaceNodeStore* store,
                                                   Cursor<std::string> from, Budget& budget) {
    Page<NamespaceItem, std::string> page;
    page.next = from;
    // One operation per entry; stops before an entry it cannot pay for.
    const auto take = [&](NamespaceItem&& item) {
        if (const auto stop = budget.must_stop()) {
            page.stopped = *stop;
            return false;
        }
        if (!budget.take_operation()) {
            page.stopped = Stop::budget;
            return false;
        }
        page.next.after = item.first;
        page.items.push_back(std::move(item));
        return true;
    };
    bool stopped = false;
    if (!snapshot.namespace_root) {
        auto it = from.after ? snapshot.entries.upper_bound(*from.after) : snapshot.entries.begin();
        for (; it != snapshot.entries.end(); ++it)
            if (!take(NamespaceItem(it->first, it->second))) {
                stopped = true;
                break;
            }
    } else {
        if (!store)
            throw DecodeError("namespace is a tree and no node store was supplied");
        stopped = walk_subtree_after(*snapshot.namespace_root, *store, from.after, take);
    }
    if (!stopped) {
        page.next = {};
        page.stopped = Stop::end;
    }
    return page;
}

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

bool namespace_differs(const MetadataSnapshot& a, const MetadataSnapshot& b) {
    if (a.namespace_root || b.namespace_root)
        return a.namespace_root != b.namespace_root;
    return a.entries != b.entries;
}

std::optional<FsEntry> namespace_entry(const MetadataSnapshot& snapshot,
                                       const NamespaceNodeStore* store, std::string_view path,
                                       bool with_extents) {
    if (!snapshot.namespace_root) {
        const auto found = snapshot.entries.find(std::string(path));
        if (found == snapshot.entries.end())
            return {};
        if (with_extents)
            return found->second;
        // Stat-only in both forms: no extent list is handed back.
        FsEntry stat = found->second;
        stat.extents.clear();
        return stat;
    }
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    return namespace_tree_lookup(*snapshot.namespace_root, path, *store, with_extents);
}

bool namespace_contains(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                        std::string_view path) {
    if (!snapshot.namespace_root)
        return snapshot.entries.find(std::string(path)) != snapshot.entries.end();
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    return namespace_tree_lookup(*snapshot.namespace_root, path, *store, false).has_value();
}

namespace {

// A node standing in a spine sequence. One whose level is above the level
// being built stands for everything beneath it, unread.
struct SpineItem {
    Child child;
    uint8_t level{};
    // Written by this update, not carried over from the tree it started from.
    bool fresh{};
};

// A node's own level (0 for a leaf) and, for a branch, its children.
struct SpineNode {
    uint8_t level{};
    std::vector<Child> children;
};

SpineNode read_spine_node(const ObjectId& id, const NamespaceNodeStore& store) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    SpineNode node;
    if (magic == leaf_magic)
        return node;
    if (magic != branch_magic)
        throw DecodeError("not a namespace tree node");
    node.level = reader.u8();
    const auto count = reader.u32();
    if (!node.level || !count)
        throw DecodeError("malformed namespace tree branch");
    node.children.reserve(std::min<size_t>(count, reader.remaining() / 44));
    for (uint32_t i = 0; i < count; ++i) {
        Child child;
        child.first_key = reader.string(8192);
        child.id = ObjectId{reader.fixed<32>()};
        child.items = reader.u64();
        node.children.push_back(std::move(child));
    }
    reader.finish();
    return node;
}

// Replaces the item at `at` with its children, one level down.
void expand_spine_item(std::vector<SpineItem>& items, size_t at, const NamespaceNodeStore& store) {
    const auto level = items[at].level;
    auto node = read_spine_node(items[at].child.id, store);
    if (node.level != level)
        throw DecodeError("namespace tree node at an unexpected level");
    std::vector<SpineItem> children;
    children.reserve(node.children.size());
    for (auto& child : node.children)
        children.push_back({std::move(child), static_cast<uint8_t>(level - 1), false});
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(at));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(at),
                 std::make_move_iterator(children.begin()),
                 std::make_move_iterator(children.end()));
}

// A leaf record as it lies in its leaf, or an entry to encode in its place.
struct LeafRecord {
    std::string key;
    std::span<const uint8_t> raw;
    const FsEntry* entry{};
};

// A leaf's records by key, each with its encoded bytes: nothing an entry
// refers to is read. `backing` keeps the leaf's bytes alive.
void read_leaf_records(const ObjectId& id, const NamespaceNodeStore& store,
                       std::deque<Bytes>& backing, std::vector<LeafRecord>& out) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    backing.push_back(std::move(*encoded));
    const Bytes& bytes = backing.back();
    Reader reader(bytes);
    if (reader.fixed<4>() != leaf_magic)
        throw DecodeError("not a namespace tree leaf");
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
        const auto start = bytes.size() - reader.remaining();
        auto key = reader.string(8192);
        skip_leaf_entry_stat(reader);
        switch (static_cast<ExtentForm>(reader.u8())) {
        case ExtentForm::none:
            break;
        case ExtentForm::inlined: {
            const auto extents = reader.u32();
            (void)reader.view(static_cast<size_t>(extents) * encoded_extent_bytes);
            break;
        }
        case ExtentForm::external:
            (void)reader.u64();
            (void)reader.fixed<32>();
            break;
        default:
            throw DecodeError("bad namespace tree extent form");
        }
        const auto end = bytes.size() - reader.remaining();
        out.push_back({std::move(key), std::span<const uint8_t>(bytes).subspan(start, end - start),
                       nullptr});
    }
    reader.finish();
}

// chunk_leaves over records: one that lies in a leaf already is copied as it
// stands, so an untouched neighbour's extents are neither read nor rebuilt.
std::vector<Child> chunk_records(const std::vector<LeafRecord>& records, NamespaceNodeStore& store,
                                 const NamespaceTreeLimits& limits, bool& open) {
    std::vector<Child> leaves;
    size_t run_begin = 0;
    bool ended_on_boundary = true;
    const auto flush = [&](size_t run_end) {
        if (run_end == run_begin)
            return;
        Writer writer;
        writer.raw(leaf_magic);
        writer.u32(static_cast<uint32_t>(run_end - run_begin));
        for (size_t i = run_begin; i < run_end; ++i) {
            if (records[i].entry)
                encode_leaf_entry(writer, records[i].key, *records[i].entry, store, limits);
            else
                writer.raw(records[i].raw);
        }
        const auto encoded = writer.take();
        leaves.push_back(Child{records[run_begin].key, store.put(encoded),
                               static_cast<uint64_t>(run_end - run_begin)});
        run_begin = run_end;
    };
    for (size_t i = 0; i < records.size(); ++i) {
        if (ends_a_leaf(records[i].key, limits) || i + 1 - run_begin >= limits.entry_max_fanout) {
            ended_on_boundary = true;
            flush(i + 1);
        } else {
            ended_on_boundary = false;
        }
    }
    flush(records.size());
    open = !ended_on_boundary;
    return leaves;
}

// Every item brought down to `level`, reading whatever stands above it.
void expand_spine_to(std::vector<SpineItem>& items, uint8_t level,
                     const NamespaceNodeStore& store) {
    for (size_t i = 0; i < items.size();) {
        if (items[i].level > level)
            expand_spine_item(items, i, store);
        else
            ++i;
    }
}

// The level above `items`, which stand at `level`. A node above the level
// being built is kept unread wherever the grouping reaches it at a group
// boundary: it was built from the same children by the same rule. One the
// grouping reaches mid-group is opened and its children regrouped, until the
// groups fall on an old boundary again.
std::vector<SpineItem> splice_spine_level(std::vector<SpineItem> items, uint8_t level,
                                          NamespaceNodeStore& store,
                                          const NamespaceTreeLimits& limits) {
    const auto above = next_spine_level(level);
    const auto target = limits.branch_target_fanout;
    const auto maximum = limits.branch_max_fanout;
    // The whole level, as a full build makes it.
    const auto exact = [&] {
        expand_spine_to(items, level, store);
        std::vector<Child> children;
        children.reserve(items.size());
        for (auto& item : items)
            children.push_back(std::move(item.child));
        std::vector<SpineItem> parents;
        for (auto& parent : build_spine_level(children, above, store, true, target, maximum))
            parents.push_back({std::move(parent), above, true});
        return parents;
    };
    if (std::all_of(items.begin(), items.end(),
                    [&](const SpineItem& item) { return item.level == level; }))
        return exact();

    std::vector<SpineItem> parents;
    std::vector<Child> run;
    // Whether some group made here holds more than one child.
    bool several = false;
    bool deeper = false;
    const auto flush = [&] {
        if (run.empty())
            return;
        several = several || run.size() > 1;
        parents.push_back({put_spine_node(run, above, true, store), above, true});
        run.clear();
    };
    for (size_t i = 0; i < items.size();) {
        if (items[i].level == level) {
            run.push_back(items[i].child);
            if (branch_boundary(items[i].child, above, true, target) || run.size() >= maximum)
                flush();
            ++i;
        } else if (run.empty()) {
            deeper = deeper || items[i].level > above;
            parents.push_back(items[i]);
            ++i;
        } else {
            expand_spine_item(items, i, store);
        }
    }
    flush();

    // A full build regroups a whole level by count alone when grouping by
    // key reduces nothing: when every group is a single child. A level of a
    // few nodes is settled as a full build settles it. A larger one needs
    // only a group of several, made here or found among those kept.
    constexpr size_t few = 8;
    if (!deeper && parents.size() <= few)
        return exact();
    if (several)
        return parents;
    for (const auto& parent : parents) {
        if (parent.fresh)
            continue;
        ObjectId id = parent.child.id;
        auto node = read_spine_node(id, store);
        while (node.level > above) {
            id = node.children.front().id;
            node = read_spine_node(id, store);
        }
        if (node.level != above)
            throw DecodeError("namespace tree node at an unexpected level");
        // A child ending a group short of its node's end means the node was
        // grouped by count: the level it came from reduced nothing by key.
        for (size_t i = 0; i + 1 < node.children.size(); ++i)
            if (branch_boundary(node.children[i], above, true, target))
                return exact();
        if (node.children.size() > 1)
            return parents;
    }
    return exact();
}

} // namespace

ObjectId update_namespace_tree(const ObjectId& root, NamespaceNodeStore& store,
                               const NamespaceChanges& changes, const NamespaceTreeLimits& limits) {
    if (changes.empty())
        return root;

    // The leaf sequence, with every subtree no change falls in left unread.
    // A change belongs to the last leaf whose first key is at or before its
    // key, else to the first leaf (where a full build would put it).
    std::vector<SpineItem> sequence;
    sequence.push_back({Child{{}, root, 0}, read_spine_node(root, store).level, false});
    std::vector<SpineItem> leaves;
    std::deque<Bytes> backing;
    size_t at = 0;
    auto change = changes.begin();
    while (change != changes.end()) {
        while (at + 1 < sequence.size() && sequence[at + 1].child.first_key <= change->first)
            leaves.push_back(std::move(sequence[at++]));
        if (at < sequence.size() && sequence[at].level > 0) {
            expand_spine_item(sequence, at, store);
            continue;
        }

        // A window of leaves: read, changed, cut into leaves again. While the
        // last leaf cut is open (did not end on a boundary key) the window
        // takes the next leaf, until its cuts fall where a full build's do.
        std::vector<LeafRecord> window;
        backing.clear();
        if (at < sequence.size())
            read_leaf_records(sequence[at++].child.id, store, backing, window);
        const auto first_change = change;
        std::vector<Child> cut;
        for (;;) {
            const std::string* beyond =
                at < sequence.size() ? &sequence[at].child.first_key : nullptr;
            while (change != changes.end() && (!beyond || change->first < *beyond))
                ++change;
            std::vector<LeafRecord> merged;
            merged.reserve(window.size() + 8);
            auto record = window.begin();
            for (auto applied = first_change; record != window.end() || applied != change;) {
                if (applied == change || (record != window.end() && record->key < applied->first)) {
                    merged.push_back(*record++);
                    continue;
                }
                if (record != window.end() && record->key == applied->first)
                    ++record;
                if (applied->second)
                    merged.push_back({applied->first, {}, &*applied->second});
                ++applied;
            }
            bool open = false;
            cut = chunk_records(merged, store, limits, open);
            // An open tail with nothing left to take is the sequence end,
            // where a full build flushes too.
            if (!open || at >= sequence.size())
                break;
            while (sequence[at].level > 0)
                expand_spine_item(sequence, at, store);
            read_leaf_records(sequence[at++].child.id, store, backing, window);
        }
        for (auto& leaf : cut)
            leaves.push_back({std::move(leaf), 0, true});
    }
    for (; at < sequence.size(); ++at)
        leaves.push_back(std::move(sequence[at]));

    uint8_t level = 0;
    for (;;) {
        if (leaves.empty())
            return empty_leaf(store);
        if (leaves.size() == 1) {
            // A full build stops at the first level that is one node. A
            // subtree kept unread may stand over a chain of single children,
            // left when its neighbours went: the root is the foot of it.
            auto id = leaves.front().child.id;
            for (auto above = leaves.front().level; above > level; --above) {
                auto node = read_spine_node(id, store);
                if (node.children.size() != 1)
                    break;
                id = node.children.front().id;
            }
            return id;
        }
        leaves = splice_spine_level(std::move(leaves), level, store, limits);
        level = next_spine_level(level);
    }
}

ObjectId apply_delta_to_namespace_tree(const ObjectId& root, NamespaceNodeStore& store,
                                       const MetadataDelta& delta,
                                       const NamespaceTreeLimits& limits) {
    NamespaceChanges changes;
    // Erase, upsert, append: the order apply_metadata_delta_in_place uses, so
    // an erased-then-upserted path ends up present on both paths.
    for (const auto& path : delta.erase_entries)
        changes[normalize_path(path)] = std::nullopt;
    for (const auto& [path, value] : delta.upsert_entries)
        changes[normalize_path(path)] = value;
    for (const auto& [path, append] : delta.append_entries) {
        const auto normalized = normalize_path(path);
        // Append base: this delta's pending value, else the tree's entry.
        std::optional<FsEntry> base;
        if (const auto pending = changes.find(normalized); pending != changes.end())
            base = pending->second;
        else
            base = namespace_tree_lookup(root, normalized, store, true);
        if (!base)
            throw DecodeError("metadata delta append base mismatch");
        apply_entry_append(*base, normalized, append);
        changes[normalized] = std::move(base);
    }
    const auto updated = update_namespace_tree(root, store, changes, limits);
    // The map form's post-delta invariant: "/" must be a directory. Stat-only,
    // so it costs the tree's depth.
    const auto root_entry = namespace_tree_lookup(updated, "/", store, false);
    if (!root_entry || root_entry->type != EntryType::directory)
        throw DecodeError("metadata delta lost root");
    return updated;
}

std::vector<NamespaceTreeChild> namespace_tree_children(std::span<const uint8_t> node) {
    std::vector<NamespaceTreeChild> children;
    Reader reader(node);
    const auto magic = reader.fixed<4>();
    const auto extent = [&] {
        const auto ref = decode_extent(reader);
        if (!ref.hole)
            children.push_back({true, ref.id});
    };
    if (magic == leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            (void)reader.string(8192);
            skip_leaf_entry_stat(reader);
            switch (static_cast<ExtentForm>(reader.u8())) {
            case ExtentForm::none:
                break;
            case ExtentForm::inlined: {
                const auto inlined = reader.u32();
                for (uint32_t x = 0; x < inlined; ++x)
                    extent();
                break;
            }
            case ExtentForm::external:
                (void)reader.u64();
                children.push_back({false, ObjectId{reader.fixed<32>()}});
                break;
            default:
                throw DecodeError("bad namespace tree extent form");
            }
        }
    } else if (magic == branch_magic) {
        (void)reader.u8();
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            (void)reader.string(8192);
            children.push_back({false, ObjectId{reader.fixed<32>()}});
            (void)reader.u64();
        }
    } else if (magic == extent_leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i)
            extent();
    } else if (magic == extent_branch_magic) {
        (void)reader.u8();
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            children.push_back({false, ObjectId{reader.fixed<32>()}});
            (void)reader.u64();
        }
    } else {
        throw DecodeError("not a namespace tree node");
    }
    reader.finish();
    return children;
}

void for_each_namespace_entry_with_prefix(const MetadataSnapshot& snapshot,
                                          const NamespaceNodeStore* store,
                                          std::string_view prefix,
                                          const NamespaceVisitor& visit) {
    if (!snapshot.namespace_root) {
        for (auto it = snapshot.entries.lower_bound(std::string(prefix));
             it != snapshot.entries.end(); ++it) {
            if (!it->first.starts_with(prefix))
                break;
            visit(it->first, it->second);
        }
        return;
    }
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    walk_subtree_prefix(*snapshot.namespace_root, *store, prefix, prefix_upper_bound(prefix), visit);
}

namespace {
// The stat-only entries at or after `from` in the leaf that would hold it, and
// the first key of the leaf after: one descent.
struct LeafRun {
    std::vector<std::pair<std::string, FsEntry>> items;
    std::optional<std::string> next;
};

LeafRun leaf_run_from(const ObjectId& root, const NamespaceNodeStore& store,
                      std::string_view from) {
    LeafRun run;
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
                auto item = decode_leaf_entry(reader, store, false);
                if (item.first >= from)
                    run.items.push_back(std::move(item));
            }
            reader.finish();
            return run;
        }
        if (magic != branch_magic)
            throw DecodeError("not a namespace tree node");
        (void)reader.u8(); // level
        const auto count = reader.u32();
        if (!count)
            throw DecodeError("namespace tree branch without children");
        // The last child whose first key is <= from, else the first; the
        // child after it bounds this subtree from above.
        ObjectId chosen{};
        bool have = false;
        std::optional<std::string> after;
        for (uint32_t i = 0; i < count; ++i) {
            auto first_key = reader.string(8192);
            ObjectId child{reader.fixed<32>()};
            (void)reader.u64();
            if (!have || first_key <= from) {
                chosen = child;
                have = true;
                continue;
            }
            after = std::move(first_key);
            break;
        }
        if (after)
            run.next = std::move(after);
        current = chosen;
    }
}
} // namespace

void scan_namespace(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                    std::string_view from, const NamespaceScanVisitor& visit) {
    if (!snapshot.namespace_root) {
        auto it = snapshot.entries.lower_bound(std::string(from));
        while (it != snapshot.entries.end()) {
            FsEntry stat = it->second;
            stat.extents.clear();
            auto step = visit(it->first, stat);
            if (step.kind == NamespaceScanStep::Kind::stop)
                return;
            if (step.kind == NamespaceScanStep::Kind::seek) {
                if (step.to <= it->first)
                    throw std::logic_error("namespace scan sought backwards");
                it = snapshot.entries.lower_bound(step.to);
            } else {
                ++it;
            }
        }
        return;
    }
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    std::string cursor(from);
    for (;;) {
        auto run = leaf_run_from(*snapshot.namespace_root, *store, cursor);
        bool sought = false;
        for (size_t i = 0; i < run.items.size(); ++i) {
            const auto& [path, stat] = run.items[i];
            auto step = visit(path, stat);
            if (step.kind == NamespaceScanStep::Kind::stop)
                return;
            if (step.kind != NamespaceScanStep::Kind::seek)
                continue;
            if (step.to <= path)
                throw std::logic_error("namespace scan sought backwards");
            // A target inside this leaf is reached by stepping over what
            // lies before it; only one beyond the leaf costs a descent.
            if (!run.next || step.to < *run.next) {
                while (i + 1 < run.items.size() && run.items[i + 1].first < step.to)
                    ++i;
                continue;
            }
            cursor = std::move(step.to);
            sought = true;
            break;
        }
        if (sought)
            continue;
        if (!run.next)
            return;
        cursor = std::move(*run.next);
    }
}

void for_each_namespace_child(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              std::string_view directory, const NamespaceChildVisitor& visit) {
    const auto prefix = directory == "/" ? std::string("/") : std::string(directory) + "/";
    scan_namespace(snapshot, store, prefix, [&](const std::string& path, const FsEntry& stat) {
        if (!path.starts_with(prefix))
            return NamespaceScanStep::stop();
        const std::string_view rest = std::string_view(path).substr(prefix.size());
        if (rest.empty())
            return NamespaceScanStep::next();
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            visit(std::string(rest), path, stat);
            return NamespaceScanStep::next();
        }
        // Inside a child directory: everything beneath it is passed in one
        // descent.
        auto beyond = prefix_upper_bound(prefix + std::string(rest.substr(0, slash + 1)));
        if (beyond.empty())
            return NamespaceScanStep::stop();
        return NamespaceScanStep::seek(std::move(beyond));
    });
}

std::optional<std::string>
first_namespace_path_under(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                           std::string_view directory,
                           const std::function<bool(const std::string&)>& skip) {
    const auto prefix = directory == "/" ? std::string("/") : std::string(directory) + "/";
    std::optional<std::string> found;
    scan_namespace(snapshot, store, prefix, [&](const std::string& path, const FsEntry&) {
        if (!path.starts_with(prefix))
            return NamespaceScanStep::stop();
        if (path == directory || (skip && skip(path)))
            return NamespaceScanStep::next();
        found = path;
        return NamespaceScanStep::stop();
    });
    return found;
}

void for_each_namespace_entry(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              const NamespaceVisitor& visit) {
    if (!snapshot.namespace_root) {
        for (const auto& [path, entry] : snapshot.entries)
            visit(path, entry);
        return;
    }
    // Without a store, iterating `entries` would silently visit nothing.
    if (!store)
        throw DecodeError("namespace is a tree and no node store was supplied");
    walk_namespace_tree(*snapshot.namespace_root, *store, visit);
}

namespace {

// Names every node read through it. The walk reads each node of the tree, so
// what it names is the tree.
class RecordingNodeStore final : public NamespaceNodeStore {
  public:
    RecordingNodeStore(const NamespaceNodeStore& inner, std::vector<ObjectId>& read)
        : inner_(inner), read_(read) {}
    ObjectId put(std::span<const uint8_t>) override {
        throw std::logic_error("a namespace walk does not write");
    }
    std::optional<Bytes> get(const ObjectId& id) const override {
        auto node = inner_.get(id);
        if (node)
            read_.push_back(id);
        return node;
    }

  private:
    const NamespaceNodeStore& inner_;
    std::vector<ObjectId>& read_;
};

} // namespace

void for_each_namespace_entry(const MetadataSnapshot& snapshot, const NamespaceNodeStore* store,
                              const NamespaceVisitor& visit, std::vector<ObjectId>& nodes) {
    if (!snapshot.namespace_root || !store) {
        for_each_namespace_entry(snapshot, store, visit);
        return;
    }
    const RecordingNodeStore recording(*store, nodes);
    walk_namespace_tree(*snapshot.namespace_root, recording, visit);
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
                // Leaf records are path-ordered: a later key means absent.
                if (item.first > path)
                    return {};
            }
            return {};
        }
        if (magic != branch_magic)
            throw DecodeError("not a namespace tree node");
        (void)reader.u8(); // level
        const auto count = reader.u32();
        // Descend into the last child whose first key is <= path; a path below
        // the first child's key is absent.
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
            skip_leaf_entry_stat(reader);
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

void NamespaceWorkingSet::reload() {
    erased_.clear();
    erased_.insert(delta_.erase_entries.begin(), delta_.erase_entries.end());
}

bool NamespaceWorkingSet::erased(const std::string& path) const {
    return erased_.contains(path);
}

std::optional<FsEntry> NamespaceWorkingSet::get(const std::string& path, bool with_extents) const {
    if (const auto pending = delta_.upsert_entries.find(path);
        pending != delta_.upsert_entries.end()) {
        if (with_extents)
            return pending->second;
        FsEntry stat = pending->second;
        stat.extents.clear();
        return stat;
    }
    if (erased(path))
        return {};
    return namespace_entry(snapshot_, nodes_, path, with_extents);
}

bool NamespaceWorkingSet::contains(const std::string& path) const {
    if (delta_.upsert_entries.contains(path))
        return true;
    if (erased(path))
        return false;
    return namespace_contains(snapshot_, nodes_, path);
}

void NamespaceWorkingSet::put(const std::string& path, const FsEntry& entry) {
    delta_.upsert_entries[path] = entry;
    // Recreating a path erased earlier in the batch drops its tombstone.
    if (erased_.erase(path))
        delta_.erase_entries.erase(
            std::remove(delta_.erase_entries.begin(), delta_.erase_entries.end(), path),
            delta_.erase_entries.end());
    if (!tree_backed_)
        snapshot_.entries[path] = entry;
}

void NamespaceWorkingSet::erase(const std::string& path) {
    delta_.upsert_entries.erase(path);
    if (erased_.insert(path).second)
        delta_.erase_entries.push_back(path);
    if (!tree_backed_)
        snapshot_.entries.erase(path);
}

namespace {
bool path_under(const std::string& path, const std::string& root) {
    return path == root ||
           (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == '/');
}
} // namespace

std::optional<std::string> NamespaceWorkingSet::first_path_under(
    const std::string& directory) const {
    // Overlay first: batch-created children count, batch-erased ones do not.
    const auto prefix = directory == "/" ? std::string("/") : directory + "/";
    for (auto it = delta_.upsert_entries.lower_bound(prefix);
         it != delta_.upsert_entries.end() && it->first.starts_with(prefix); ++it)
        if (it->first != directory)
            return it->first;
    return first_namespace_path_under(snapshot_, nodes_, directory,
                                      [&](const std::string& path) { return erased(path); });
}

std::vector<std::pair<std::string, FsEntry>> NamespaceWorkingSet::subtree(
    const std::string& directory) const {
    std::map<std::string, FsEntry> merged;
    const auto prefix = directory == "/" ? std::string("/") : directory + "/";
    for_each_namespace_entry_with_prefix(snapshot_, nodes_, prefix,
                                         [&](const std::string& path, const FsEntry& entry) {
                                             if (!erased(path))
                                                 merged.emplace(path, entry);
                                         });
    // `directory` itself is not under its own prefix, and a rename moves it too.
    if (auto self = get(directory); self)
        merged.insert_or_assign(directory, *self);
    for (const auto& [path, entry] : delta_.upsert_entries)
        if (path_under(path, directory))
            merged.insert_or_assign(path, entry);
    return {merged.begin(), merged.end()};
}

NamespaceMigration plan_namespace_migration(const MetadataRecord& head,
                                            NamespaceNodeStore& nodes) {
    if (!valid_metadata_record(head))
        throw std::runtime_error("the head to migrate is not a valid metadata record");
    auto snapshot = decode_snapshot(head.payload);
    if (snapshot.namespace_root)
        throw std::runtime_error("this namespace is already a tree");
    if (!snapshot.metadata_write_replicas_required)
        throw std::runtime_error("this namespace predates the protocol-20 write floor and cannot "
                                 "be re-rooted; transition it first");

    NamespaceMigration migration;
    migration.entries = snapshot.entries.size();
    migration.previous_payload_bytes = head.payload.size();

    const auto source = snapshot.entries;
    auto migrated = detach_namespace(std::move(snapshot), nodes);
    // Part of the record, which every migrating node must compute alike.
    migrated.retention_baseline_complete = false;
    migration.root = *migrated.namespace_root;
    migration.stats = namespace_tree_stats(migration.root, nodes);

    // Read the tree back and compare paths, stat fields and extents against
    // the source: everything after this assumes the tree is the namespace.
    const auto read_back = read_namespace_tree(migration.root, nodes);
    if (read_back.size() != source.size())
        throw std::runtime_error("namespace migration verification failed: tree holds " +
                                 std::to_string(read_back.size()) + " entries, namespace had " +
                                 std::to_string(source.size()));
    for (const auto& [path, entry] : source) {
        const auto found = read_back.find(path);
        if (found == read_back.end())
            throw std::runtime_error("namespace migration verification failed: tree is missing " +
                                     path);
        if (!(found->second == entry))
            throw std::runtime_error("namespace migration verification failed: tree changed " +
                                     path);
    }

    migration.record.generation = head.generation + 1;
    migration.record.previous = head.hash;
    migration.record.payload = encode_snapshot_v14(migrated);
    migration.record.hash = metadata_hash(migration.record.generation, migration.record.previous,
                                          migration.record.payload);
    return migration;
}

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
    // "/" must be a directory. `decode_snapshot` cannot check this for SM14
    // (it has no node store), so whoever materialises the tree does.
    const auto root = snapshot.entries.find("/");
    if (root == snapshot.entries.end() || root->second.type != EntryType::directory)
        throw DecodeError("missing root");
    snapshot.namespace_root.reset();
    return snapshot;
}

namespace {

// One node's structure, read without fetching anything beyond it: a branch's
// (first key, id) children, or a leaf's (path, external extent root) entries.
struct NodeShape {
    bool leaf{};
    std::vector<std::pair<std::string, ObjectId>> children;      // branch
    std::vector<std::pair<std::string, std::optional<ObjectId>>> entries; // leaf
};

NodeShape read_shape(const ObjectId& id, const NamespaceNodeStore& store) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    NodeShape shape;
    if (magic == leaf_magic) {
        shape.leaf = true;
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            // Mirrors encode_leaf_entry field by field, skipping stat data.
            auto path = reader.string(8192);
            skip_leaf_entry_stat(reader);
            std::optional<ObjectId> extent_root;
            switch (static_cast<ExtentForm>(reader.u8())) {
            case ExtentForm::none:
                break;
            case ExtentForm::inlined: {
                const auto extents = reader.u32();
                for (uint32_t e = 0; e < extents; ++e)
                    (void)decode_extent(reader);
                break;
            }
            case ExtentForm::external:
                (void)reader.u64();
                extent_root = ObjectId{reader.fixed<32>()};
                break;
            default:
                throw DecodeError("bad namespace tree extent form");
            }
            shape.entries.emplace_back(std::move(path), extent_root);
        }
        reader.finish();
        return shape;
    }
    if (magic != branch_magic)
        throw DecodeError("not a namespace tree node");
    (void)reader.u8(); // level
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
        auto key = reader.string(8192);
        ObjectId child{reader.fixed<32>()};
        (void)reader.u64();
        shape.children.emplace_back(std::move(key), child);
    }
    reader.finish();
    return shape;
}

void collect_extent_spine(const ObjectId& id, const NamespaceNodeStore& store,
                          std::vector<ObjectId>& out) {
    out.push_back(id);
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree extent node unavailable: " + to_string(id));
    Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == extent_leaf_magic)
        return;
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
        collect_extent_spine(child, store, out);
}

void collect_all(const ObjectId& id, const NamespaceNodeStore& store, std::vector<ObjectId>& out) {
    out.push_back(id);
    const auto shape = read_shape(id, store);
    if (shape.leaf) {
        for (const auto& [_, extent_root] : shape.entries)
            if (extent_root)
                collect_extent_spine(*extent_root, store, out);
        return;
    }
    for (const auto& [_, child] : shape.children)
        collect_all(child, store, out);
}

// The parallel walk. If `before` is absent, unreadable or of a different kind,
// nothing is pruned and the subtree under `after` is collected in full.
void collect_changed(const ObjectId& after, const std::optional<ObjectId>& before,
                     const NamespaceNodeStore& store, std::vector<ObjectId>& out) {
    if (before && *before == after)
        return;
    out.push_back(after);
    const auto shape = read_shape(after, store);
    if (shape.leaf) {
        // A changed leaf contributes the extent spines its counterpart did
        // not already name: those are what the change introduced. With no
        // readable counterpart leaf, all of them.
        std::set<ObjectId> named;
        if (before) {
            try {
                const auto old = read_shape(*before, store);
                if (old.leaf)
                    for (const auto& [_, extent_root] : old.entries)
                        if (extent_root)
                            named.insert(*extent_root);
            } catch (const std::exception&) {
            }
        }
        for (const auto& [_, extent_root] : shape.entries)
            if (extent_root && !named.contains(*extent_root))
                collect_extent_spine(*extent_root, store, out);
        return;
    }
    std::optional<NodeShape> old;
    if (before) {
        try {
            auto shape_before = read_shape(*before, store);
            if (!shape_before.leaf)
                old = std::move(shape_before);
        } catch (const std::exception&) {
            // Unreadable old node: prune nothing, over-collect.
        }
    }
    for (size_t i = 0; i < shape.children.size(); ++i) {
        const auto& [key, child] = shape.children[i];
        std::optional<ObjectId> counterpart;
        if (old) {
            // Skip a child whose id appears on the old side; otherwise pair it
            // by key with the old child most likely to share descendants.
            bool shared = false;
            for (const auto& [_, old_child] : old->children)
                if (old_child == child) {
                    shared = true;
                    break;
                }
            if (shared)
                continue;
            for (const auto& [old_key, old_child] : old->children)
                if (old_key <= key)
                    counterpart = old_child;
        }
        collect_changed(child, counterpart, store, out);
    }
}

} // namespace

void collect_namespace_tree_nodes(const ObjectId& root, const NamespaceNodeStore& store,
                                  std::vector<ObjectId>& out) {
    collect_all(root, store, out);
}

void collect_namespace_tree_changes(const std::optional<ObjectId>& before, const ObjectId& after,
                                    const NamespaceNodeStore& store, std::vector<ObjectId>& out) {
    collect_changed(after, before, store, out);
}

namespace {

// One tree's entries in path order, read a node at a time. `pending` holds
// what has not been opened, next at the back; `records` the leaf being read,
// each as encoded. Equal bytes are equal entries, extents included: an extent
// sequence's root is a function of its extents.
struct DiffStream {
    const NamespaceNodeStore& store;
    std::vector<SpineItem> pending;
    std::deque<Bytes> backing;
    std::vector<LeafRecord> records;
    size_t next{};

    DiffStream(const NamespaceNodeStore& nodes, const ObjectId& root) : store(nodes) {
        pending.push_back({Child{{}, root, 0}, read_spine_node(root, nodes).level, false});
    }
    bool buffered() const { return next < records.size(); }
    // Opens the next node: a branch into its children, a leaf into records.
    void open() {
        auto item = std::move(pending.back());
        pending.pop_back();
        if (item.level == 0) {
            records.clear();
            backing.clear();
            next = 0;
            read_leaf_records(item.child.id, store, backing, records);
            return;
        }
        auto node = read_spine_node(item.child.id, store);
        if (node.level != item.level)
            throw DecodeError("namespace tree node at an unexpected level");
        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
            pending.push_back({std::move(*child), static_cast<uint8_t>(item.level - 1), false});
    }
    // Reads on until a record is buffered or nothing is left.
    void fill() {
        while (!buffered() && !pending.empty())
            open();
    }
};

FsEntry decode_leaf_record(const LeafRecord& record, const NamespaceNodeStore& store) {
    Reader reader(record.raw);
    auto item = decode_leaf_entry(reader, store, true);
    reader.finish();
    return std::move(item.second);
}

} // namespace

NamespaceDifferences diff_namespace_trees(const ObjectId& before, const ObjectId& after,
                                          const NamespaceNodeStore& store) {
    NamespaceDifferences out;
    if (before == after)
        return out;
    // Two streams of entries, walked together. Where both stand at the start
    // of the same node, that subtree is the same in both and is passed over
    // unread; otherwise the one standing higher is opened. Entries are
    // compared as their leaves hold them, and only one that differs is
    // decoded, extents and all.
    DiffStream old_side(store, before);
    DiffStream new_side(store, after);
    const auto removed = [&](const LeafRecord& record) {
        out[record.key].before = decode_leaf_record(record, store);
    };
    const auto added = [&](const LeafRecord& record) {
        out[record.key].after = decode_leaf_record(record, store);
    };
    for (;;) {
        if (!old_side.buffered() && !new_side.buffered()) {
            if (old_side.pending.empty() && new_side.pending.empty())
                break;
            if (old_side.pending.empty()) {
                new_side.fill();
                continue;
            }
            if (new_side.pending.empty()) {
                old_side.fill();
                continue;
            }
            const auto& a = old_side.pending.back();
            const auto& b = new_side.pending.back();
            if (a.level == b.level && a.child.id == b.child.id) {
                old_side.pending.pop_back();
                new_side.pending.pop_back();
            } else if (a.level > b.level) {
                old_side.open();
            } else if (b.level > a.level) {
                new_side.open();
            } else {
                old_side.open();
                new_side.open();
            }
            continue;
        }
        if (!old_side.buffered()) {
            old_side.fill();
            if (!old_side.buffered())
                added(new_side.records[new_side.next++]);
            continue;
        }
        if (!new_side.buffered()) {
            new_side.fill();
            if (!new_side.buffered())
                removed(old_side.records[old_side.next++]);
            continue;
        }
        const auto& a = old_side.records[old_side.next];
        const auto& b = new_side.records[new_side.next];
        if (a.key < b.key) {
            removed(a);
            ++old_side.next;
        } else if (b.key < a.key) {
            added(b);
            ++new_side.next;
        } else {
            if (!std::equal(a.raw.begin(), a.raw.end(), b.raw.begin(), b.raw.end())) {
                NamespaceDifference difference;
                difference.before = decode_leaf_record(a, store);
                difference.after = decode_leaf_record(b, store);
                if (difference.before != difference.after)
                    out.emplace(a.key, std::move(difference));
            }
            ++old_side.next;
            ++new_side.next;
        }
    }
    return out;
}

NamespaceTreeMerge merge_tree_backed_heads(const MetadataSnapshot& left,
                                           const MetadataSnapshot& right,
                                           const Hash256& left_head, const Hash256& right_head,
                                           const NamespaceNodeStore& store) {
    if (!left.namespace_root || !right.namespace_root)
        throw std::logic_error("a tree merge requires two tree-backed namespaces");
    // The lower head is the merge commit's primary parent: the changes are
    // against its tree, so they are also the commit's delta.
    const bool left_primary = !(right_head < left_head);
    NamespaceTreeMerge out;
    out.onto = left_primary ? *left.namespace_root : *right.namespace_root;
    if (left_head == right_head) {
        out.merged.snapshot = left;
        out.merged.snapshot.namespace_root.reset();
        return out;
    }
    const auto differing = diff_namespace_trees(*left.namespace_root, *right.namespace_root, store);

    // What the merge must see: each path that differs, each standing
    // conflict's subject, and the directories above them.
    std::set<std::string> paths{"/"};
    for (const auto& [path, _] : differing)
        paths.insert(path);
    for (const auto* conflicts : {&left.conflicts, &right.conflicts})
        for (const auto& [_, conflict] : *conflicts)
            if (conflict.kind == MetadataConflictKind::namespace_entry)
                paths.insert(conflict.key);
    for (const auto& path : std::vector<std::string>(paths.begin(), paths.end()))
        for (auto above = path; above != "/" && !above.empty();) {
            above = parent_path(above);
            paths.insert(above);
        }

    NamespaceEntries left_entries, right_entries;
    for (const auto& path : paths) {
        const auto found = differing.find(path);
        if (found != differing.end()) {
            if (found->second.before)
                left_entries.emplace(path, *found->second.before);
            if (found->second.after)
                right_entries.emplace(path, *found->second.after);
            continue;
        }
        // Not a difference: both heads hold the same entry, or neither.
        if (auto same = namespace_tree_lookup(*left.namespace_root, path, store, true)) {
            left_entries.emplace(path, *same);
            right_entries.emplace(path, std::move(*same));
        }
    }

    out.merged = merge_metadata_heads_over(left, right, left_entries, right_entries, left_head,
                                           right_head);
    const auto& primary_entries = left_primary ? left_entries : right_entries;
    for (const auto& path : paths) {
        const auto merged = out.merged.snapshot.entries.find(path);
        const auto was = primary_entries.find(path);
        const bool present = merged != out.merged.snapshot.entries.end();
        if (present != (was != primary_entries.end()) ||
            (present && merged->second != was->second))
            out.changes[path] =
                present ? std::optional<FsEntry>(merged->second) : std::optional<FsEntry>();
    }
    out.merged.snapshot.entries.clear();
    return out;
}

std::optional<MetadataDelta> tree_merge_delta(const MetadataSnapshot& primary,
                                              const MetadataSnapshot& merged,
                                              const NamespaceChanges& changes) {
    auto delta = metadata_delta(primary, merged);
    if (!delta)
        return {};
    for (const auto& [path, entry] : changes) {
        if (entry)
            delta->upsert_entries.insert_or_assign(path, *entry);
        else
            delta->erase_entries.push_back(path);
    }
    return delta;
}

NamespaceTreeStats namespace_tree_stats(const ObjectId& root, const NamespaceNodeStore& store) {
    NamespaceTreeStats stats;
    walk_stats(root, store, stats, 1);
    return stats;
}

} // namespace macha
