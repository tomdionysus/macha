// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only forensic decoder for a metadata replica's on-disk state. Unlike
// macha-metadata-repair this never constructs a MetadataReplica, so it can
// examine a quarantined (`*.corrupt.<ts>`) or otherwise unloadable state
// directory without triggering recovery, and it walks the history exactly the
// way MetadataReplica::materialized_locked() does so a reconstruction failure
// can be pinned to the precise frame that breaks the chain.
//
//   macha-metadata-dump <cluster.key> <history.log> [heads.meta] [--all] [--stats]
//
// Prints one line per anomalous history frame (delta whose parent is absent
// or whose generation is not parent+1, duplicate hash, undecodable frame),
// a summary, and for every accepted head in heads.meta the delta chain walk
// with the exact point at which materialization would fail. --all prints
// every frame. --stats materializes each reconstructible head and prints
// what its snapshot is made of (entries, extents, tombstones, conflicts,
// node status) and how many encoded bytes each part accounts for -- the
// measurement behind discipline 4 of the self-healing plan.
#include "codec.hpp"
#include "crypto.hpp"
#include "metadata.hpp"
#include "namespace_tree.hpp"

#include <algorithm>

#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace macha;

namespace {
constexpr std::array<uint8_t, 8> MH{'D', 'H', 'T', 'M', 'H', 'S', 'T', '1'},
    MA{'D', 'H', 'T', 'M', 'A', 'C', 'C', '1'};

struct Frame {
    uint64_t offset{};
    uint64_t length{}; // encrypted frame bytes after the 4-byte length prefix
    uint64_t generation{};
    Hash256 previous{}, hash{};
    bool previous_known{};
    MetadataHistoryEntry::Body body{};
    std::vector<Hash256> merge_parents;
    size_t payload_bytes{};
};

std::string h(const Hash256& value) {
    return hex(value.bytes).substr(0, 16);
}

const char* body_name(MetadataHistoryEntry::Body body) {
    return body == MetadataHistoryEntry::Body::delta ? "delta" : "full";
}

void print(const Frame& frame, const std::string& note = {}) {
    std::cout << "offset=" << frame.offset << " gen=" << frame.generation
              << " hash=" << h(frame.hash) << " prev=" << h(frame.previous)
              << " prev_known=" << (frame.previous_known ? 1 : 0)
              << " body=" << body_name(frame.body) << " payload=" << frame.payload_bytes
              << " merge_parents=" << frame.merge_parents.size();
    for (const auto& parent : frame.merge_parents)
        std::cout << " mp=" << h(parent);
    if (!note.empty())
        std::cout << "  <-- " << note;
    std::cout << '\n';
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: macha-metadata-dump <cluster.key> <history.log> [heads.meta] "
                     "[--all] [--stats] [--tree]\n";
        return 2;
    }
    bool all = false;
    bool stats = false;
    bool tree = false;
    std::string heads_path;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--all")
            all = true;
        else if (std::string(argv[i]) == "--stats")
            stats = true;
        else if (std::string(argv[i]) == "--tree") {
            // Implies --stats: the tree is built from the materialised head,
            // which is what --stats already produces.
            tree = true;
            stats = true;
        }
        else
            heads_path = argv[i];
    }
    const auto key = load_cluster_keys(argv[1]).storage;
    const std::filesystem::path history_path = argv[2];

    std::map<Hash256, Frame> index;
    std::vector<Frame> order;
    size_t anomalies = 0, full_count = 0, delta_count = 0, gap_deltas = 0;
    const uint64_t file_size = std::filesystem::file_size(history_path);
    std::ifstream stream(history_path, std::ios::binary);
    uint64_t offset = 0;
    while (offset + 4 <= file_size) {
        std::array<uint8_t, 4> header_bytes{};
        stream.read(reinterpret_cast<char*>(header_bytes.data()), 4);
        Reader header(header_bytes);
        const uint64_t length = header.u32();
        if (offset + 4 + length > file_size) {
            std::cout << "offset=" << offset << " incomplete trailing frame length=" << length
                      << '\n';
            break;
        }
        Bytes frame_bytes(length);
        stream.read(reinterpret_cast<char*>(frame_bytes.data()),
                    static_cast<std::streamsize>(length));
        Frame frame;
        frame.offset = offset;
        frame.length = length;
        std::string note;
        try {
            Reader envelope(frame_bytes);
            auto nonce = envelope.fixed<12>();
            auto tag = envelope.fixed<16>();
            auto ciphertext = envelope.bytes();
            envelope.finish();
            auto entry = decode_metadata_history_entry(aes_gcm_open(key, nonce, tag, ciphertext, MH));
            frame.generation = entry.generation;
            frame.previous = entry.previous;
            frame.hash = entry.hash;
            frame.previous_known = entry.previous_known;
            frame.body = entry.body;
            frame.merge_parents = entry.merge_parents;
            frame.payload_bytes = entry.payload.size();
        } catch (const std::exception& error) {
            std::cout << "offset=" << offset << " UNDECODABLE: " << error.what() << '\n';
            ++anomalies;
            offset += 4 + length;
            continue;
        }
        if (frame.body == MetadataHistoryEntry::Body::delta) {
            ++delta_count;
            auto parent = index.find(frame.previous);
            if (!frame.previous_known)
                note = "delta without previous_known";
            else if (parent == index.end())
                note = "delta parent ABSENT from index";
            else if (!metadata_delta_succession_valid(parent->second.generation, frame.generation))
                note = "delta parent generation not older: parent gen=" +
                       std::to_string(parent->second.generation) + " child gen=" +
                       std::to_string(frame.generation);
            else if (parent->second.generation + 1 != frame.generation)
                ++gap_deltas; // Legitimate (merge commit); pre-0.27.0 readers rejected these.
        } else {
            ++full_count;
        }
        if (index.contains(frame.hash))
            note = "DUPLICATE hash";
        if (!note.empty())
            ++anomalies;
        if (all || !note.empty())
            print(frame, note);
        index.emplace(frame.hash, frame);
        order.push_back(frame);
        offset += 4 + length;
    }
    std::cout << "history: frames=" << order.size() << " full=" << full_count
              << " delta=" << delta_count << " gap_deltas=" << gap_deltas
              << " anomalies=" << anomalies << " bytes=" << offset
              << "/" << file_size;
    if (!order.empty())
        std::cout << " first_gen=" << order.front().generation
                  << " last_gen=" << order.back().generation;
    std::cout << '\n';

    if (heads_path.empty())
        return 0;

    std::ifstream heads_stream(heads_path, std::ios::binary);
    Bytes heads_bytes(std::istreambuf_iterator<char>(heads_stream), {});
    Reader reader(heads_bytes);
    auto magic = reader.raw(8);
    if (!std::equal(magic.begin(), magic.end(), MA.begin())) {
        std::cerr << "bad heads.meta magic\n";
        return 1;
    }
    auto nonce = reader.fixed<12>();
    auto tag = reader.fixed<16>();
    auto ciphertext = reader.bytes();
    reader.finish();
    auto heads = decode_metadata_acceptance_set(aes_gcm_open(key, nonce, tag, ciphertext, MA));
    std::cout << "accepted heads: " << heads.size() << '\n';
    for (const auto& head : heads) {
        std::cout << "head gen=" << head.generation << " hash=" << hex(head.hash.bytes)
                  << " required=" << head.required << " replicas=" << head.replicas.size() << '\n';
        auto found = index.find(head.hash);
        if (found == index.end()) {
            std::cout << "  NOT IN HISTORY INDEX\n";
            continue;
        }
        // Mirror MetadataReplica::materialized_locked(): walk deltas back to a
        // full anchor, then check each child is previous==parent.hash and
        // metadata_delta_succession_valid(parent, child).
        std::vector<Frame> chain;
        std::set<Hash256> seen;
        Frame cursor = found->second;
        bool broken = false;
        while (cursor.body == MetadataHistoryEntry::Body::delta) {
            if (!seen.insert(cursor.hash).second || !cursor.previous_known) {
                std::cout << "  chain broken at "; print(cursor, "cycle or previous unknown");
                broken = true;
                break;
            }
            chain.push_back(cursor);
            auto parent = index.find(cursor.previous);
            if (parent == index.end()) {
                std::cout << "  chain broken at "; print(cursor, "parent absent");
                broken = true;
                break;
            }
            cursor = parent->second;
        }
        if (broken)
            continue;
        std::cout << "  anchor "; print(cursor);
        Frame working = cursor;
        bool ok = true;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (it->previous != working.hash ||
                !metadata_delta_succession_valid(working.generation, it->generation)) {
                std::cout << "  MATERIALIZATION WOULD FAIL at ";
                print(*it, "parent gen=" + std::to_string(working.generation) + " child gen=" +
                               std::to_string(it->generation));
                ok = false;
                break;
            }
            working = *it;
        }
        std::cout << "  chain length=" << chain.size() << " reconstructible=" << (ok ? "yes" : "NO")
                  << '\n';
        if (!ok || !stats)
            continue;

        // Materialize exactly as the replica would, then attribute the
        // encoded bytes to each part of the snapshot by re-encoding with
        // that part removed.
        auto load = [&](const Frame& frame) {
            std::ifstream in(history_path, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(frame.offset + 4));
            Bytes bytes(frame.length);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            Reader envelope(bytes);
            auto nonce = envelope.fixed<12>();
            auto tag = envelope.fixed<16>();
            auto ciphertext = envelope.bytes();
            envelope.finish();
            return decode_metadata_history_entry(aes_gcm_open(key, nonce, tag, ciphertext, MH));
        };
        MetadataSnapshot snapshot;
        try {
            snapshot = decode_snapshot(load(cursor).payload);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                apply_metadata_delta_in_place(snapshot, decode_metadata_delta(load(*it).payload));
        } catch (const std::exception& error) {
            std::cout << "  stats unavailable: " << error.what() << '\n';
            continue;
        }
        size_t files = 0, directories = 0, extents = 0, holes = 0;
        for (const auto& [path, value] : snapshot.entries) {
            if (value.type == EntryType::directory)
                ++directories;
            else
                ++files;
            extents += value.extents.size();
            for (const auto& extent : value.extents)
                holes += extent.hole ? 1 : 0;
        }
        const auto full = encode_snapshot(snapshot).size();
        auto without = [&](const std::function<void(MetadataSnapshot&)>& strip) {
            MetadataSnapshot copy = snapshot;
            strip(copy);
            return full - encode_snapshot(copy).size();
        };
        const auto garbage_bytes = without([](MetadataSnapshot& c) { c.garbage.clear(); });
        const auto conflict_bytes = without([](MetadataSnapshot& c) { c.conflicts.clear(); });
        const auto node_status_bytes = without([](MetadataSnapshot& c) { c.node_status.clear(); });
        const auto extent_bytes = without([](MetadataSnapshot& c) {
            for (auto& [path, value] : c.entries)
                value.extents.clear();
        });
        const auto entry_bytes = without([](MetadataSnapshot& c) { c.entries.clear(); });
        std::cout << "  snapshot: encoded_bytes=" << full << " entries=" << snapshot.entries.size()
                  << " (files=" << files << " directories=" << directories << ") extents=" << extents
                  << " holes=" << holes << " tombstones=" << snapshot.garbage.size()
                  << " conflicts=" << snapshot.conflicts.size()
                  << " node_status=" << snapshot.node_status.size()
                  << " identity_resets=" << snapshot.identity_resets.size()
                  << " mutation_sequences=" << snapshot.mutation_sequences.size()
                  << " merge_parents=" << snapshot.merge_parents.size() << '\n';
        std::cout << "  bytes: entries=" << entry_bytes << " (of which extents=" << extent_bytes
                  << ", paths+attrs=" << (entry_bytes - extent_bytes) << ") tombstones="
                  << garbage_bytes << " conflicts=" << conflict_bytes
                  << " node_status=" << node_status_bytes << " other="
                  << (full - entry_bytes - garbage_bytes - conflict_bytes - node_status_bytes)
                  << '\n';
        if (tree) {
            // Stage B of the Merkle plan, measured against a real namespace
            // rather than a generated one. Everything here is offline and
            // read-only: the tree is built in memory from the head that
            // --stats just materialised, and nothing is written to the record,
            // the history or the control store.
            MemoryNamespaceNodeStore nodes;
            const auto root = build_namespace_tree(snapshot.entries, nodes);
            const auto shape = namespace_tree_stats(root, nodes);
            std::cout << "  tree: root=" << to_string(root).substr(0, 16)
                      << " nodes=" << nodes.nodes() << " bytes=" << nodes.bytes()
                      << " leaves=" << shape.leaves << " branches=" << shape.branches
                      << " extent_nodes=" << shape.extent_nodes << " depth=" << shape.depth
                      << " largest_node=" << shape.largest_node_bytes << '\n';

            // The number the whole plan turns on: what one ordinary write
            // costs. Today it is the entire library -- re-serialised,
            // re-hashed and replicated -- because the record payload IS the
            // namespace and its identity is a hash over those bytes.
            //
            // Pick a real file rather than a synthetic one, and pick the
            // median by extent count so the answer is not flattered by a
            // one-extent file or distorted by the largest.
            std::vector<std::pair<size_t, std::string>> by_extents;
            for (const auto& [path, value] : snapshot.entries)
                if (value.type != EntryType::directory)
                    by_extents.emplace_back(value.extents.size(), path);
            if (!by_extents.empty()) {
                std::sort(by_extents.begin(), by_extents.end());
                const auto& median = by_extents[by_extents.size() / 2];
                auto touched = snapshot.entries;
                auto found = touched.find(median.second);
                if (found != touched.end()) {
                    // An mtime bump: the commonest namespace write there is,
                    // and a value change on an existing path, which is the
                    // case key-only boundaries exist to keep cheap.
                    found->second.mtime_ns += 1;
                    nodes.forget_written();
                    const auto after = build_namespace_tree(touched, nodes);
                    uint64_t dirty_bytes = 0;
                    for (const auto& id : nodes.written())
                        if (auto body = nodes.get(id)) dirty_bytes += body->size();
                    std::cout << "  tree: one mtime change on a median file ("
                              << median.first << " extents) rewrote "
                              << nodes.written().size() << " nodes, " << dirty_bytes
                              << " bytes; root " << (after == root ? "UNCHANGED (bug)" : "moved")
                              << '\n';
                    std::cout << "  tree: today the same write re-serialises and re-hashes "
                              << full << " bytes, the whole namespace\n";
                }
            }
        }
        // Encoded bytes are what replication and the journal carry; resident
        // bytes are what every node holds while it is running. They are
        // different numbers and the second one is the larger, so report both
        // rather than letting the file size stand in for the cost.
        uint64_t extent_capacity = 0;
        for (const auto& [path, value] : snapshot.entries)
            extent_capacity += value.extents.capacity();
        const auto resident = snapshot_resident_bytes(snapshot);
        std::cout << "  resident: decoded_bytes=" << resident
                  << " encoded_payload_bytes=" << full
                  << " materialization_bytes=" << (resident + full)
                  << " extent_slots=" << extent_capacity << " (in use " << extents << ")"
                  << " sizeof_fs_entry=" << sizeof(FsEntry)
                  << " sizeof_extent_ref=" << sizeof(ExtentRef)
                  << " sizeof_map_value=" << sizeof(std::map<std::string, FsEntry>::value_type)
                  << " sizeof_snapshot=" << sizeof(MetadataSnapshot) << '\n';
        if (snapshot.extent_size && extents) {
            // Extents, not files, are what scales with a media library, so the
            // only projection worth printing is per unit of stored content.
            const long double library_tb =
                static_cast<long double>(extents) *
                static_cast<long double>(snapshot.extent_size) / (1024.0L * 1024.0L * 1024.0L * 1024.0L);
            std::cout << "  scaling: extent_size=" << snapshot.extent_size
                      << " library_tib=" << library_tb << " decoded_bytes_per_tib="
                      << static_cast<uint64_t>(resident / library_tb)
                      << " materialization_bytes_per_tib="
                      << static_cast<uint64_t>((resident + full) / library_tb) << '\n';
        }
        if (!snapshot.conflicts.empty()) {
            size_t namespace_kind = 0, identical = 0, both_files = 0, one_side_absent = 0,
                   with_base = 0;
            std::map<std::string, size_t> by_heads;
            for (const auto& [id, conflict] : snapshot.conflicts) {
                if (conflict.kind == MetadataConflictKind::namespace_entry)
                    ++namespace_kind;
                if (conflict.left_entry && conflict.right_entry &&
                    *conflict.left_entry == *conflict.right_entry)
                    ++identical;
                if (conflict.left_entry && conflict.right_entry)
                    ++both_files;
                else
                    ++one_side_absent;
                if (conflict.base_entry)
                    ++with_base;
                ++by_heads[h(conflict.left_head) + "/" + h(conflict.right_head)];
            }
            std::cout << "  conflicts: namespace_entry=" << namespace_kind
                      << " both_alternatives_present=" << both_files
                      << " one_side_absent=" << one_side_absent << " identical_alternatives="
                      << identical << " with_base=" << with_base
                      << " distinct_head_pairs=" << by_heads.size() << '\n';
            size_t shown = 0;
            for (const auto& [id, conflict] : snapshot.conflicts) {
                if (shown++ >= 5)
                    break;
                std::cout << "    " << id << " key=" << conflict.key
                          << " left=" << (conflict.left_entry ? std::to_string(conflict.left_entry->size) + "B/" + std::to_string(conflict.left_entry->extents.size()) + "x" : "absent")
                          << " right=" << (conflict.right_entry ? std::to_string(conflict.right_entry->size) + "B/" + std::to_string(conflict.right_entry->extents.size()) + "x" : "absent")
                          << " base=" << (conflict.base_entry ? "yes" : "no") << '\n';
            }
        }
        if (!snapshot.entries.empty())
            std::cout << "  per_entry_bytes=" << full / snapshot.entries.size()
                      << " per_entry_bytes_excluding_extents="
                      << (full - extent_bytes) / snapshot.entries.size() << '\n';
    }
    return 0;
}
