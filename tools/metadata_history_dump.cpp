// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only forensic decoder for a metadata replica's on-disk state. Unlike
// macha-metadata-repair this never constructs a MetadataReplica, so it can
// examine a quarantined (`*.corrupt.<ts>`) or otherwise unloadable state
// directory without triggering recovery, and it walks the history exactly the
// way MetadataReplica::materialized_locked() does so a reconstruction failure
// can be pinned to the precise frame that breaks the chain.
//
//   macha-metadata-dump <cluster.key> <history.log> [heads.meta] [--all]
//
// Prints one line per anomalous history frame (delta whose parent is absent
// or whose generation is not parent+1, duplicate hash, undecodable frame),
// a summary, and for every accepted head in heads.meta the delta chain walk
// with the exact point at which materialization would fail. --all prints
// every frame.
#include "codec.hpp"
#include "crypto.hpp"
#include "metadata.hpp"

#include <fstream>
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
        std::cerr << "usage: macha-metadata-dump <cluster.key> <history.log> [heads.meta] [--all]\n";
        return 2;
    }
    bool all = false;
    std::string heads_path;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--all")
            all = true;
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
    }
    return 0;
}
