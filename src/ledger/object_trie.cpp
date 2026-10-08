// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/object_trie.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>

namespace macha {
namespace {

constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'L', 'J', '0', '1'};
constexpr std::array<uint8_t, 8> nodes_aad{'M', 'A', 'C', 'H', 'L', 'N', '0', '1'};
constexpr std::array<uint8_t, 8> root_magic{'M', 'L', 'T', 'R', '0', '0', '0', '1'};
constexpr uint32_t max_frame = 4U * 1024U * 1024U;
constexpr size_t changes_per_frame = 65536;
constexpr uint8_t kind_leaf = 1;
constexpr uint8_t kind_interior = 2;
constexpr uint8_t journal_version = 1;

Hash256 leaf_hash(const std::vector<ObjectTrie::Record>& records) {
    if (records.empty())
        return {};
    Writer writer;
    writer.u8('L');
    for (const auto& [id, value] : records) {
        writer.fixed(id.bytes);
        writer.u64(value);
    }
    return sha256(writer.data());
}

} // namespace

std::filesystem::path ObjectTrie::nodes_path(uint64_t generation) const {
    return dir_ / ("nodes-" + std::to_string(generation) + ".bin");
}

size_t ObjectTrie::footprint(const Node& node) noexcept {
    return sizeof(Node) + node.records.capacity() * sizeof(Record) +
           node.children.capacity() * sizeof(Child);
}

ObjectTrie::NodePtr ObjectTrie::make_leaf(uint8_t depth, std::vector<Record> records) {
    auto node = std::make_shared<Node>();
    node->depth = depth;
    node->leaf = true;
    node->count = records.size();
    node->hash = leaf_hash(records);
    node->records = std::move(records);
    return node;
}

ObjectTrie::NodePtr ObjectTrie::make_interior(uint8_t depth, std::vector<Child> children) {
    auto node = std::make_shared<Node>();
    node->depth = depth;
    node->leaf = false;
    Writer writer;
    writer.u8('I');
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].empty())
            continue;
        node->count += children[i].count;
        writer.u8(static_cast<uint8_t>(i));
        writer.fixed(children[i].hash.bytes);
        writer.u64(children[i].count);
    }
    node->hash = sha256(writer.data());
    node->children = std::move(children);
    return node;
}

ObjectTrie::Child ObjectTrie::child_of(NodePtr node) {
    Child child;
    child.count = node->count;
    child.hash = node->hash;
    child.node = std::move(node);
    return child;
}

Bytes ObjectTrie::encode(const Node& node) const {
    Writer writer;
    if (node.leaf) {
        writer.u8(kind_leaf);
        writer.u8(node.depth);
        writer.u32(static_cast<uint32_t>(node.records.size()));
        for (const auto& [id, value] : node.records) {
            writer.fixed(id.bytes);
            writer.u64(value);
        }
        return writer.take();
    }
    writer.u8(kind_interior);
    writer.u8(node.depth);
    std::array<uint8_t, 32> present{};
    for (size_t i = 0; i < 256; ++i)
        if (!node.children[i].empty())
            present[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
    writer.fixed(present);
    for (const auto& child : node.children) {
        if (child.empty())
            continue;
        if (child.offset == unsaved)
            throw std::logic_error("object trie: encoding a node with an unsaved child");
        writer.u64(child.offset);
        writer.u32(child.size);
        writer.u64(child.count);
        writer.fixed(child.hash.bytes);
    }
    return writer.take();
}

ObjectTrie::NodePtr ObjectTrie::decode(std::span<const uint8_t> bytes) const {
    Reader reader(bytes);
    const auto kind = reader.u8();
    const auto depth = reader.u8();
    if (depth >= 32)
        throw DecodeError("object trie node too deep");
    if (kind == kind_leaf) {
        const auto count = reader.u32();
        if (count == 0 || count > leaf_max)
            throw DecodeError("object trie leaf of the wrong size");
        std::vector<Record> records;
        records.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Record record;
            record.first.bytes = reader.fixed<32>();
            record.second = reader.u64();
            if (!records.empty() && !(records.back().first < record.first))
                throw DecodeError("object trie leaf out of order");
            records.push_back(record);
        }
        reader.finish();
        return make_leaf(depth, std::move(records));
    }
    if (kind != kind_interior)
        throw DecodeError("unknown object trie node");
    const auto present = reader.fixed<32>();
    std::vector<Child> children(256);
    for (size_t i = 0; i < 256; ++i) {
        if (!(present[i / 8] & (1U << (i % 8))))
            continue;
        auto& child = children[i];
        child.offset = reader.u64();
        child.size = reader.u32();
        child.count = reader.u64();
        child.hash.bytes = reader.fixed<32>();
        if (child.offset == unsaved || child.count == 0)
            throw DecodeError("object trie child not saved");
    }
    reader.finish();
    auto node = make_interior(depth, std::move(children));
    if (node->count <= leaf_max)
        throw DecodeError("object trie interior node too small");
    return node;
}

ObjectTrie::NodePtr ObjectTrie::load(const Child& child) const {
    if (child.node)
        return child.node;
    if (child.empty() || child.offset == unsaved)
        throw std::logic_error("object trie: loading an empty child");
    if (const auto found = cache_.find(child.offset); found != cache_.end()) {
        recent_.splice(recent_.begin(), recent_, found->second.recent);
        return found->second.node;
    }
    auto node = decode(nodes_->read_at(child.offset));
    if (node->hash != child.hash || node->count != child.count)
        throw std::runtime_error("object trie node at " + std::to_string(child.offset) +
                                 " does not match its parent");
    ++loads_;
    recent_.push_front(child.offset);
    const auto bytes = footprint(*node);
    cache_.emplace(child.offset, Cached{node, bytes, recent_.begin()});
    cache_used_ += bytes;
    while (cache_used_ > options_.cache_bytes && recent_.size() > 1) {
        const auto victim = recent_.back();
        recent_.pop_back();
        const auto found = cache_.find(victim);
        cache_used_ -= found->second.bytes;
        cache_.erase(found);
    }
    return node;
}

ObjectTrie::Child ObjectTrie::build(uint8_t depth, std::vector<Record> records) const {
    if (records.empty())
        return {};
    if (records.size() <= leaf_max)
        return child_of(make_leaf(depth, std::move(records)));
    if (depth >= 32)
        throw std::logic_error("object trie: more records than ids allow");
    std::vector<Child> children(256);
    auto begin = records.begin();
    while (begin != records.end()) {
        const auto byte = begin->first.bytes[depth];
        auto end = std::find_if(begin, records.end(),
                                [&](const Record& r) { return r.first.bytes[depth] != byte; });
        children[byte] =
            build(static_cast<uint8_t>(depth + 1), std::vector<Record>(begin, end));
        begin = end;
    }
    return child_of(make_interior(depth, std::move(children)));
}

void ObjectTrie::gather(const Child& child, std::vector<Record>& out, bool superseding) {
    if (child.empty())
        return;
    const auto node = load(child);
    if (superseding && child.offset != unsaved)
        superseded_bytes_ += child.size;
    if (node->leaf) {
        out.insert(out.end(), node->records.begin(), node->records.end());
        return;
    }
    for (const auto& below : node->children)
        gather(below, out, superseding);
}

ObjectTrie::Child ObjectTrie::mutate(const Child& child, uint8_t depth,
                                     std::span<const Change> changes) {
    if (changes.empty())
        return child;
    const NodePtr node = child.empty() ? NodePtr{} : load(child);
    if (!node || node->leaf) {
        if (node && child.offset != unsaved)
            superseded_bytes_ += child.size;
        static const std::vector<Record> none;
        const auto& old = node ? node->records : none;
        std::vector<Record> merged;
        merged.reserve(old.size() + changes.size());
        auto at = old.begin();
        for (const auto& change : changes) {
            while (at != old.end() && at->first < change.id)
                merged.push_back(*at++);
            if (at != old.end() && at->first == change.id)
                ++at;
            if (change.value)
                merged.emplace_back(change.id, *change.value);
        }
        merged.insert(merged.end(), at, old.end());
        return build(depth, std::move(merged));
    }
    if (child.offset != unsaved)
        superseded_bytes_ += child.size;
    auto children = node->children;
    auto begin = changes.begin();
    while (begin != changes.end()) {
        const auto byte = begin->id.bytes[depth];
        auto end = std::find_if(begin, changes.end(),
                                [&](const Change& c) { return c.id.bytes[depth] != byte; });
        children[byte] = mutate(children[byte], static_cast<uint8_t>(depth + 1),
                                std::span<const Change>(&*begin, static_cast<size_t>(end - begin)));
        begin = end;
    }
    uint64_t count = 0;
    for (const auto& below : children)
        count += below.count;
    if (count <= leaf_max) {
        std::vector<Record> all;
        all.reserve(count);
        for (const auto& below : children)
            gather(below, all, true);
        return build(depth, std::move(all));
    }
    return child_of(make_interior(depth, std::move(children)));
}

ObjectTrie::Child ObjectTrie::save(const Child& child, SealedJournal& into) {
    if (child.empty() || child.offset != unsaved)
        return child;
    const auto& node = *child.node;
    NodePtr saved;
    if (node.leaf) {
        saved = child.node;
    } else {
        std::vector<Child> children(256);
        for (size_t i = 0; i < 256; ++i) {
            children[i] = save(node.children[i], into);
            if (children[i].offset != unsaved)
                children[i].node.reset();
        }
        saved = make_interior(node.depth, std::move(children));
    }
    const auto bytes = encode(*saved);
    const auto before = into.bytes();
    const auto offset = into.append_unsynced(bytes);
    Child out;
    out.offset = offset;
    out.size = static_cast<uint32_t>(into.bytes() - before);
    out.count = saved->count;
    out.hash = saved->hash;
    return out;
}

void ObjectTrie::publish_root() const {
    Writer writer;
    writer.fixed(root_magic);
    writer.u64(generation_);
    writer.u64(root_.offset);
    writer.u32(root_.size);
    writer.u64(root_.count);
    writer.fixed(root_.hash.bytes);
    writer.u64(live_bytes_);
    const auto check = sha256(writer.data());
    writer.fixed(check.bytes);
    const auto& bytes = writer.data();
    durable_replace_file(dir_ / "root",
                         std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                          bytes.size()));
}

ObjectTrie::ObjectTrie(std::filesystem::path dir, std::array<uint8_t, 32> key, Options options)
    : dir_(std::move(dir)), key_(key), options_(options) {
    std::filesystem::create_directories(dir_);
    if (std::ifstream in(dir_ / "root", std::ios::binary); in) {
        const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
        Reader reader(bytes);
        if (reader.fixed<8>() != root_magic)
            throw DecodeError("object trie root of another format");
        generation_ = reader.u64();
        root_.offset = reader.u64();
        root_.size = reader.u32();
        root_.count = reader.u64();
        root_.hash.bytes = reader.fixed<32>();
        live_bytes_ = reader.u64();
        const auto check = reader.fixed<32>();
        reader.finish();
        if (sha256(std::span<const uint8_t>(bytes.data(), bytes.size() - 32)).bytes != check)
            throw DecodeError("object trie root is damaged");
        if (root_.count == 0)
            root_ = {};
    }
    // Node files of other generations are a rewrite that was not published,
    // or one that was and whose predecessor was not yet removed.
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("nodes-") && entry.path() != nodes_path(generation_)) {
            std::error_code ec;
            std::filesystem::remove(entry.path(), ec);
        }
    }
    nodes_ = std::make_unique<SealedJournal>(nodes_path(generation_), key_, nodes_aad, max_frame);
    if (!root_.empty())
        root_.node = load(root_);
    journal_ = std::make_unique<SealedJournal>(dir_ / "journal.log", key_, journal_aad, max_frame);
    journal_->replay(
        [&](std::span<const uint8_t> plain) {
            Reader reader(plain);
            if (reader.u8() != journal_version)
                throw DecodeError("object trie journal of another version");
            const auto count = reader.u32();
            if (count > changes_per_frame)
                throw DecodeError("object trie journal frame too large");
            std::vector<Change> changes;
            changes.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Change change;
                change.id.bytes = reader.fixed<32>();
                if (reader.u8())
                    change.value = reader.u64();
                if (!changes.empty() && !(changes.back().id < change.id))
                    throw DecodeError("object trie journal frame out of order");
                changes.push_back(change);
            }
            reader.finish();
            apply_unjournaled(changes);
        },
        max_frame, "object trie journal");
    if (journal_->bytes() >= options_.checkpoint_bytes)
        checkpoint();
}

ObjectTrie::~ObjectTrie() = default;

void ObjectTrie::apply_unjournaled(std::span<const Change> changes) {
    root_ = mutate(root_, 0, changes);
}

void ObjectTrie::apply(std::span<const Change> input) {
    if (input.empty())
        return;
    // Sorted by id, a later change to the same id winning.
    std::vector<Change> changes(input.begin(), input.end());
    std::stable_sort(changes.begin(), changes.end(),
                     [](const Change& a, const Change& b) { return a.id < b.id; });
    std::vector<Change> unique;
    unique.reserve(changes.size());
    for (auto& change : changes) {
        if (!unique.empty() && unique.back().id == change.id)
            unique.back() = change;
        else
            unique.push_back(change);
    }
    for (size_t from = 0; from < unique.size(); from += changes_per_frame) {
        const auto part = std::span<const Change>(unique).subspan(
            from, std::min(changes_per_frame, unique.size() - from));
        Writer writer;
        writer.u8(journal_version);
        writer.u32(static_cast<uint32_t>(part.size()));
        for (const auto& change : part) {
            writer.fixed(change.id.bytes);
            writer.u8(change.value ? 1 : 0);
            if (change.value)
                writer.u64(*change.value);
        }
        journal_->append(writer.data());
        apply_unjournaled(part);
    }
    if (journal_->bytes() >= options_.checkpoint_bytes)
        checkpoint();
}

void ObjectTrie::checkpoint() {
    const auto before = nodes_->bytes();
    auto saved = save(root_, *nodes_);
    nodes_->sync();
    const auto written = nodes_->bytes() - before;
    live_bytes_ = live_bytes_ + written - std::min(live_bytes_ + written, superseded_bytes_);
    superseded_bytes_ = 0;
    root_ = saved;
    root_.node.reset();
    publish_root();
    journal_->reset();
    ++checkpoints_;
    if (!root_.empty())
        root_.node = load(root_);

    // More superseded than live, and enough of it to matter: rewrite.
    const auto file = nodes_->bytes();
    if (file > 2 * live_bytes_ && file - live_bytes_ > options_.rewrite_floor_bytes) {
        const auto next = generation_ + 1;
        std::error_code ec;
        std::filesystem::remove(nodes_path(next), ec);
        auto fresh = std::make_unique<SealedJournal>(nodes_path(next), key_, nodes_aad, max_frame);
        // Every node is copied: unsave the tree, then save it into the new file.
        std::function<Child(const Child&)> copy = [&](const Child& child) -> Child {
            if (child.empty())
                return {};
            const auto node = load(child);
            if (node->leaf)
                return child_of(make_leaf(node->depth, node->records));
            std::vector<Child> children(256);
            for (size_t i = 0; i < 256; ++i)
                children[i] = copy(node->children[i]);
            return child_of(make_interior(node->depth, std::move(children)));
        };
        auto rewritten = save(copy(root_), *fresh);
        fresh->sync();
        const auto old = nodes_path(generation_);
        generation_ = next;
        root_ = rewritten;
        live_bytes_ = fresh->bytes();
        nodes_ = std::move(fresh);
        cache_.clear();
        recent_.clear();
        cache_used_ = 0;
        publish_root();
        std::filesystem::remove(old, ec);
        ++rewrites_;
        if (!root_.empty())
            root_.node = load(root_);
    }
}

std::optional<uint64_t> ObjectTrie::get(const ObjectId& id) const {
    if (root_.empty())
        return std::nullopt;
    auto node = load(root_);
    while (!node->leaf) {
        const auto& child = node->children[id.bytes[node->depth]];
        if (child.empty())
            return std::nullopt;
        node = load(child);
    }
    const auto found = std::lower_bound(
        node->records.begin(), node->records.end(), id,
        [](const Record& record, const ObjectId& key) { return record.first < key; });
    if (found == node->records.end() || found->first != id)
        return std::nullopt;
    return found->second;
}

void ObjectTrie::collect(const Child& child, const std::optional<ObjectId>& after, bool bounded,
                         size_t limit, std::vector<Record>& out) const {
    if (child.empty() || out.size() >= limit)
        return;
    const auto node = load(child);
    if (node->leaf) {
        for (const auto& record : node->records) {
            if (out.size() >= limit)
                return;
            if (bounded && !(*after < record.first))
                continue;
            out.push_back(record);
        }
        return;
    }
    const size_t start = bounded ? after->bytes[node->depth] : 0;
    for (size_t i = start; i < 256 && out.size() < limit; ++i)
        collect(node->children[i], after, bounded && i == start, limit, out);
}

std::vector<ObjectTrie::Record> ObjectTrie::next(const std::optional<ObjectId>& after,
                                                 size_t limit) const {
    std::vector<Record> out;
    collect(root_, after, after.has_value(), limit, out);
    return out;
}

uint64_t ObjectTrie::size() const noexcept {
    return root_.count;
}

Hash256 ObjectTrie::root_hash() const noexcept {
    return root_.hash;
}

ObjectTrie::Stats ObjectTrie::stats() const noexcept {
    return {journal_->bytes(), nodes_->bytes(), live_bytes_, cache_used_,
            cache_.size(),     loads_,          checkpoints_, rewrites_};
}

} // namespace macha
