// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/reference_counts.hpp"

#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>

namespace macha {

namespace {

constexpr std::array<uint8_t, 8> state_magic{'M', 'R', 'E', 'F', '0', '0', '0', '1'};

ObjectTrie::Options options_for(size_t cache_bytes) {
    ObjectTrie::Options options;
    options.cache_bytes = cache_bytes;
    return options;
}

Bytes encode_count(uint64_t count) {
    Writer writer;
    writer.u64(count);
    return writer.take();
}

uint64_t decode_count(const Bytes& value) {
    Reader reader(value);
    const auto count = reader.u64();
    reader.finish();
    return count;
}

// One record per distinct id, its value the number of times it appears.
std::vector<ObjectTrie::Record> counted(std::vector<ObjectId> ids) {
    std::sort(ids.begin(), ids.end());
    std::vector<ObjectTrie::Record> records;
    for (auto it = ids.begin(); it != ids.end();) {
        const auto end = std::upper_bound(it, ids.end(), *it);
        records.emplace_back(*it, encode_count(static_cast<uint64_t>(end - it)));
        it = end;
    }
    return records;
}

// The changes `in` and `out` make to `trie`'s counts, or none when a count
// would go below zero.
std::optional<std::vector<ObjectTrie::Change>> changes_for(const ObjectTrie& trie,
                                                           const std::vector<ObjectId>& in,
                                                           const std::vector<ObjectId>& out) {
    std::map<ObjectId, int64_t> delta;
    for (const auto& id : in)
        ++delta[id];
    for (const auto& id : out)
        --delta[id];
    std::vector<ObjectTrie::Change> changes;
    for (const auto& [id, by] : delta) {
        if (by == 0)
            continue;
        const auto value = trie.get(id);
        const auto now = static_cast<int64_t>(value ? decode_count(*value) : 0);
        if (now + by < 0)
            return std::nullopt;
        changes.push_back({id, now + by == 0 ? std::nullopt
                                             : std::optional(encode_count(
                                                   static_cast<uint64_t>(now + by)))});
    }
    return changes;
}

} // namespace

ReferenceCounts::ReferenceCounts(std::filesystem::path dir, std::array<uint8_t, 32> key,
                                 size_t cache_bytes)
    : dir_(std::move(dir)), extents_(dir_ / "extents", key, options_for(cache_bytes - cache_bytes / 4)),
      nodes_(dir_ / "nodes", key, options_for(cache_bytes / 4)) {
    // A missing, damaged or dirty state leaves no root: the next count walks.
    std::ifstream in(dir_ / "state", std::ios::binary);
    if (!in)
        return;
    const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
    try {
        Reader reader(bytes);
        if (reader.fixed<8>() != state_magic)
            return;
        const bool clean = reader.u8() == 1;
        const bool has_root = reader.u8() == 1;
        ObjectId root;
        root.bytes = reader.fixed<32>();
        Totals totals;
        totals.entries = reader.u64();
        totals.extent_count = reader.u64();
        reader.finish();
        if (!clean)
            return;
        if (has_root)
            root_ = root;
        totals_ = totals;
    } catch (const std::exception& error) {
        Log::warn("reference counts state unreadable path=" + (dir_ / "state").string() +
                  " error=" + error.what());
    }
}

void ReferenceCounts::write_state(bool clean) const {
    Writer writer;
    writer.fixed(state_magic);
    writer.u8(clean ? 1 : 0);
    writer.u8(root_ ? 1 : 0);
    writer.fixed(root_ ? root_->bytes : ObjectId{}.bytes);
    writer.u64(totals_.entries);
    writer.u64(totals_.extent_count);
    const auto& bytes = writer.data();
    durable_replace_file(dir_ / "state",
                         std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                          bytes.size()));
}

void ReferenceCounts::reset(const std::optional<ObjectId>& root, std::vector<ObjectId> extents,
                            std::vector<ObjectId> nodes, Totals totals) {
    write_state(false);
    extents_.replace_all(counted(std::move(extents)));
    nodes_.replace_all(counted(std::move(nodes)));
    root_ = root;
    totals_ = totals;
    write_state(true);
}

bool ReferenceCounts::change(const ObjectId& root, std::vector<ObjectId> extents_in,
                             std::vector<ObjectId> extents_out, std::vector<ObjectId> nodes_in,
                             std::vector<ObjectId> nodes_out, Totals totals) {
    // Checked whole before anything is written, so a refusal changes nothing.
    auto extent_changes = changes_for(extents_, extents_in, extents_out);
    auto node_changes = changes_for(nodes_, nodes_in, nodes_out);
    if (!extent_changes || !node_changes)
        return false;
    write_state(false);
    extents_.apply(*extent_changes);
    nodes_.apply(*node_changes);
    root_ = root;
    totals_ = totals;
    write_state(true);
    return true;
}

ReferenceCounts::Views ReferenceCounts::views() const {
    return {extents_.snapshot(), nodes_.snapshot()};
}

} // namespace macha
