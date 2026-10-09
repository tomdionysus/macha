// SPDX-License-Identifier: GPL-3.0-or-later
// The object ledger's primitives: the sealed journal it shares with the
// retention store, and the per-class trie.
#include "codec.hpp"
#include "crypto.hpp"
#include "ledger/held_ledger.hpp"
#include "ledger/object_trie.hpp"
#include "ledger/reference_counts.hpp"
#include "storage/local_store.hpp"
#include "storage/sealed_journal.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

constexpr std::array<uint8_t, 8> test_aad{'M', 'A', 'C', 'H', 'T', 'E', 'S', 'T'};

std::array<uint8_t, 32> key_of(uint8_t fill) {
    std::array<uint8_t, 32> key{};
    key.fill(fill);
    return key;
}

Bytes frame_text(int i) {
    const auto text = "frame-" + std::to_string(i);
    return Bytes(text.begin(), text.end());
}

std::vector<std::string> replay_all(SealedJournal& journal) {
    std::vector<std::string> seen;
    journal.replay(
        [&](std::span<const uint8_t> plain) { seen.emplace_back(plain.begin(), plain.end()); },
        1U << 20, "test journal");
    return seen;
}

MACHA_FAST_TEST("object_ledger", test_a_sealed_journal_replays_what_was_appended_in_order) {
    TempDir dir;
    const auto path = dir.path() / "journal.log";
    {
        SealedJournal journal(path, key_of(1), test_aad, 1U << 20);
        CHECK(replay_all(journal).empty());
        for (int i = 0; i < 5; ++i)
            journal.append(frame_text(i));
        CHECK(journal.frames() == 5);
    }
    SealedJournal reopened(path, key_of(1), test_aad, 1U << 20);
    const auto seen = replay_all(reopened);
    REQUIRE(seen.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(seen[static_cast<size_t>(i)] == "frame-" + std::to_string(i));
    CHECK(reopened.frames() == 5);
    CHECK(reopened.bytes() == std::filesystem::file_size(path));
    // Replaying again changes nothing.
    CHECK(replay_all(reopened).size() == 5);
}

MACHA_FAST_TEST("object_ledger", test_a_torn_journal_keeps_what_came_before_the_tear) {
    TempDir dir;
    const auto path = dir.path() / "journal.log";
    {
        SealedJournal journal(path, key_of(2), test_aad, 1U << 20);
        for (int i = 0; i < 4; ++i)
            journal.append(frame_text(i));
    }
    const auto whole = std::filesystem::file_size(path);
    // Every cut inside the last frame keeps the first three, and the file is
    // truncated to them, so the next append follows them.
    SealedJournal probe(path, key_of(2), test_aad, 1U << 20);
    REQUIRE(replay_all(probe).size() == 4);
    for (uint64_t cut = 1; cut < whole; cut += 7) {
        TempDir copy_dir;
        const auto copy = copy_dir.path() / "journal.log";
        std::filesystem::copy_file(path, copy);
        std::filesystem::resize_file(copy, cut);
        SealedJournal torn(copy, key_of(2), test_aad, 1U << 20);
        const auto seen = replay_all(torn);
        CHECK(seen.size() < 4);
        CHECK(std::filesystem::file_size(copy) == torn.bytes());
        torn.append(frame_text(99));
        SealedJournal after(copy, key_of(2), test_aad, 1U << 20);
        const auto again = replay_all(after);
        REQUIRE(again.size() == seen.size() + 1);
        CHECK(again.back() == "frame-99");
    }
}

MACHA_FAST_TEST("object_ledger", test_a_damaged_or_refused_frame_ends_the_replay_there) {
    TempDir dir;
    const auto path = dir.path() / "journal.log";
    uint64_t second_frame_end = 0;
    {
        SealedJournal journal(path, key_of(3), test_aad, 1U << 20);
        journal.append(frame_text(0));
        journal.append(frame_text(1));
        second_frame_end = journal.bytes();
        journal.append(frame_text(2));
    }
    // A flipped byte in the third frame's ciphertext: two frames survive.
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(std::filesystem::file_size(path) - 1));
        file.put('\x55');
    }
    SealedJournal damaged(path, key_of(3), test_aad, 1U << 20);
    CHECK(replay_all(damaged).size() == 2);
    CHECK(std::filesystem::file_size(path) == second_frame_end);

    // A frame the reader refuses ends the replay as a damaged one does.
    SealedJournal refusing(path, key_of(3), test_aad, 1U << 20);
    int applied = 0;
    refusing.replay(
        [&](std::span<const uint8_t>) {
            if (applied == 1)
                throw std::runtime_error("refused");
            ++applied;
        },
        1U << 20, "test journal");
    CHECK(applied == 1);
    CHECK(refusing.frames() == 1);

    // Another key, or another journal's associated data, opens nothing.
    SealedJournal other_key(path, key_of(4), test_aad, 1U << 20);
    CHECK(replay_all(other_key).empty());
}

MACHA_FAST_TEST("object_ledger", test_a_reset_journal_is_empty_and_keeps_appending) {
    TempDir dir;
    const auto path = dir.path() / "journal.log";
    SealedJournal journal(path, key_of(5), test_aad, 1U << 20);
    journal.append(frame_text(0));
    journal.reset();
    CHECK(journal.frames() == 0);
    CHECK(journal.bytes() == 0);
    CHECK(std::filesystem::file_size(path) == 0);
    journal.append(frame_text(1));
    SealedJournal reopened(path, key_of(5), test_aad, 1U << 20);
    CHECK(replay_all(reopened) == std::vector<std::string>{"frame-1"});

    // A frame over the limit is refused, and nothing is written.
    SealedJournal small(dir.path() / "small.log", key_of(5), test_aad, 8);
    bool refused = false;
    try {
        small.append(frame_text(12345));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(!std::filesystem::exists(dir.path() / "small.log"));
}

// --- The trie ---------------------------------------------------------------

ObjectId id_of(std::mt19937_64& random) {
    ObjectId id;
    for (auto& byte : id.bytes)
        byte = static_cast<uint8_t>(random());
    return id;
}

// Of varying length, empty included.
Bytes value_of(std::mt19937_64& random) {
    Bytes value(random() % 24);
    for (auto& byte : value)
        byte = static_cast<uint8_t>(random());
    return value;
}

ObjectTrie::Options small_options() {
    ObjectTrie::Options options;
    options.cache_bytes = 64 * 1024;
    options.checkpoint_bytes = 64 * 1024;
    options.rewrite_floor_bytes = 32 * 1024;
    return options;
}

// Everything the trie holds, read back in order through next().
std::map<ObjectId, Bytes> contents(const ObjectTrie& trie) {
    std::map<ObjectId, Bytes> out;
    std::optional<ObjectId> after;
    while (true) {
        const auto page = trie.next(after, 97);
        for (const auto& [id, value] : page)
            out.emplace(id, value);
        if (page.size() < 97)
            break;
        after = page.back().first;
    }
    return out;
}

void check_matches(const ObjectTrie& trie, const std::map<ObjectId, Bytes>& model,
                   std::mt19937_64& random) {
    CHECK(trie.size() == model.size());
    CHECK(contents(trie) == model);
    for (const auto& [id, value] : model)
        CHECK(trie.get(id) == std::optional<Bytes>(value));
    for (int i = 0; i < 50; ++i)
        CHECK(!trie.get(id_of(random)).has_value());
}

MACHA_FAST_TEST("object_ledger", test_the_trie_holds_exactly_what_was_written) {
    TempDir dir;
    std::mt19937_64 random(11);
    std::map<ObjectId, Bytes> model;
    std::vector<ObjectId> known;
    ObjectTrie trie(dir.path(), key_of(6), small_options());
    CHECK(trie.size() == 0);
    CHECK(trie.root_hash() == Hash256{});
    for (int round = 0; round < 60; ++round) {
        std::vector<ObjectTrie::Change> batch;
        const int count = 1 + static_cast<int>(random() % 400);
        for (int i = 0; i < count; ++i) {
            const auto roll = random() % 10;
            if (roll < 6 || known.empty()) {
                const auto id = id_of(random);
                known.push_back(id);
                batch.push_back({id, value_of(random)});
            } else if (roll < 8) {
                batch.push_back({known[random() % known.size()], value_of(random)});
            } else {
                batch.push_back({known[random() % known.size()], std::nullopt});
            }
        }
        trie.apply(batch);
        for (const auto& change : batch) {
            if (change.value)
                model[change.id] = *change.value;
            else
                model.erase(change.id);
        }
        CHECK(trie.size() == model.size());
    }
    check_matches(trie, model, random);
    CHECK(trie.stats().checkpoints > 0);
}

MACHA_FAST_TEST("object_ledger", test_the_trie_hash_depends_only_on_its_records) {
    std::mt19937_64 random(12);
    std::vector<ObjectTrie::Change> records;
    for (int i = 0; i < 3000; ++i)
        records.push_back({id_of(random), value_of(random)});
    TempDir one_dir;
    TempDir two_dir;
    ObjectTrie one(one_dir.path(), key_of(7), small_options());
    ObjectTrie two(two_dir.path(), key_of(7), small_options());
    one.apply(records);
    // The same records in another order, by way of extra records written and
    // erased again, and in many small batches.
    auto shuffled = records;
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    // Extra records sharing a first byte, so one subtree grows interior two
    // levels down and must collapse back to a leaf when they go.
    std::vector<ObjectTrie::Change> extra;
    for (int i = 0; i < 500; ++i) {
        auto id = id_of(random);
        id.bytes[0] = 7;
        id.bytes[1] = static_cast<uint8_t>(i % 3);
        extra.push_back({id, value_of(random)});
    }
    two.apply(extra);
    for (size_t from = 0; from < shuffled.size(); from += 37)
        two.apply(std::span<const ObjectTrie::Change>(shuffled).subspan(
            from, std::min<size_t>(37, shuffled.size() - from)));
    for (auto& change : extra)
        change.value.reset();
    two.apply(extra);
    CHECK(one.size() == two.size());
    CHECK(one.root_hash() == two.root_hash());
    CHECK(contents(one) == contents(two));
    // One value different: the hashes differ.
    two.apply(std::vector<ObjectTrie::Change>{{records[0].id, Bytes{9, 9, 9}}});
    CHECK(one.root_hash() != two.root_hash());
}

MACHA_FAST_TEST("object_ledger", test_a_reopened_trie_is_the_one_that_was_closed) {
    TempDir dir;
    std::mt19937_64 random(13);
    std::map<ObjectId, Bytes> model;
    Hash256 hash{};
    {
        ObjectTrie trie(dir.path(), key_of(8), small_options());
        for (int round = 0; round < 20; ++round) {
            std::vector<ObjectTrie::Change> batch;
            for (int i = 0; i < 300; ++i) {
                const auto id = id_of(random);
                const auto value = value_of(random);
                batch.push_back({id, value});
                model[id] = value;
            }
            trie.apply(batch);
        }
        const auto last = id_of(random);
        model[last] = Bytes{1};
        trie.apply(std::vector<ObjectTrie::Change>{{last, Bytes{1}}});
        hash = trie.root_hash();
        // Some changes since the last checkpoint stay only in the journal.
        CHECK(trie.stats().journal_bytes > 0);
    }
    ObjectTrie reopened(dir.path(), key_of(8), small_options());
    CHECK(reopened.root_hash() == hash);
    check_matches(reopened, model, random);
}

// A checkpoint is three writes: the changed nodes, then the root, then the
// emptied journal. A crash after any of them reopens to the same records.
MACHA_FAST_TEST("object_ledger", test_a_crash_at_any_step_of_a_checkpoint_loses_nothing) {
    std::mt19937_64 random(14);
    for (int step = 0; step < 3; ++step) {
        TempDir dir;
        std::map<ObjectId, Bytes> model;
        ObjectTrie::Options options = small_options();
        options.checkpoint_bytes = 1ULL << 40; // only when asked
        const auto snapshot = [&](const std::string& name) {
            const auto from = dir.path() / name;
            const auto to = dir.path() / (name + ".saved");
            std::error_code ec;
            std::filesystem::remove(to, ec);
            if (std::filesystem::exists(from))
                std::filesystem::copy_file(from, to);
        };
        const auto restore = [&](const std::string& name) {
            const auto saved = dir.path() / (name + ".saved");
            std::error_code ec;
            std::filesystem::remove(dir.path() / name, ec);
            if (std::filesystem::exists(saved))
                std::filesystem::rename(saved, dir.path() / name);
        };
        {
            ObjectTrie trie(dir.path(), key_of(9), options);
            std::vector<ObjectTrie::Change> first;
            for (int i = 0; i < 400; ++i) {
                first.push_back({id_of(random), value_of(random)});
                model[first.back().id] = *first.back().value;
            }
            trie.apply(first);
            trie.checkpoint();
            std::vector<ObjectTrie::Change> second;
            for (int i = 0; i < 400; ++i) {
                second.push_back({id_of(random), value_of(random)});
                model[second.back().id] = *second.back().value;
            }
            trie.apply(second);
            snapshot("root");
            snapshot("journal.log");
            trie.checkpoint();
        }
        // Undo what came after the crash point: step 0, the root and the
        // journal as before (nodes appended, nothing published); step 1, the
        // journal as before (root published, journal not emptied); step 2,
        // nothing (the whole checkpoint).
        if (step == 0) {
            restore("root");
            restore("journal.log");
        } else if (step == 1) {
            restore("journal.log");
        }
        ObjectTrie reopened(dir.path(), key_of(9), options);
        check_matches(reopened, model, random);
    }
}

MACHA_FAST_TEST("object_ledger", test_a_trie_rewrites_its_node_file_and_stays_bounded) {
    TempDir dir;
    std::mt19937_64 random(15);
    std::map<ObjectId, Bytes> model;
    std::vector<ObjectId> ids;
    for (int i = 0; i < 2000; ++i)
        ids.push_back(id_of(random));
    ObjectTrie::Options options = small_options();
    ObjectTrie trie(dir.path(), key_of(10), options);
    // The same ids rewritten many times: every checkpoint supersedes nodes.
    for (int round = 0; round < 40; ++round) {
        std::vector<ObjectTrie::Change> batch;
        for (const auto& id : ids) {
            const auto value = value_of(random);
            batch.push_back({id, value});
            model[id] = value;
        }
        trie.apply(batch);
    }
    const auto stats = trie.stats();
    CHECK(stats.rewrites > 0);
    CHECK(stats.node_file_bytes <= 2 * stats.live_bytes + options.rewrite_floor_bytes +
                                       options.checkpoint_bytes * 4);
    // Exactly one node file is left.
    size_t node_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path()))
        node_files += entry.path().filename().string().starts_with("nodes-") ? 1 : 0;
    CHECK(node_files == 1);
    check_matches(trie, model, random);
    ObjectTrie reopened(dir.path(), key_of(10), options);
    check_matches(reopened, model, random);
}

MACHA_FAST_TEST("object_ledger", test_a_trie_larger_than_its_cache_reads_what_it_needs) {
    TempDir dir;
    std::mt19937_64 random(16);
    std::map<ObjectId, Bytes> model;
    ObjectTrie::Options options = small_options();
    options.cache_bytes = 16 * 1024;
    {
        ObjectTrie trie(dir.path(), key_of(11), options);
        std::vector<ObjectTrie::Change> batch;
        for (int i = 0; i < 20000; ++i) {
            batch.push_back({id_of(random), value_of(random)});
            model[batch.back().id] = *batch.back().value;
        }
        trie.apply(batch);
        trie.checkpoint();
    }
    ObjectTrie reopened(dir.path(), key_of(11), options);
    check_matches(reopened, model, random);
    const auto stats = reopened.stats();
    CHECK(stats.loads > 0);
    // The cache keeps to its bound, give or take the node being read.
    CHECK(stats.cache_bytes <= options.cache_bytes + 64 * 1024);
}

MACHA_FAST_TEST("object_ledger", test_a_trie_with_a_torn_journal_keeps_what_was_whole) {
    TempDir dir;
    std::mt19937_64 random(17);
    std::map<ObjectId, Bytes> whole;
    ObjectTrie::Options options = small_options();
    options.checkpoint_bytes = 1ULL << 40;
    {
        ObjectTrie trie(dir.path(), key_of(12), options);
        std::vector<ObjectTrie::Change> first;
        for (int i = 0; i < 200; ++i) {
            first.push_back({id_of(random), value_of(random)});
            whole[first.back().id] = *first.back().value;
        }
        trie.apply(first);
        std::vector<ObjectTrie::Change> second;
        for (int i = 0; i < 200; ++i)
            second.push_back({id_of(random), value_of(random)});
        trie.apply(second);
    }
    // The second batch's frame is cut short: only the first survives.
    const auto journal = dir.path() / "journal.log";
    std::filesystem::resize_file(journal, std::filesystem::file_size(journal) - 3);
    ObjectTrie reopened(dir.path(), key_of(12), options);
    check_matches(reopened, whole, random);
}

// A trie written in format 1 (a u64 value a record) opens with every record,
// each value its eight big-endian bytes, and is rewritten in the current one.
MACHA_FAST_TEST("object_ledger", test_a_format_1_trie_opens_and_is_rewritten) {
    TempDir dir;
    std::mt19937_64 random(19);
    const auto key = key_of(20);
    const auto be64 = [](uint64_t value) {
        Writer writer;
        writer.u64(value);
        return writer.take();
    };
    std::map<ObjectId, uint64_t> saved;
    for (int i = 0; i < 600; ++i)
        saved[id_of(random)] = random();
    {
        // Format 1's node file: a leaf per first byte, then the interior root.
        constexpr std::array<uint8_t, 8> nodes_aad{'M', 'A', 'C', 'H', 'L', 'N', '0', '1'};
        SealedJournal nodes(dir.path() / "nodes-0.bin", key, nodes_aad, 4U << 20);
        struct Slot {
            uint64_t offset{};
            uint32_t size{};
            uint64_t count{};
            Hash256 hash{};
        };
        std::map<uint8_t, Slot> slots;
        auto at = saved.begin();
        while (at != saved.end()) {
            const auto byte = at->first.bytes[0];
            Writer leaf;
            Writer hashed;
            hashed.u8('L');
            std::vector<std::pair<ObjectId, uint64_t>> records;
            for (; at != saved.end() && at->first.bytes[0] == byte; ++at)
                records.emplace_back(at->first, at->second);
            REQUIRE(records.size() <= ObjectTrie::leaf_max);
            leaf.u8(1);
            leaf.u8(1);
            leaf.u32(static_cast<uint32_t>(records.size()));
            for (const auto& [id, value] : records) {
                leaf.fixed(id.bytes);
                leaf.u64(value);
                hashed.fixed(id.bytes);
                hashed.u64(value);
            }
            const auto before = nodes.bytes();
            const auto offset = nodes.append_unsynced(leaf.data());
            slots[byte] = {offset, static_cast<uint32_t>(nodes.bytes() - before), records.size(),
                           sha256(hashed.data())};
        }
        Writer interior;
        Writer hashed;
        hashed.u8('I');
        interior.u8(2);
        interior.u8(0);
        std::array<uint8_t, 32> present{};
        for (const auto& [byte, slot] : slots)
            present[byte / 8] |= static_cast<uint8_t>(1U << (byte % 8));
        interior.fixed(present);
        for (const auto& [byte, slot] : slots) {
            interior.u64(slot.offset);
            interior.u32(slot.size);
            interior.u64(slot.count);
            interior.fixed(slot.hash.bytes);
            hashed.u8(byte);
            hashed.fixed(slot.hash.bytes);
            hashed.u64(slot.count);
        }
        const auto before = nodes.bytes();
        const auto offset = nodes.append_unsynced(interior.data());
        const auto size = static_cast<uint32_t>(nodes.bytes() - before);
        nodes.sync();

        Writer root;
        root.fixed(std::array<uint8_t, 8>{'M', 'L', 'T', 'R', '0', '0', '0', '1'});
        root.u64(0);
        root.u64(offset);
        root.u32(size);
        root.u64(saved.size());
        root.fixed(sha256(hashed.data()).bytes);
        root.u64(nodes.bytes());
        const auto check = sha256(root.data());
        root.fixed(check.bytes);
        std::ofstream(dir.path() / "root", std::ios::binary)
            .write(reinterpret_cast<const char*>(root.data().data()),
                   static_cast<std::streamsize>(root.data().size()));

        // Format 1's journal: one record changed, one erased, one added.
        auto changed = saved.begin();
        auto erased = std::next(changed);
        const auto added = id_of(random);
        std::map<ObjectId, std::optional<uint64_t>> frame{
            {changed->first, 77}, {erased->first, std::nullopt}, {added, 88}};
        changed->second = 77;
        saved.erase(erased);
        saved[added] = 88;
        constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'L', 'J', '0', '1'};
        SealedJournal journal(dir.path() / "journal.log", key, journal_aad, 4U << 20);
        Writer writer;
        writer.u8(1);
        writer.u32(static_cast<uint32_t>(frame.size()));
        for (const auto& [id, value] : frame) {
            writer.fixed(id.bytes);
            writer.u8(value ? 1 : 0);
            if (value)
                writer.u64(*value);
        }
        journal.append(writer.data());
    }
    std::map<ObjectId, Bytes> model;
    for (const auto& [id, value] : saved)
        model[id] = be64(value);
    {
        ObjectTrie trie(dir.path(), key, small_options());
        check_matches(trie, model, random);
        CHECK(trie.stats().journal_bytes == 0);
    }
    std::ifstream root(dir.path() / "root", std::ios::binary);
    std::array<char, 8> magic{};
    root.read(magic.data(), 8);
    CHECK(std::string(magic.data(), 8) == "MLTR0002");
    ObjectTrie reopened(dir.path(), key, small_options());
    check_matches(reopened, model, random);
}

// A snapshot keeps the records it was taken with, read from several threads,
// while the trie changes, checkpoints and rewrites its node file under it.
MACHA_FAST_TEST("object_ledger", test_a_snapshot_is_frozen_through_writes_and_rewrites) {
    TempDir dir;
    std::mt19937_64 random(22);
    ObjectTrie trie(dir.path(), key_of(18), small_options());
    std::map<ObjectId, Bytes> model;
    std::vector<ObjectId> ids;
    std::vector<ObjectTrie::Change> first;
    for (int i = 0; i < 3000; ++i) {
        ids.push_back(id_of(random));
        first.push_back({ids.back(), value_of(random)});
        model[ids.back()] = *first.back().value;
    }
    trie.apply(first);
    trie.checkpoint();
    // Some unsaved changes too, held in memory by the snapshot.
    std::vector<ObjectTrie::Change> unsaved;
    for (int i = 0; i < 50; ++i) {
        unsaved.push_back({ids[static_cast<size_t>(i)], std::nullopt});
        model.erase(ids[static_cast<size_t>(i)]);
    }
    trie.apply(unsaved);
    const auto frozen = trie.snapshot();
    const auto hash = trie.root_hash();

    const auto read_all = [&](const ObjectTrie::Snapshot& snapshot) {
        std::map<ObjectId, Bytes> out;
        std::optional<ObjectId> after;
        while (true) {
            const auto page = snapshot.next(after, 101);
            for (const auto& [id, value] : page)
                out.emplace(id, value);
            if (page.size() < 101)
                return out;
            after = page.back().first;
        }
    };
    std::atomic_bool done{false};
    std::atomic_int mismatches{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
        readers.emplace_back([&, r] {
            std::mt19937_64 pick(static_cast<uint64_t>(100 + r));
            while (!done.load()) {
                const auto& id = ids[pick() % ids.size()];
                const auto found = model.find(id);
                const auto got = frozen.get(id);
                if (found == model.end() ? got.has_value() : got != found->second)
                    ++mismatches;
            }
        });
    // Rewrite the same ids many times: checkpoints and node-file rewrites.
    for (int round = 0; round < 30; ++round) {
        std::vector<ObjectTrie::Change> batch;
        for (const auto& id : ids)
            batch.push_back({id, value_of(random)});
        trie.apply(batch);
    }
    done = true;
    for (auto& reader : readers)
        reader.join();
    CHECK(mismatches.load() == 0);
    CHECK(trie.stats().rewrites > 0);
    CHECK(frozen.size() == model.size());
    CHECK(frozen.root_hash() == hash);
    CHECK(read_all(frozen) == model);
    CHECK(trie.root_hash() != hash);
}

// Stage 1's measurement: a synthetic class of MACHA_LEDGER_MEASURE_RECORDS
// records (200,000 by default; fi-1 runs ten million), then a reopen. Opening
// must be inside the 30 s bound whatever the size; the figures are logged.
MACHA_HEAVY_TEST("object_ledger", test_a_large_trie_opens_within_the_bound) {
    uint64_t records = 200000;
    if (const char* text = std::getenv("MACHA_LEDGER_MEASURE_RECORDS"))
        records = std::strtoull(text, nullptr, 10);
    TempDir dir;
    std::mt19937_64 random(18);
    ObjectTrie::Options options; // the defaults: 64 MiB cache, 1 MiB journal
    std::vector<ObjectId> sample;
    const auto started = Clock::now();
    {
        ObjectTrie trie(dir.path(), key_of(13), options);
        std::vector<ObjectTrie::Change> batch;
        batch.reserve(10000);
        for (uint64_t i = 0; i < records; ++i) {
            batch.push_back({id_of(random), Bytes(8, static_cast<uint8_t>(i))});
            if (sample.size() < 2000 && i % 97 == 0)
                sample.push_back(batch.back().id);
            if (batch.size() == 10000) {
                trie.apply(batch);
                batch.clear();
            }
        }
        trie.apply(batch);
        trie.checkpoint();
        // Leave a journal behind, as a crash would, just under the size at
        // which it would be checkpointed: the most an open replays.
        std::vector<ObjectTrie::Change> tail;
        for (int i = 0; i < 22000; ++i)
            tail.push_back({id_of(random), Bytes(8, 1)});
        trie.apply(tail);
        REQUIRE(trie.stats().journal_bytes > 900 * 1024);
    }
    const auto built_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

    const auto open_started = Clock::now();
    ObjectTrie trie(dir.path(), key_of(13), options);
    const auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - open_started).count();

    const auto cold_started = Clock::now();
    for (const auto& id : sample)
        CHECK(trie.get(id).has_value());
    const auto cold_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - cold_started).count();
    const auto warm_started = Clock::now();
    for (const auto& id : sample)
        CHECK(trie.get(id).has_value());
    const auto warm_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - warm_started).count();
    const auto stats = trie.stats();
    Log::info("ledger measure records=" + std::to_string(trie.size()) +
              " built_ms=" + std::to_string(built_ms) + " open_ms=" + std::to_string(open_ms) +
              " cold_lookup_us=" + std::to_string(cold_us / static_cast<long long>(sample.size())) +
              " warm_lookup_us=" + std::to_string(warm_us / static_cast<long long>(sample.size())) +
              " node_file_bytes=" + std::to_string(stats.node_file_bytes) +
              " live_bytes=" + std::to_string(stats.live_bytes) +
              " cache_bytes=" + std::to_string(stats.cache_bytes) +
              " loads=" + std::to_string(stats.loads));
    CHECK(open_ms < 30000);
    CHECK(stats.cache_bytes <= options.cache_bytes + 64 * 1024);
}

// --- Stage 2: a store's held ledger --------------------------------------------

LocalStoreOptions ledgered(const std::filesystem::path& ledger) {
    LocalStoreOptions options;
    options.limit = 64ULL * 1024 * 1024;
    options.ledger_dir = ledger;
    options.ledger_cache_bytes = 1024 * 1024;
    return options;
}

MACHA_FAST_TEST("object_ledger", test_a_store_with_its_ledger_starts_knowing_what_it_holds) {
    TempDir dir;
    const auto root = dir.path() / "store";
    const auto ledger = dir.path() / "ledger";
    const auto key = key_of(14);
    std::vector<ObjectId> kept;
    std::vector<ObjectId> removed;
    {
        LocalStore store(root, ledgered(ledger), key);
        // The first start walks its objects once and seeds the ledger.
        REQUIRE(wait_until([&] { return std::filesystem::exists(ledger / "seeded"); }, 10s));
        for (int i = 0; i < 40; ++i) {
            const auto data = pattern(4096 + i, static_cast<uint8_t>(i));
            const auto id = object_id(data);
            REQUIRE(store.put(id, data));
            (i % 4 == 0 ? removed : kept).push_back(id);
        }
        for (const auto& id : removed)
            REQUIRE(store.remove(id));
    }
    // A file the ledger never heard of, as a crash between a file and its
    // record would leave: written by a store keeping no ledger.
    const auto unlisted_data = pattern(7777, 99);
    const auto unlisted = object_id(unlisted_data);
    {
        LocalStore bare(root, 64ULL * 1024 * 1024, key);
        REQUIRE(bare.put(unlisted, unlisted_data));
    }
    {
        LocalStore store(root, ledgered(ledger), key);
        // Known at once: no walk.
        CHECK(store.held_view().identity.complete);
        for (const auto& id : kept)
            CHECK(store.has(id));
        for (const auto& id : removed)
            CHECK(!store.has(id));
        // The ledger is the answer, so the unlisted file is not seen, which
        // is also how this test knows nothing walked the store...
        CHECK(!store.has(unlisted));
        // ...until a read finds it, which records it.
        CHECK(store.get(unlisted) == std::optional<Bytes>(unlisted_data));
        CHECK(store.has(unlisted));
    }
    LocalStore again(root, ledgered(ledger), key);
    CHECK(again.held_view().identity.complete);
    CHECK(again.has(unlisted));
    for (const auto& id : kept)
        CHECK(again.has(id));
}

// The verification pass corrects the ledger both ways: a file it does not list
// is recorded held, and a listed object whose file is gone is recorded lost.
MACHA_FAST_TEST("object_ledger", test_verification_corrects_the_ledger_from_the_disk) {
    TempDir dir;
    const auto root = dir.path() / "store";
    const auto ledger = dir.path() / "ledger";
    const auto key = key_of(15);
    std::vector<ObjectId> held;
    {
        LocalStore store(root, ledgered(ledger), key);
        REQUIRE(wait_until([&] { return std::filesystem::exists(ledger / "seeded"); }, 10s));
        for (int i = 0; i < 30; ++i) {
            const auto data = pattern(4096 + i, static_cast<uint8_t>(i));
            held.push_back(object_id(data));
            REQUIRE(store.put(held.back(), data));
        }
    }
    const auto unlisted_data = pattern(5555, 77);
    const auto unlisted = object_id(unlisted_data);
    {
        LocalStore bare(root, 64ULL * 1024 * 1024, key);
        REQUIRE(bare.put(unlisted, unlisted_data));
    }
    LocalStore store(root, ledgered(ledger), key);
    const auto gone = held.front();
    REQUIRE(std::filesystem::remove(store.object_path(gone)));
    CHECK(store.has(gone));
    CHECK(!store.has(unlisted));
    const auto losses = store.losses();

    // Slice by slice until a pass completes.
    LocalStore::VerifyResult total;
    size_t steps = 0;
    while (!total.complete) {
        const auto step = store.verify_step(4096);
        total.recorded += step.recorded;
        total.lost += step.lost;
        total.complete = step.complete;
        REQUIRE(++steps <= 16);
    }
    CHECK(steps == 16);
    CHECK(total.recorded == 1);
    CHECK(total.lost == 1);
    CHECK(store.has(unlisted));
    CHECK(!store.has(gone));
    CHECK(store.losses() == losses + 1);
    for (size_t i = 1; i < held.size(); ++i)
        CHECK(store.has(held[i]));
    // Nothing left to correct.
    const auto again = store.verify_step(65536);
    CHECK(again.complete);
    CHECK(again.recorded == 0);
    CHECK(again.lost == 0);
}

// Packed objects are held like any other: listed by every put and removal,
// by the next open when a store keeping no ledger packed them, and never
// recorded lost by verification for having no file of their own.
MACHA_FAST_TEST("object_ledger", test_the_ledger_lists_what_the_packs_hold) {
    TempDir dir;
    const auto root = dir.path() / "store";
    const auto ledger = dir.path() / "ledger";
    const auto key = key_of(16);
    auto packing = ledgered(ledger);
    packing.pack_threshold = 64 * 1024;
    packing.pack_target_size = 1024 * 1024;
    const auto small = [](int i) { return pattern(1000 + i, static_cast<uint8_t>(i)); };
    const auto kept = object_id(small(1));
    const auto dropped = object_id(small(2));
    {
        LocalStore store(root, packing, key);
        REQUIRE(wait_until([&] { return store.held_view().identity.complete; }, 10s));
        REQUIRE(store.put(kept, small(1)));
        REQUIRE(store.put(dropped, small(2)));
        REQUIRE(store.object_path(kept).parent_path() == root / "packs");
        const auto view = store.held_view();
        CHECK(view.held(kept));
        CHECK(view.held(dropped));
        REQUIRE(store.remove(dropped));
        CHECK(!store.held_view().held(dropped));
    }
    const auto unlisted = object_id(small(3));
    {
        auto bare = packing;
        bare.ledger_dir.clear();
        LocalStore store(root, bare, key);
        REQUIRE(store.put(unlisted, small(3)));
    }
    LocalStore store(root, packing, key);
    const auto view = store.held_view();
    CHECK(view.identity.complete);
    CHECK(view.held(kept));
    CHECK(view.held(unlisted));
    CHECK(!view.held(dropped));

    LocalStore::VerifyResult total;
    while (!total.complete) {
        const auto step = store.verify_step(65536);
        total.lost += step.lost;
        total.complete = step.complete;
    }
    CHECK(total.lost == 0);
    CHECK(store.held_view().identity == view.identity);
}

// A write's flush does not wait for a seed being built, and what it queued
// after the snapshot is applied over the seed.
MACHA_FAST_TEST("object_ledger", test_a_flush_does_not_wait_for_the_seed) {
    TempDir dir;
    HeldLedger ledger(dir.path(), key_of(16), small_options());
    std::mt19937_64 random(21);
    const auto seeded_id = id_of(random);
    const auto later_id = id_of(random);
    std::promise<void> in_snapshot;
    std::promise<void> release;
    auto released = release.get_future().share();
    auto seeding = std::async(std::launch::async, [&] {
        ledger.seed([&] {
            ledger.drop_queued();
            in_snapshot.set_value();
            // The slow part of a seed, held open.
            released.wait();
            return std::vector<ObjectId>{seeded_id};
        });
    });
    in_snapshot.get_future().wait();
    // While the seed is held open: a write records and flushes, and returns.
    auto writing = std::async(std::launch::async, [&] {
        ledger.record(later_id, true);
        ledger.flush();
    });
    const bool flushed = writing.wait_for(5s) == std::future_status::ready;
    CHECK(flushed);
    CHECK(!ledger.seeded());
    release.set_value();
    seeding.get();
    if (!flushed)
        writing.get();
    // Queued after the snapshot: applied over the seed by the seed itself.
    CHECK(ledger.held(seeded_id) == std::optional<bool>(true));
    CHECK(ledger.held(later_id) == std::optional<bool>(true));
    CHECK(ledger.size() == 2);
}

// A restarted store resumes its check where it stopped.
MACHA_FAST_TEST("object_ledger", test_verification_resumes_after_a_restart) {
    TempDir dir;
    const auto root = dir.path() / "store";
    const auto ledger = dir.path() / "ledger";
    const auto key = key_of(17);
    {
        LocalStore store(root, ledgered(ledger), key);
        REQUIRE(wait_until([&] { return std::filesystem::exists(ledger / "seeded"); }, 10s));
        CHECK(store.verify_step(100).directories == 100);
    }
    LocalStore store(root, ledgered(ledger), key);
    const auto rest = store.verify_step(65536);
    CHECK(rest.complete);
    CHECK(rest.directories == 65536 - 100);
}

// The namespace's reference counts: counted per reference, kept across a
// reopen at their root, a bad change refused whole, an interrupted one
// leaving no root (the next count walks).
MACHA_FAST_TEST("object_ledger", test_reference_counts_follow_and_survive_a_restart) {
    TempDir dir;
    std::mt19937_64 random(23);
    const auto key = key_of(19);
    const auto a = id_of(random);
    const auto b = id_of(random);
    const auto node = id_of(random);
    const auto root1 = id_of(random);
    const auto root2 = id_of(random);
    const auto count_of = [](const ObjectTrie::Snapshot& view, const ObjectId& id) -> uint64_t {
        const auto value = view.get(id);
        if (!value)
            return 0;
        Reader reader(*value);
        return reader.u64();
    };
    {
        ReferenceCounts counts(dir.path() / "referenced", key, 1 << 20);
        CHECK(!counts.root());
        // `a` is shared by two files.
        counts.reset(root1, {a, b, a}, {node}, {3, 3});
        const auto views = counts.views();
        CHECK(count_of(views.extents, a) == 2);
        CHECK(count_of(views.extents, b) == 1);
        CHECK(views.nodes.size() == 1);
        // One reference to `a` goes, `b` goes: `a` stays, `b` is gone.
        CHECK(counts.change(root2, {}, {a, b}, {}, {}, {1, 1}));
        // The earlier view is frozen.
        CHECK(count_of(views.extents, b) == 1);
        // Taking out what is not counted is refused, changing nothing.
        CHECK(!counts.change(root1, {}, {b}, {}, {}, {0, 0}));
        CHECK(counts.root() == std::optional(root2));
    }
    {
        ReferenceCounts counts(dir.path() / "referenced", key, 1 << 20);
        CHECK(counts.root() == std::optional(root2));
        CHECK(counts.totals().entries == 1);
        const auto views = counts.views();
        CHECK(count_of(views.extents, a) == 1);
        CHECK(!views.extents.get(b));
        CHECK(views.nodes.get(node).has_value());
    }
    // A change interrupted between its first state write and its last.
    {
        std::ofstream state(dir.path() / "referenced" / "state",
                            std::ios::binary | std::ios::in | std::ios::out);
        state.seekp(8);
        state.put(0);
    }
    ReferenceCounts interrupted(dir.path() / "referenced", key, 1 << 20);
    CHECK(!interrupted.root());
}

} // namespace
