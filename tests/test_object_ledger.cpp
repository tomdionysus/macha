// SPDX-License-Identifier: GPL-3.0-or-later
// The object ledger's primitives: the sealed journal it shares with the
// retention store, and the per-class trie.
#include "ledger/object_trie.hpp"
#include "storage/local_store.hpp"
#include "storage/sealed_journal.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <string>
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

ObjectTrie::Options small_options() {
    ObjectTrie::Options options;
    options.cache_bytes = 64 * 1024;
    options.checkpoint_bytes = 64 * 1024;
    options.rewrite_floor_bytes = 32 * 1024;
    return options;
}

// Everything the trie holds, read back in order through next().
std::map<ObjectId, uint64_t> contents(const ObjectTrie& trie) {
    std::map<ObjectId, uint64_t> out;
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

void check_matches(const ObjectTrie& trie, const std::map<ObjectId, uint64_t>& model,
                   std::mt19937_64& random) {
    CHECK(trie.size() == model.size());
    CHECK(contents(trie) == model);
    for (const auto& [id, value] : model)
        CHECK(trie.get(id) == std::optional<uint64_t>(value));
    for (int i = 0; i < 50; ++i)
        CHECK(!trie.get(id_of(random)).has_value());
}

MACHA_FAST_TEST("object_ledger", test_the_trie_holds_exactly_what_was_written) {
    TempDir dir;
    std::mt19937_64 random(11);
    std::map<ObjectId, uint64_t> model;
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
                batch.push_back({id, random()});
            } else if (roll < 8) {
                batch.push_back({known[random() % known.size()], random()});
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
        records.push_back({id_of(random), random()});
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
        extra.push_back({id, random()});
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
    two.apply(std::vector<ObjectTrie::Change>{{records[0].id, *records[0].value + 1}});
    CHECK(one.root_hash() != two.root_hash());
}

MACHA_FAST_TEST("object_ledger", test_a_reopened_trie_is_the_one_that_was_closed) {
    TempDir dir;
    std::mt19937_64 random(13);
    std::map<ObjectId, uint64_t> model;
    Hash256 hash{};
    {
        ObjectTrie trie(dir.path(), key_of(8), small_options());
        for (int round = 0; round < 20; ++round) {
            std::vector<ObjectTrie::Change> batch;
            for (int i = 0; i < 300; ++i) {
                const auto id = id_of(random);
                const auto value = random();
                batch.push_back({id, value});
                model[id] = value;
            }
            trie.apply(batch);
        }
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
        std::map<ObjectId, uint64_t> model;
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
                first.push_back({id_of(random), random()});
                model[first.back().id] = *first.back().value;
            }
            trie.apply(first);
            trie.checkpoint();
            std::vector<ObjectTrie::Change> second;
            for (int i = 0; i < 400; ++i) {
                second.push_back({id_of(random), random()});
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
    std::map<ObjectId, uint64_t> model;
    std::vector<ObjectId> ids;
    for (int i = 0; i < 2000; ++i)
        ids.push_back(id_of(random));
    ObjectTrie::Options options = small_options();
    ObjectTrie trie(dir.path(), key_of(10), options);
    // The same ids rewritten many times: every checkpoint supersedes nodes.
    for (int round = 0; round < 40; ++round) {
        std::vector<ObjectTrie::Change> batch;
        for (const auto& id : ids) {
            const auto value = random();
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
    std::map<ObjectId, uint64_t> model;
    ObjectTrie::Options options = small_options();
    options.cache_bytes = 16 * 1024;
    {
        ObjectTrie trie(dir.path(), key_of(11), options);
        std::vector<ObjectTrie::Change> batch;
        for (int i = 0; i < 20000; ++i) {
            batch.push_back({id_of(random), random()});
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
    std::map<ObjectId, uint64_t> whole;
    ObjectTrie::Options options = small_options();
    options.checkpoint_bytes = 1ULL << 40;
    {
        ObjectTrie trie(dir.path(), key_of(12), options);
        std::vector<ObjectTrie::Change> first;
        for (int i = 0; i < 200; ++i) {
            first.push_back({id_of(random), random()});
            whole[first.back().id] = *first.back().value;
        }
        trie.apply(first);
        std::vector<ObjectTrie::Change> second;
        for (int i = 0; i < 200; ++i)
            second.push_back({id_of(random), random()});
        trie.apply(second);
    }
    // The second batch's frame is cut short: only the first survives.
    const auto journal = dir.path() / "journal.log";
    std::filesystem::resize_file(journal, std::filesystem::file_size(journal) - 3);
    ObjectTrie reopened(dir.path(), key_of(12), options);
    check_matches(reopened, whole, random);
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
            batch.push_back({id_of(random), i});
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
        for (int i = 0; i < 24000; ++i)
            tail.push_back({id_of(random), 1});
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
        CHECK(store.indexed());
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
    CHECK(again.indexed());
    CHECK(again.has(unlisted));
    for (const auto& id : kept)
        CHECK(again.has(id));
}

} // namespace
