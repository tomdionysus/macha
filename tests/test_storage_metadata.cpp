// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "codec.hpp"
#include "crypto.hpp"
#include "storage/sealed_journal.hpp"

#include <cstdio>
#include <fstream>
#include <thread>
#include <future>
#include <optional>
#include <string_view>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

// One mutation by an author on a head, stamped as a commit stamps what it
// writes.
struct Mutation {
    MetadataSnapshot& head;
    MetadataDot dot;

    Mutation(MetadataSnapshot& snapshot, uint8_t author) : head(snapshot) {
        if (!head.legacy_clock)
            head.legacy_clock = head.mutation_sequences;
        dot.author.bytes[0] = author;
        dot.sequence = ++head.mutation_sequences[dot.author];
    }
    void put(const std::string& path, FsEntry entry) {
        const auto prior = head.entries.find(path);
        entry.provenance = {};
        stamp_entry_provenance(entry, path, prior == head.entries.end() ? nullptr : &prior->second,
                               dot);
        head.entries[path] = std::move(entry);
    }
    void erase(const std::string& path) { head.entries.erase(path); }
    void catalogue(const ObjectId& root) {
        head.catalogue_root = root;
        head.catalogue_dot = dot;
    }
};

// Flips the last byte of the history frame ending at `frame_end`: the frame
// stays indexed but no longer authenticates, so it cannot be replayed until it
// is flipped back.
void flip_history_frame(const std::filesystem::path& history, uint64_t frame_end) {
    std::fstream file(history, std::ios::binary | std::ios::in | std::ios::out);
    char byte{};
    file.seekg(static_cast<std::streamoff>(frame_end - 1));
    file.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x5a);
    file.seekp(static_cast<std::streamoff>(frame_end - 1));
    file.write(&byte, 1);
    file.flush();
    if (!file)
        throw std::runtime_error("cannot flip history frame byte in " + history.string());
}

std::optional<uint64_t> process_rss_kib() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view prefix = "VmRSS:";
        if (!line.starts_with(prefix))
            continue;
        const auto first = line.find_first_of("0123456789", prefix.size());
        if (first == std::string::npos)
            return {};
        return std::stoull(line.substr(first));
    }
#endif
    return {};
}

MACHA_FAST_TEST("storage_metadata", test_retention_claims_are_causal_durable_and_observed_remove) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "retention-state";

    const auto origin_a = random_node_id();
    const auto origin_b = random_node_id();
    const auto object = object_id(pattern(4097));
    const auto second = object_id(pattern(8193));

    {
        RetentionStore retention(state, keys.storage);
        retention.retain(RetentionClass::data, object, {origin_a, 1});
        CHECK(retention.retained(RetentionClass::data, object));

        // A removal clears only claims causally visible in its mutation clock, so B's
        // concurrent claim survives an A-only delete context.
        retention.retain(RetentionClass::data, object, {origin_b, 1});
        std::vector<ObjectId> no_live;
        CHECK(retention.release_unreferenced(RetentionClass::data, no_live,
                                             RetentionClock{{origin_a, 1}}, 16) == 1);
        CHECK(retention.retained(RetentionClass::data, object));

        // Once a reconciled view has seen both branch dots, the unreferenced object
        // may lose both claims.
        CHECK(retention.release_unreferenced(RetentionClass::data, no_live,
                                             RetentionClock{{origin_a, 1}, {origin_b, 1}},
                                             16) == 1);
        CHECK(!retention.retained(RetentionClass::data, object));

        // Removed clocks suppress delayed/replayed old ADDs but do not suppress
        // a genuinely later mutation from the same origin.
        retention.retain(RetentionClass::data, object, {origin_a, 1});
        CHECK(!retention.retained(RetentionClass::data, object));
        retention.retain(RetentionClass::data, object, {origin_a, 2});
        CHECK(retention.retained(RetentionClass::data, object));

        retention.retain(RetentionClass::control, second, {origin_b, 7});
        CHECK(retention.retained(RetentionClass::control, second));
        // Compaction preserves the causal state while bounding the journal.
        CHECK(retention.compact_if_needed(1));
        CHECK(!retention.compact_if_needed(1));
    }

    // Claims and remove contexts survive a restart: the later DATA and CONTROL
    // claims, and the tombstone that suppresses a delayed A:1 replay.
    {
        RetentionStore reopened(state, keys.storage);
        CHECK(reopened.retained(RetentionClass::data, object));
        CHECK(reopened.retained(RetentionClass::control, second));
        reopened.retain(RetentionClass::data, object, {origin_a, 1});
        CHECK(reopened.retained(RetentionClass::data, object));
        CHECK(reopened.release_unreferenced(RetentionClass::data, {}, RetentionClock{{origin_a, 2}},
                                            16) == 1);
        CHECK(!reopened.retained(RetentionClass::data, object));
    }
}

MACHA_FAST_TEST("storage_metadata", test_retention_prune_cursor_cannot_starve_later_tombstones) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    RetentionStore retention(t.path() / "state", keys.storage);
    const auto origin = random_node_id();

    std::array<ObjectId, 5> objects{};
    for (size_t i = 0; i < objects.size(); ++i)
        objects[i].bytes.back() = static_cast<uint8_t>(i + 1);

    for (const auto& id : objects)
        retention.retain(RetentionClass::data, id, {origin, 1});
    CHECK(retention.release_unreferenced(RetentionClass::data, {}, RetentionClock{{origin, 1}},
                                         32) == objects.size());

    // The first two tombstones are still backed by objects; a fixed budget must
    // still progress to the later dead rows rather than restart at the beginning.
    auto exists = [&](const ObjectId& id) { return id == objects[0] || id == objects[1]; };
    CHECK(retention.prune_unclaimed(RetentionClass::data, exists, 2) == 0);
    CHECK(retention.prune_unclaimed(RetentionClass::data, exists, 2) == 2);

    // A pruned tombstone no longer suppresses an ancient replay, showing the row
    // was erased.
    retention.retain(RetentionClass::data, objects[2], {origin, 1});
    CHECK(retention.retained(RetentionClass::data, objects[2]));

    // The protected leading row was examined but retained, so its remove clock
    // must still suppress the same old dot.
    retention.retain(RetentionClass::data, objects[0], {origin, 1});
    CHECK(!retention.retained(RetentionClass::data, objects[0]));
}

// The presence check runs without the store's lock, so a claim made while it
// runs is seen, and the row it revived is kept.
MACHA_FAST_TEST("storage_metadata", test_retention_prune_keeps_a_row_claimed_while_it_checks_presence) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    RetentionStore retention(t.path() / "state", keys.storage);
    const auto origin = random_node_id();

    std::array<ObjectId, 2> objects{};
    for (size_t i = 0; i < objects.size(); ++i)
        objects[i].bytes.back() = static_cast<uint8_t>(i + 1);
    for (const auto& id : objects)
        retention.retain(RetentionClass::data, id, {origin, 1});
    CHECK(retention.release_unreferenced(RetentionClass::data, {}, RetentionClock{{origin, 1}},
                                         32) == objects.size());

    auto exists = [&](const ObjectId& id) {
        // Reads and writes the store from inside the check.
        CHECK(!retention.retained(RetentionClass::data, id));
        if (id == objects[0])
            retention.retain(RetentionClass::data, id, {origin, 2});
        return false;
    };
    CHECK(retention.prune_unclaimed(RetentionClass::data, exists, 8) == 1);
    CHECK(retention.retained(RetentionClass::data, objects[0]));
    // objects[1]'s row is gone: its old dot is no longer suppressed.
    retention.retain(RetentionClass::data, objects[1], {origin, 1});
    CHECK(retention.retained(RetentionClass::data, objects[1]));
}

MACHA_FAST_TEST("storage_metadata", test_retention_claims_checkpoint_into_the_ledger_and_restart) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto origin = random_node_id();
    std::vector<ObjectId> objects;
    for (uint16_t shard = 0; shard < 256; ++shard) {
        for (uint16_t suffix = 0; suffix < 2; ++suffix) {
            ObjectId id{};
            id.bytes[0] = static_cast<uint8_t>(shard);
            id.bytes[30] = static_cast<uint8_t>(suffix);
            id.bytes[31] = static_cast<uint8_t>(255 - shard);
            objects.push_back(id);
        }
    }
    ObjectId later{};
    later.bytes[5] = 9;
    {
        RetentionStore retention(state, keys.storage);
        retention.retain_batch(RetentionClass::data, objects, {origin, 7});
        REQUIRE(retention.compact_if_needed(1));
        CHECK(std::filesystem::file_size(state / "retention" / "claims.log") == 0);
        // After the checkpoint: in the journal only.
        retention.retain(RetentionClass::control, later, {origin, 8});
    }
    CHECK(std::filesystem::exists(state / "retention" / "ledger-data" / "root"));
    CHECK(std::filesystem::file_size(state / "retention" / "claims.log") > 0);
    RetentionStore reopened(state, keys.storage);
    for (const auto& id : objects)
        CHECK(reopened.retained(RetentionClass::data, id));
    CHECK(reopened.retained(RetentionClass::control, later));
    CHECK(!reopened.retained(RetentionClass::data, later));
    CHECK(reopened.claim_objects(RetentionClass::data) == objects.size());
    CHECK(reopened.claim_objects(RetentionClass::control) == 1);
}

// Enough changed objects checkpoint the tries on their own: what the tries
// hold unsaved stays bounded whatever the batch.
MACHA_FAST_TEST("storage_metadata", test_retention_checkpoints_after_enough_changes) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto origin = random_node_id();
    std::vector<ObjectId> objects(10000);
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i].bytes[0] = static_cast<uint8_t>(i);
        objects[i].bytes[1] = static_cast<uint8_t>(i >> 8);
    }
    RetentionStore retention(state, keys.storage);
    retention.retain_batch(RetentionClass::data, objects, {origin, 1});
    CHECK(std::filesystem::file_size(state / "retention" / "claims.log") == 0);
    CHECK(retention.claim_objects(RetentionClass::data) == objects.size());
}

// A start that replays a long journal checkpoints it, so the next start does
// not replay it again.
MACHA_FAST_TEST("storage_metadata", test_retention_start_checkpoints_a_long_journal) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto root = state / "retention";
    const auto origin = random_node_id();
    std::vector<ObjectId> objects(60000);
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i].bytes[0] = static_cast<uint8_t>(i);
        objects[i].bytes[1] = static_cast<uint8_t>(i >> 8);
        objects[i].bytes[2] = static_cast<uint8_t>(i >> 16);
    }
    std::sort(objects.begin(), objects.end());
    { RetentionStore created(state, keys.storage); }
    {
        // As a crash leaves it: frames journaled, no checkpoint.
        constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'R', 'T', 'J', '1'};
        SealedJournal journal(root / "claims.log", keys.storage, journal_aad, 4U << 20);
        for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
            Writer frame;
            frame.u8(1);
            frame.u8(static_cast<uint8_t>(RetentionClass::data));
            frame.fixed(origin.bytes);
            frame.u64(sequence);
            frame.u32(static_cast<uint32_t>(objects.size()));
            for (const auto& id : objects)
                frame.fixed(id.bytes);
            journal.append(frame.data());
        }
    }
    REQUIRE(std::filesystem::file_size(root / "claims.log") > 4ULL * 1024 * 1024);
    {
        RetentionStore reopened(state, keys.storage);
        CHECK(std::filesystem::file_size(root / "claims.log") == 0);
        CHECK(reopened.claims(RetentionClass::data, objects[7]).adds.at(origin) == 3);
    }
    RetentionStore again(state, keys.storage);
    CHECK(again.claim_objects(RetentionClass::data) == objects.size());
}

// A prune is journaled: the erased row stays erased across a restart with no
// checkpoint between, so an old dot is no longer suppressed.
MACHA_FAST_TEST("storage_metadata", test_retention_prune_survives_a_restart) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto origin = random_node_id();
    ObjectId pruned{};
    pruned.bytes[0] = 1;
    ObjectId kept{};
    kept.bytes[0] = 2;
    {
        RetentionStore retention(state, keys.storage);
        retention.retain(RetentionClass::data, pruned, {origin, 1});
        retention.retain(RetentionClass::data, kept, {origin, 1});
        CHECK(retention.release_unreferenced(RetentionClass::data, {},
                                             RetentionClock{{origin, 1}}, 8) == 2);
        CHECK(retention.prune_unclaimed(
                  RetentionClass::data, [&](const ObjectId& id) { return id == kept; }, 8) == 1);
    }
    RetentionStore reopened(state, keys.storage);
    CHECK(reopened.claims(RetentionClass::data, pruned).removed.empty());
    CHECK(reopened.claims(RetentionClass::data, kept).removed.size() == 1);
    reopened.retain(RetentionClass::data, pruned, {origin, 1});
    CHECK(reopened.retained(RetentionClass::data, pruned));
    reopened.retain(RetentionClass::data, kept, {origin, 1});
    CHECK(!reopened.retained(RetentionClass::data, kept));
}

// A node's sharded checkpoint and the journal after it move into the ledger at
// the first start, claims and tombstones alike; the first checkpoint removes
// the shard files.
MACHA_FAST_TEST("storage_metadata", test_retention_shard_checkpoint_moves_into_the_ledger) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto root = state / "retention";
    const auto a = random_node_id();
    const auto b = random_node_id();
    std::vector<ObjectId> data_ids;
    for (int i = 0; i < 600; ++i) {
        ObjectId id{};
        id.bytes[0] = static_cast<uint8_t>(i * 7);
        id.bytes[1] = static_cast<uint8_t>(i);
        id.bytes[2] = static_cast<uint8_t>(i >> 8);
        data_ids.push_back(id);
    }
    std::sort(data_ids.begin(), data_ids.end());
    // data_ids[0] carries a tombstone from b only; the rest a claim from a.
    ObjectId control_id{};
    control_id.bytes[0] = 200;
    control_id.bytes[9] = 1;
    {
        const auto generation = root / "checkpoints" / "gen-1-1-1";
        std::filesystem::create_directories(generation);
        constexpr std::array<uint8_t, 8> aad{'M', 'A', 'C', 'H', 'R', 'T', 'S', '1'};
        for (int shard = 0; shard < 256; ++shard) {
            Writer plain;
            plain.u8(2);
            plain.u8(static_cast<uint8_t>(shard));
            std::vector<ObjectId> in_shard;
            for (const auto& id : data_ids)
                if (id.bytes[0] == shard)
                    in_shard.push_back(id);
            plain.u32(static_cast<uint32_t>(in_shard.size()));
            for (const auto& id : in_shard) {
                plain.fixed(id.bytes);
                if (id == data_ids[0]) {
                    plain.u32(0);
                    plain.u32(1);
                    plain.fixed(b.bytes);
                    plain.u64(5);
                } else {
                    plain.u32(1);
                    plain.fixed(a.bytes);
                    plain.u64(3);
                    plain.u32(0);
                }
            }
            const bool control_here = shard == control_id.bytes[0];
            plain.u32(control_here ? 1 : 0);
            if (control_here) {
                plain.fixed(control_id.bytes);
                plain.u32(1);
                plain.fixed(a.bytes);
                plain.u64(4);
                plain.u32(0);
            }
            const auto sealed = aes_gcm_seal(keys.storage, plain.data(), aad);
            Writer outer;
            outer.fixed(std::array<uint8_t, 8>{'M', 'R', 'T', 'S', '0', '0', '0', '2'});
            outer.fixed(sealed.nonce);
            outer.fixed(sealed.tag);
            outer.bytes(sealed.ciphertext);
            char name[8];
            std::snprintf(name, sizeof(name), "%02x.meta", shard);
            std::ofstream(generation / name, std::ios::binary)
                .write(reinterpret_cast<const char*>(outer.data().data()),
                       static_cast<std::streamsize>(outer.data().size()));
        }
        std::ofstream(root / "claims.current") << "gen-1-1-1\n";
        // The journal after the checkpoint: b claims data_ids[1].
        constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'R', 'T', 'J', '1'};
        SealedJournal journal(root / "claims.log", keys.storage, journal_aad, 4U << 20);
        Writer frame;
        frame.u8(1);
        frame.u8(static_cast<uint8_t>(RetentionClass::data));
        frame.fixed(b.bytes);
        frame.u64(6);
        frame.u32(1);
        frame.fixed(data_ids[1].bytes);
        journal.append(frame.data());
    }
    {
        RetentionStore retention(state, keys.storage);
        CHECK(!retention.retained(RetentionClass::data, data_ids[0]));
        CHECK(retention.claims(RetentionClass::data, data_ids[0]).removed.at(b) == 5);
        CHECK(retention.claims(RetentionClass::data, data_ids[1]).adds.size() == 2);
        for (size_t i = 1; i < data_ids.size(); ++i)
            CHECK(retention.retained(RetentionClass::data, data_ids[i]));
        CHECK(retention.retained(RetentionClass::control, control_id));
        CHECK(retention.claim_objects(RetentionClass::data) == data_ids.size() - 1);
        // The tombstone still suppresses b's older dot.
        retention.retain(RetentionClass::data, data_ids[0], {b, 5});
        CHECK(!retention.retained(RetentionClass::data, data_ids[0]));
        CHECK(std::filesystem::exists(root / "claims.current"));
        REQUIRE(retention.compact_if_needed(1));
    }
    CHECK(!std::filesystem::exists(root / "claims.current"));
    CHECK(!std::filesystem::exists(root / "checkpoints"));
    RetentionStore reopened(state, keys.storage);
    CHECK(reopened.claims(RetentionClass::data, data_ids[1]).adds.size() == 2);
    CHECK(reopened.retained(RetentionClass::control, control_id));
    CHECK(reopened.claim_objects(RetentionClass::data) == data_ids.size() - 1);
}

MACHA_FAST_TEST("storage_metadata", test_retention_claim_is_physical_gc_barrier) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto disk = t.path() / "disk";
    std::filesystem::create_directories(disk);
    const auto node = random_node_id();
    StoragePool pool(state, node, {{disk, 64ULL * 1024 * 1024}}, keys.storage);
    pool.refresh();
    RetentionStore retention(state, keys.storage);

    auto bytes = pattern(128 * 1024 + 17);
    const auto id = object_id(bytes);
    REQUIRE(pool.put(id, bytes));
    auto physical = object_path(disk, id);
    REQUIRE(std::filesystem::exists(physical));
    std::filesystem::last_write_time(physical, std::filesystem::file_time_type::clock::now() - 48h);

    const auto origin = random_node_id();
    retention.retain(RetentionClass::data, id, {origin, 1});
    std::vector<ObjectId> none;
    auto protected_pass = pool.gc_step(none, none, 0ms, 128, {}, [&](const ObjectId& candidate) {
        return retention.retained(RetentionClass::data, candidate);
    });
    CHECK(protected_pass.complete);
    CHECK(pool.has(id));

    // Metadata has observed and removed the only reference: the claim goes first,
    // then physical GC may reclaim the object.
    CHECK(retention.release_unreferenced(RetentionClass::data, none, RetentionClock{{origin, 1}},
                                         16) == 1);
    CHECK(!retention.retained(RetentionClass::data, id));
    auto reclaim_pass = pool.gc_step(none, none, 0ms, 128, {}, [&](const ObjectId& candidate) {
        return retention.retained(RetentionClass::data, candidate);
    });
    CHECK(reclaim_pass.complete);
    CHECK(!pool.has(id));
}

MACHA_FAST_TEST("storage_metadata", test_metadata_record_payload_copy_is_shared) {
    MetadataRecord record;
    record.payload = Bytes(4 * 1024 * 1024, 0x5a);
    const auto* backing = record.payload.data();

    MetadataRecord copy = record;
    MetadataRecord second_copy = copy;
    CHECK(copy.payload.data() == backing);
    CHECK(second_copy.payload.data() == backing);

    copy.payload = Bytes(16, 0x11);
    CHECK(copy.payload.data() != backing);
    CHECK(record.payload.data() == second_copy.payload.data());

    // Many MetadataRecord values share one immutable namespace payload rather
    // than one allocation each; on Linux resident memory is checked too.
    record.payload = Bytes(8 * 1024 * 1024, 0xa5);
    const auto large_backing = record.payload.data();
    const auto rss_before = process_rss_kib();
    std::vector<MetadataRecord> copies(64, record);
    for (const auto& retained : copies)
        CHECK(retained.payload.data() == large_backing);
    if (rss_before) {
        const auto rss_after = process_rss_kib();
        REQUIRE(rss_after.has_value());
        // A deep copy would add about 512 MiB.
        CHECK(*rss_after <= *rss_before + 64 * 1024);
    }
}

MACHA_FAST_TEST("storage_metadata", test_metadata_seed_sibling_replays_after_restart) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto replica_path = t.path() / "seed-sibling";

    const auto genesis = genesis_metadata();
    auto make_sibling = [&](std::string path) {
        auto snapshot = decode_snapshot(genesis.payload);
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        snapshot.entries.emplace(std::move(path), entry);

        MetadataRecord record;
        record.generation = genesis.generation + 1;
        record.previous = genesis.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };

    auto first = make_sibling("/first");
    auto second = make_sibling("/second");
    if (second.hash < first.hash)
        std::swap(first, second);
    REQUIRE(first.hash != second.hash);

    {
        MetadataReplica replica(replica_path, keys.storage);
        REQUIRE(replica.seed(first));
        // Same-generation sibling replacement is an intentional deterministic
        // convergence rule: the greater hash wins when the predecessor agrees.
        REQUIRE(replica.seed(second));
        REQUIRE(replica.remember_current_committed(second.generation, second.hash));
        CHECK(replica.current().hash == second.hash);
        CHECK(replica.committed().hash == second.hash);
    }

    // Journal replay reproduces every transition seed() accepted while live.
    MetadataReplica replayed(replica_path, keys.storage);
    CHECK(replayed.current().hash == second.hash);
    CHECK(replayed.committed().hash == second.hash);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_hash_streaming_matches_canonical_encoding) {
    const uint64_t generation = 0x1122334455667788ULL;
    Hash256 previous{};
    for (size_t i = 0; i < previous.bytes.size(); ++i)
        previous.bytes[i] = static_cast<uint8_t>(i * 7 + 3);
    auto payload = pattern(2 * 1024 * 1024 + 137);

    Writer canonical;
    canonical.u64(generation);
    canonical.fixed(previous.bytes);
    canonical.bytes(payload);
    CHECK(metadata_hash(generation, previous, payload) == sha256(canonical.data()));
}

MACHA_FAST_TEST("storage_metadata", test_metadata_delta_in_place_preserves_namespace_storage) {
    auto snapshot = decode_snapshot(genesis_metadata().payload);
    for (size_t i = 0; i < 4096; ++i) {
        FsEntry entry;
        entry.type = EntryType::file;
        entry.size = i;
        entry.version = i + 1;
        snapshot.entries["/file-" + std::to_string(i)] = entry;
    }

    const auto stable_path = std::string("/file-2048");
    const auto* stable_entry = &snapshot.entries.at(stable_path);
    const auto origin = random_node_id();

    MetadataDelta delta;
    delta.mutation_sequences[origin] = 9;
    auto changed = snapshot.entries.at("/file-7");
    changed.size = 0x12345678;
    ++changed.version;
    delta.upsert_entries["/file-7"] = changed;

    apply_metadata_delta_in_place(snapshot, delta);
    CHECK(&snapshot.entries.at(stable_path) == stable_entry);
    CHECK(snapshot.entries.at("/file-7") == changed);
    CHECK(snapshot.mutation_sequences.at(origin) == 9);

    // Repeated small deltas must operate on the same decoded namespace rather
    // than retaining successive deep copies of it.
    for (uint64_t sequence = 10; sequence < 1010; ++sequence) {
        MetadataDelta update;
        update.mutation_sequences[origin] = sequence;
        auto current = snapshot.entries.at("/file-7");
        ++current.version;
        current.size = sequence;
        update.upsert_entries["/file-7"] = current;
        apply_metadata_delta_in_place(snapshot, update);
        CHECK(&snapshot.entries.at(stable_path) == stable_entry);
    }
    CHECK(snapshot.mutation_sequences.at(origin) == 1009);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_branch_merge_history_roundtrip) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);

    const auto genesis = genesis_metadata();
    const auto base = decode_snapshot(genesis.payload);

    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;

    // Two authors, each with a directory of its own, a different file at one
    // path and a catalogue root of its own.
    auto left_snapshot = base;
    FsEntry left_conflict;
    left_conflict.type = EntryType::file;
    left_conflict.mode = 0644;
    left_conflict.size = 11;
    left_conflict.version = 1;
    {
        Mutation mutation(left_snapshot, 1);
        mutation.put("/left", directory);
        mutation.put("/same", left_conflict);
        mutation.catalogue(object_id(pattern(111)));
    }

    auto right_snapshot = base;
    auto right_conflict = left_conflict;
    right_conflict.size = 22;
    right_conflict.version = 2;
    right_conflict.mtime_ns = 1;
    {
        Mutation mutation(right_snapshot, 2);
        mutation.put("/right", directory);
        mutation.put("/same", right_conflict);
        mutation.catalogue(object_id(pattern(222)));
    }

    auto make_child = [&](const MetadataSnapshot& snapshot) {
        MetadataRecord record;
        record.generation = genesis.generation + 1;
        record.previous = genesis.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    const auto left = make_child(left_snapshot);
    const auto right = make_child(right_snapshot);
    REQUIRE(left.hash != right.hash);

    const auto merged = merge_metadata_heads(left_snapshot, right_snapshot, left.hash, right.hash);
    CHECK(merged.snapshot.entries.contains("/left"));
    CHECK(merged.snapshot.entries.contains("/right"));
    // The later file is in place, and one of the two catalogue roots.
    REQUIRE(merged.snapshot.entries.contains("/same"));
    CHECK(merged.snapshot.entries.at("/same").size == 22);
    CHECK((merged.snapshot.catalogue_root == left_snapshot.catalogue_root ||
           merged.snapshot.catalogue_root == right_snapshot.catalogue_root));
    CHECK(merged.conflicts_created == 2);
    CHECK(merged.snapshot.conflicts.size() == 2);

    auto branch_snapshot = merged.snapshot;
    branch_snapshot.merge_parents = {right.hash};
    const auto branch_encoded = encode_snapshot(branch_snapshot);
    const auto branch_decoded = decode_snapshot(branch_encoded);
    CHECK(branch_decoded.merge_parents == branch_snapshot.merge_parents);
    CHECK(branch_decoded.conflicts == branch_snapshot.conflicts);

    const auto left_path = t.path() / "branch-left";
    const auto right_path = t.path() / "branch-right";
    {
        MetadataReplica left_replica(left_path, keys.storage);
        MetadataReplica right_replica(right_path, keys.storage);
        REQUIRE(left_replica.seed(left));
        REQUIRE(left_replica.remember_current_committed(left.generation, left.hash));
        REQUIRE(right_replica.seed(right));
        REQUIRE(right_replica.remember_current_committed(right.generation, right.hash));

        auto right_history = right_replica.history_entry(right.hash);
        REQUIRE(right_history.has_value());
        REQUIRE(left_replica.import_history(*right_history));
        auto common = left_replica.history_common_ancestor(left.hash, right.hash);
        REQUIRE(common.has_value());
        CHECK(*common == genesis.hash);

        MetadataRecord reconciliation;
        reconciliation.generation = std::max(left.generation, right.generation) + 1;
        reconciliation.previous = left.hash;
        reconciliation.payload = branch_encoded;
        reconciliation.hash = metadata_hash(reconciliation.generation, reconciliation.previous,
                                            reconciliation.payload);

        REQUIRE(left_replica.seed(reconciliation));
        REQUIRE(left_replica.remember_current_committed(reconciliation.generation,
                                                        reconciliation.hash));
        // The right branch can accept the same merge because its committed head
        // is an explicit secondary parent, even though the primary parent is left.
        REQUIRE(right_replica.seed(reconciliation));
        REQUIRE(right_replica.remember_current_committed(reconciliation.generation,
                                                         reconciliation.hash));
        CHECK(left_replica.history_is_ancestor(left.hash, reconciliation.hash));
        CHECK(left_replica.history_is_ancestor(right.hash, reconciliation.hash));
    }

    MetadataReplica reopened_left(left_path, keys.storage);
    MetadataReplica reopened_right(right_path, keys.storage);
    CHECK(reopened_left.committed().hash == reopened_right.committed().hash);
    CHECK(reopened_left.history_is_ancestor(left.hash, reopened_left.committed().hash));
    CHECK(reopened_left.history_is_ancestor(right.hash, reopened_left.committed().hash));
    auto historical_merge = reopened_left.historical(reopened_left.committed().hash);
    REQUIRE(historical_merge.has_value());
    const auto reopened_snapshot = decode_snapshot(historical_merge->payload);
    CHECK(reopened_snapshot.conflicts == branch_snapshot.conflicts);
    CHECK(reopened_snapshot.merge_parents == branch_snapshot.merge_parents);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_commit_store_acceptance_heads_roundtrip) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "accepted-heads";

    const auto genesis = genesis_metadata();
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;

    auto make_child = [&](const std::string& name, uint8_t author) {
        auto snapshot = decode_snapshot(genesis.payload);
        snapshot.metadata_write_replicas_required = 2;
        Mutation(snapshot, author).put(name, directory);
        MetadataRecord record;
        record.generation = genesis.generation + 1;
        record.previous = genesis.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    const auto left = make_child("/left", 1);
    const auto right = make_child("/right", 2);
    REQUIRE(left.hash != right.hash);
    CHECK(decode_snapshot(left.payload).metadata_write_replicas_required == 2);

    NodeId a{}, b{}, c{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    c.bytes[15] = 3;
    MetadataAcceptance left_accept{left.generation, left.hash, 2, {a, b}};
    MetadataAcceptance right_accept{right.generation, right.hash, 2, {b, c}};

    // The SM13 `metadata_participants` roster is migration bookkeeping, not a
    // voter set: a certificate may name any real metadata replica it omits.
    {
        auto rostered_snapshot = decode_snapshot(genesis.payload);
        rostered_snapshot.metadata_write_replicas_required = 2;
        rostered_snapshot.metadata_participants = {a, b};
        rostered_snapshot.entries["/roster-is-not-authority"] = directory;
        MetadataRecord rostered;
        rostered.generation = genesis.generation + 1;
        rostered.previous = genesis.hash;
        rostered.payload = encode_snapshot(rostered_snapshot);
        rostered.hash = metadata_hash(rostered.generation, rostered.previous, rostered.payload);
        MetadataReplica replica(t.path() / "roster-not-authority", keys.storage);
        REQUIRE(replica.store_commit(rostered));
        CHECK(replica.accept_commit(
            MetadataAcceptance{rostered.generation, rostered.hash, 2, {a, c}}));
    }

    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(left));
        const auto before_acceptance = replica.diagnostics();
        REQUIRE(replica.accept_commit(left_accept));
        const auto after_left = replica.diagnostics();
        CHECK(after_left.accepted_head_persistence_writes ==
              before_acceptance.accepted_head_persistence_writes + 1);
        CHECK(after_left.accepted_head_persistence_bytes >
              before_acceptance.accepted_head_persistence_bytes);
        CHECK(after_left.accepted_head_persistence_failures == 0);
        REQUIRE(replica.store_commit(right));
        REQUIRE(replica.accept_commit(right_accept));
        const auto after_right = replica.diagnostics();
        CHECK(after_right.accepted_head_persistence_writes ==
              after_left.accepted_head_persistence_writes + 1);
        CHECK(after_right.accepted_head_persistence_bytes >
              after_left.accepted_head_persistence_bytes);
        CHECK(after_right.accepted_head_persistence_failures == 0);
        auto heads = replica.accepted_heads();
        REQUIRE(heads.size() == 2);
        CHECK(replica.history_is_ancestor(genesis.hash, left.hash));
        CHECK(replica.history_is_ancestor(genesis.hash, right.hash));
    }

    MetadataRecord reconciliation;
    MetadataAcceptance merge_accept;
    {
        MetadataReplica replica(path, keys.storage);
        auto heads = replica.accepted_heads();
        REQUIRE(heads.size() == 2);
        auto common = replica.history_common_ancestor(left.hash, right.hash);
        REQUIRE(common.has_value());
        CHECK(*common == genesis.hash);

        auto merged = merge_metadata_heads(decode_snapshot(left.payload),
                                           decode_snapshot(right.payload), left.hash, right.hash);
        CHECK(merged.snapshot.entries.contains("/left"));
        CHECK(merged.snapshot.entries.contains("/right"));
        auto primary = left;
        auto secondary = right;
        if (secondary.hash < primary.hash)
            std::swap(primary, secondary);
        merged.snapshot.merge_parents = {secondary.hash};
        reconciliation.generation = std::max(left.generation, right.generation) + 1;
        reconciliation.previous = primary.hash;
        reconciliation.payload = encode_snapshot(merged.snapshot);
        reconciliation.hash = metadata_hash(reconciliation.generation, reconciliation.previous,
                                            reconciliation.payload);
        merge_accept = {reconciliation.generation, reconciliation.hash, 2, {a, c}};
        REQUIRE(replica.store_commit(reconciliation));
        REQUIRE(replica.accept_commit(merge_accept));
        heads = replica.accepted_heads();
        REQUIRE(heads.size() == 1);
        CHECK(heads.front().hash == reconciliation.hash);
        CHECK(replica.history_is_ancestor(left.hash, reconciliation.hash));
        CHECK(replica.history_is_ancestor(right.hash, reconciliation.hash));
    }

    MetadataReplica reopened(path, keys.storage);
    auto heads = reopened.accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == reconciliation.hash);
    auto certificate = reopened.acceptance(reconciliation.hash);
    REQUIRE(certificate.has_value());
    CHECK(*certificate == merge_accept);

    // With one head the history is truncated to it: the head stays
    // reconstructable and the branches behind it are dropped.
    const auto history_path = path / "metadata" / "history.log";
    const auto history_before = std::filesystem::file_size(history_path);
    REQUIRE(reopened.compact_history_if_safe(1, 1));
    const auto history_after = std::filesystem::file_size(history_path);
    CHECK(history_after < history_before);
    CHECK(!reopened.historical(left.hash).has_value());
    CHECK(!reopened.historical(right.hash).has_value());
    REQUIRE(reopened.historical(reconciliation.hash).has_value());

    MetadataReplica rerooted(path, keys.storage);
    CHECK(!rerooted.historical(left.hash).has_value());
    REQUIRE(rerooted.historical(reconciliation.hash).has_value());
    CHECK(rerooted.historical(reconciliation.hash)->payload == reconciliation.payload);
}

MACHA_FAST_TEST("storage_metadata",
               test_accept_commit_refreshes_checkpoint_on_both_repair_and_ordinary_paths) {
    // accept_commit() persists the checkpoint after releasing its lock: the
    // checkpoint reflects the new head and survives a reload.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "checkpoint-refresh-paths";

    const auto genesis = genesis_metadata();
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    auto make_child = [&](const MetadataRecord& parent, std::string name) {
        auto snapshot = decode_snapshot(parent.payload);
        snapshot.metadata_write_replicas_required = 1;
        snapshot.entries[std::move(name)] = directory;
        MetadataRecord record;
        record.generation = parent.generation + 1;
        record.previous = parent.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    const auto child = make_child(genesis, "/child");
    const auto grandchild = make_child(child, "/grandchild");

    NodeId a{};
    a.bytes[15] = 1;

    MetadataReplica replica(path, keys.storage);
    REQUIRE(replica.store_commit(child));
    // No existing heads: the "changed" path at the end of accept_commit().
    REQUIRE(replica.accept_commit(MetadataAcceptance{child.generation, child.hash, 1, {a}}));
    CHECK(replica.committed().hash == child.hash);
    {
        // Reopening loads the on-disk checkpoint, proving persist() landed.
        MetadataReplica reopened(path, keys.storage);
        CHECK(reopened.committed().hash == child.hash);
    }

    REQUIRE(replica.store_commit(grandchild));
    // A direct descendant of the sole head prunes child from accepted_heads_ and
    // moves the materialized head and its checkpoint forward.
    REQUIRE(
        replica.accept_commit(MetadataAcceptance{grandchild.generation, grandchild.hash, 1, {a}}));
    auto heads = replica.accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == grandchild.hash);
    CHECK(replica.committed().hash == grandchild.hash);
    {
        MetadataReplica reopened(path, keys.storage);
        CHECK(reopened.committed().hash == grandchild.hash);
    }
}

MACHA_FAST_TEST("storage_metadata", test_merge_delta_primary_may_precede_merge_generation) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "merge-delta-generation-gap";

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    const auto genesis = genesis_metadata();

    auto older_snapshot = decode_snapshot(genesis.payload);
    older_snapshot.metadata_write_replicas_required = 2;
    older_snapshot.entries["/older"] = directory;
    MetadataRecord older;
    older.generation = 10;
    older.previous = genesis.hash;
    older.payload = encode_snapshot(older_snapshot);
    older.hash = metadata_hash(older.generation, older.previous, older.payload);

    MetadataRecord newer;
    MetadataSnapshot newer_snapshot;
    for (size_t attempt = 0; attempt < 256; ++attempt) {
        newer_snapshot = decode_snapshot(genesis.payload);
        newer_snapshot.metadata_write_replicas_required = 2;
        newer_snapshot.entries["/newer-" + std::to_string(attempt)] = directory;
        newer.generation = 20;
        newer.previous = genesis.hash;
        newer.payload = encode_snapshot(newer_snapshot);
        newer.hash = metadata_hash(newer.generation, newer.previous, newer.payload);
        if (older.hash < newer.hash)
            break;
    }
    REQUIRE(older.hash < newer.hash);

    MetadataRecord merge;
    auto merged_snapshot = older_snapshot;
    merged_snapshot.entries.insert(newer_snapshot.entries.begin(), newer_snapshot.entries.end());
    merged_snapshot.merge_parents = {newer.hash};
    merge.generation = newer.generation + 1;
    merge.previous = older.hash; // deterministic lower hash, not generation-1
    merge.payload = encode_snapshot(merged_snapshot);
    merge.hash = metadata_hash(merge.generation, merge.previous, merge.payload);
    auto delta = metadata_delta(older_snapshot, merged_snapshot);
    REQUIRE(delta.has_value());
    const auto encoded_delta = encode_metadata_delta(*delta);

    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(older));
        REQUIRE(replica.accept_commit({older.generation, older.hash, 2, {a, b}}));
        REQUIRE(replica.store_commit(newer));
        REQUIRE(replica.accept_commit({newer.generation, newer.hash, 2, {a, b}}));
        REQUIRE(replica.store_commit(merge, encoded_delta));
        REQUIRE(replica.accept_commit({merge.generation, merge.hash, 2, {a, b}}));
        auto entry = replica.history_entry(merge.hash);
        REQUIRE(entry.has_value());
        CHECK(entry->body == MetadataHistoryEntry::Body::delta);
        CHECK(older.generation + 1 < merge.generation);
    }

    MetadataReplica reopened(path, keys.storage);
    const auto heads = reopened.accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == merge.hash);
    CHECK(reopened.history_is_ancestor(older.hash, merge.hash));
    CHECK(reopened.history_is_ancestor(newer.hash, merge.hash));
}

MACHA_FAST_TEST("storage_metadata",
                test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen) {
    // A reconciliation merge is numbered max(parents)+1 with the lower-hash
    // parent as primary, so its delta sits more than one generation above that
    // parent; it must still reconstruct once evicted from the cache, live and
    // after a restart.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "merge-delta-generation-gap-evicted";

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    const auto genesis = genesis_metadata();

    auto older_snapshot = decode_snapshot(genesis.payload);
    older_snapshot.metadata_write_replicas_required = 2;
    older_snapshot.entries["/older"] = directory;
    MetadataRecord older;
    older.generation = 10;
    older.previous = genesis.hash;
    older.payload = encode_snapshot(older_snapshot);
    older.hash = metadata_hash(older.generation, older.previous, older.payload);

    MetadataRecord newer;
    MetadataSnapshot newer_snapshot;
    for (size_t attempt = 0; attempt < 256; ++attempt) {
        newer_snapshot = decode_snapshot(genesis.payload);
        newer_snapshot.metadata_write_replicas_required = 2;
        newer_snapshot.entries["/newer-" + std::to_string(attempt)] = directory;
        newer.generation = 20;
        newer.previous = genesis.hash;
        newer.payload = encode_snapshot(newer_snapshot);
        newer.hash = metadata_hash(newer.generation, newer.previous, newer.payload);
        if (older.hash < newer.hash)
            break;
    }
    REQUIRE(older.hash < newer.hash);

    MetadataRecord merge;
    auto merged_snapshot = older_snapshot;
    merged_snapshot.entries.insert(newer_snapshot.entries.begin(), newer_snapshot.entries.end());
    merged_snapshot.merge_parents = {newer.hash};
    merge.generation = newer.generation + 1;
    merge.previous = older.hash;
    merge.payload = encode_snapshot(merged_snapshot);
    merge.hash = metadata_hash(merge.generation, merge.previous, merge.payload);
    auto delta = metadata_delta(older_snapshot, merged_snapshot);
    REQUIRE(delta.has_value());
    const auto encoded_delta = encode_metadata_delta(*delta);

    // An unrelated concurrent branch overtakes the merge: `committed` moves to it,
    // the merge stays accepted, and nothing pins it in the cache.
    MetadataRecord divergent;
    auto divergent_snapshot = newer_snapshot;
    divergent_snapshot.entries["/divergent"] = directory;
    divergent.generation = 30;
    divergent.previous = newer.hash;
    divergent.payload = encode_snapshot(divergent_snapshot);
    divergent.hash = metadata_hash(divergent.generation, divergent.previous, divergent.payload);

    {
        // A one-byte materialization budget: only the pinned current/committed head
        // stays cached, so other reads reconstruct from history.log.
        MetadataReplica replica(path, keys.storage, {}, true, 1);
        REQUIRE(replica.store_commit(older));
        REQUIRE(replica.accept_commit({older.generation, older.hash, 2, {a, b}}));
        REQUIRE(replica.store_commit(newer));
        REQUIRE(replica.accept_commit({newer.generation, newer.hash, 2, {a, b}}));
        REQUIRE(replica.store_commit(merge, encoded_delta));
        REQUIRE(replica.accept_commit({merge.generation, merge.hash, 2, {a, b}}));
        auto entry = replica.history_entry(merge.hash);
        REQUIRE(entry.has_value());
        REQUIRE(entry->body == MetadataHistoryEntry::Body::delta);

        REQUIRE(replica.store_commit(divergent));
        REQUIRE(replica.accept_commit({divergent.generation, divergent.hash, 2, {a, b}}));
        CHECK(replica.committed().hash == divergent.hash);
        // Any further materialization evicts the now-unpinned merge.
        REQUIRE(replica.materialized(divergent.hash));

        // The merge still reconstructs from disk once evicted.
        auto reconstructed = replica.materialized(merge.hash);
        REQUIRE(reconstructed);
        CHECK(reconstructed->record.hash == merge.hash);
        CHECK(reconstructed->record.payload == merge.payload);
        CHECK(replica.accepted_heads().size() == 2);
    }

    // A restart does not throw and still sees both heads.
    MetadataReplica reopened(path, keys.storage, {}, true, 1);
    const auto heads = reopened.accepted_heads();
    REQUIRE(heads.size() == 2);
    auto reconstructed = reopened.materialized(merge.hash);
    REQUIRE(reconstructed);
    CHECK(reconstructed->record.payload == merge.payload);
    CHECK(reopened.history_is_ancestor(older.hash, merge.hash));
    CHECK(reopened.history_is_ancestor(newer.hash, merge.hash));
}

MACHA_FAST_TEST("storage_metadata",
                test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live) {
    // An accepted head that cannot be replayed locally does not discard the
    // replica at startup, is reported precisely, and is re-anchored live from a
    // peer's self-contained full record.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto source_path = t.path() / "reanchor-source";
    const auto target_path = t.path() / "reanchor-target";

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    const auto genesis = genesis_metadata();

    auto older_snapshot = decode_snapshot(genesis.payload);
    older_snapshot.metadata_write_replicas_required = 2;
    older_snapshot.entries["/older"] = directory;
    MetadataRecord older;
    older.generation = 10;
    older.previous = genesis.hash;
    older.payload = encode_snapshot(older_snapshot);
    older.hash = metadata_hash(older.generation, older.previous, older.payload);

    MetadataRecord newer;
    MetadataSnapshot newer_snapshot;
    for (size_t attempt = 0; attempt < 256; ++attempt) {
        newer_snapshot = decode_snapshot(genesis.payload);
        newer_snapshot.metadata_write_replicas_required = 2;
        newer_snapshot.entries["/newer-" + std::to_string(attempt)] = directory;
        newer.generation = 20;
        newer.previous = genesis.hash;
        newer.payload = encode_snapshot(newer_snapshot);
        newer.hash = metadata_hash(newer.generation, newer.previous, newer.payload);
        if (older.hash < newer.hash)
            break;
    }
    REQUIRE(older.hash < newer.hash);

    MetadataRecord merge;
    auto merged_snapshot = older_snapshot;
    merged_snapshot.entries.insert(newer_snapshot.entries.begin(), newer_snapshot.entries.end());
    merged_snapshot.merge_parents = {newer.hash};
    merge.generation = newer.generation + 1;
    merge.previous = older.hash;
    merge.payload = encode_snapshot(merged_snapshot);
    merge.hash = metadata_hash(merge.generation, merge.previous, merge.payload);
    auto delta = metadata_delta(older_snapshot, merged_snapshot);
    REQUIRE(delta.has_value());
    const auto encoded_delta = encode_metadata_delta(*delta);

    MetadataRecord divergent;
    auto divergent_snapshot = newer_snapshot;
    divergent_snapshot.entries["/divergent"] = directory;
    divergent.generation = 30;
    divergent.previous = newer.hash;
    divergent.payload = encode_snapshot(divergent_snapshot);
    divergent.hash = metadata_hash(divergent.generation, divergent.previous, divergent.payload);

    // Source: a healthy replica holding both heads (the merge as a delta).
    MetadataReplica source(source_path, keys.storage, {}, true, 1);
    const auto source_history = source_path / "metadata" / "history.log";
    REQUIRE(source.store_commit(older));
    REQUIRE(source.accept_commit({older.generation, older.hash, 2, {a, b}}));
    REQUIRE(source.store_commit(newer));
    REQUIRE(source.accept_commit({newer.generation, newer.hash, 2, {a, b}}));
    REQUIRE(source.store_commit(merge, encoded_delta));
    const auto merge_frame_end = std::filesystem::file_size(source_history);
    REQUIRE(source.accept_commit({merge.generation, merge.hash, 2, {a, b}}));
    REQUIRE(source.store_commit(divergent));
    REQUIRE(source.accept_commit({divergent.generation, divergent.hash, 2, {a, b}}));
    REQUIRE(source.accepted_heads().size() == 2);

    // Target: identical except it never received the merge's history frame,
    // then finds itself with the source's certificate set naming the merge.
    {
        MetadataReplica target(target_path, keys.storage, {}, true, 1);
        REQUIRE(target.store_commit(older));
        REQUIRE(target.accept_commit({older.generation, older.hash, 2, {a, b}}));
        REQUIRE(target.store_commit(newer));
        REQUIRE(target.accept_commit({newer.generation, newer.hash, 2, {a, b}}));
        REQUIRE(target.store_commit(divergent));
        REQUIRE(target.accept_commit({divergent.generation, divergent.hash, 2, {a, b}}));
        CHECK(target.committed().hash == divergent.hash);
    }
    std::filesystem::copy_file(source_path / "metadata" / "heads.meta",
                               target_path / "metadata" / "heads.meta",
                               std::filesystem::copy_options::overwrite_existing);

    // (a) Startup keeps the certificate and flags the head instead of throwing.
    MetadataReplica target(target_path, keys.storage, {}, true, 1);
    CHECK(!target.recovery_required());
    REQUIRE(target.accepted_head_certificates().size() == 2);
    CHECK(target.accepted_heads().size() == 1); // merge excluded from reads
    auto flagged = target.unreconstructable_heads();
    REQUIRE(flagged.size() == 1);
    CHECK(flagged.front() == merge.hash);
    CHECK(!target.materialized(merge.hash));

    // (c) A peer serves the record as a self-contained full body and the
    // target re-anchors it in place.
    auto served = source.full_history_record(merge.hash);
    REQUIRE(served.has_value());
    CHECK(served->body == MetadataHistoryEntry::Body::full);
    CHECK(served->merge_parents == std::vector<Hash256>{newer.hash});
    REQUIRE(target.reanchor_history(*served));
    CHECK(target.unreconstructable_heads().empty());
    CHECK(target.accepted_heads().size() == 2);
    auto repaired = target.materialized(merge.hash);
    REQUIRE(repaired);
    CHECK(repaired->record.payload == merge.payload);
    CHECK(target.history_is_ancestor(older.hash, merge.hash));

    // Garbage is refused: a full body whose hash does not bind its content.
    auto forged = *served;
    forged.generation += 1;
    CHECK(!target.reanchor_history(forged));

    // Re-anchoring over an *indexed* but unreplayable frame supersedes it in
    // place, and a restart prefers the full frame over the older delta.
    {
        // The merge's delta frame stops authenticating, so the enumeration that
        // flags the head and the repair's pre-check both fail to replay it; the
        // cache holds only the committed head, so nothing masks the frame.
        flip_history_frame(source_history, merge_frame_end);
        REQUIRE(source.accepted_heads().size() == 1);
        REQUIRE(source.unreconstructable_heads() == std::vector<Hash256>{merge.hash});
        MetadataHistoryEntry full;
        full.generation = merge.generation;
        full.previous = merge.previous;
        full.hash = merge.hash;
        full.previous_known = true;
        full.merge_parents = {newer.hash};
        full.body = MetadataHistoryEntry::Body::full;
        full.payload.assign(merge.payload.begin(), merge.payload.end());
        const auto before = source.diagnostics().history_records;
        REQUIRE(source.reanchor_history(full));
        CHECK(source.diagnostics().history_records == before + 1);
        // The superseded delta frame is whole again for the restart below.
        flip_history_frame(source_history, merge_frame_end);
        auto entry = source.history_entry(merge.hash);
        REQUIRE(entry.has_value());
        CHECK(entry->body == MetadataHistoryEntry::Body::full);
        CHECK(source.unreconstructable_heads().empty());
        CHECK(source.accepted_heads().size() == 2);
    }
    MetadataReplica reopened_source(source_path, keys.storage, {}, true, 1);
    CHECK(reopened_source.accepted_heads().size() == 2);
    auto reopened_entry = reopened_source.history_entry(merge.hash);
    REQUIRE(reopened_entry.has_value());
    CHECK(reopened_entry->body == MetadataHistoryEntry::Body::full);
    REQUIRE(reopened_source.materialized(merge.hash));
    MetadataReplica reopened_target(target_path, keys.storage, {}, true, 1);
    CHECK(reopened_target.accepted_heads().size() == 2);
    CHECK(reopened_target.unreconstructable_heads().empty());
}

MACHA_FAST_TEST("storage_metadata", test_full_fallback_boundary_heals_when_parent_arrives) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto source_path = t.path() / "boundary-source";
    const auto target_path = t.path() / "boundary-target";

    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    const auto genesis = genesis_metadata();
    auto parent_snapshot = decode_snapshot(genesis.payload);
    parent_snapshot.metadata_write_replicas_required = 2;
    parent_snapshot.entries["/parent"] = directory;
    MetadataRecord parent;
    parent.generation = 49;
    parent.previous = genesis.hash;
    parent.payload = encode_snapshot(parent_snapshot);
    parent.hash = metadata_hash(parent.generation, parent.previous, parent.payload);

    auto child_snapshot = parent_snapshot;
    child_snapshot.entries["/child"] = directory;
    MetadataRecord child;
    child.generation = 50;
    child.previous = parent.hash;
    child.payload = encode_snapshot(child_snapshot);
    child.hash = metadata_hash(child.generation, child.previous, child.payload);

    auto descendant_snapshot = child_snapshot;
    descendant_snapshot.entries["/descendant"] = directory;
    MetadataRecord descendant;
    descendant.generation = 51;
    descendant.previous = child.hash;
    descendant.payload = encode_snapshot(descendant_snapshot);
    descendant.hash =
        metadata_hash(descendant.generation, descendant.previous, descendant.payload);

    auto sibling_snapshot = parent_snapshot;
    sibling_snapshot.entries["/sibling"] = directory;
    MetadataRecord sibling;
    sibling.generation = 50;
    sibling.previous = parent.hash;
    sibling.payload = encode_snapshot(sibling_snapshot);
    sibling.hash = metadata_hash(sibling.generation, sibling.previous, sibling.payload);

    MetadataReplica source(source_path, keys.storage);
    REQUIRE(source.store_commit(parent));
    auto parent_entry = source.history_entry(parent.hash);
    REQUIRE(parent_entry.has_value());

    {
        MetadataReplica target(target_path, keys.storage);
        REQUIRE(target.store_commit(child));
        auto boundary = target.history_entry(child.hash);
        REQUIRE(boundary.has_value());
        CHECK(!boundary->previous_known);

        // The head's direct predecessor exists and the missing ancestry is one level
        // deeper, so healing the head alone does not reach the compacted boundary.
        REQUIRE(target.store_commit(descendant));
        REQUIRE(target.history_contains(descendant.previous));
        CHECK(!target.history_contains(parent.hash));
        const auto resident_before = target.diagnostics().materialization_cache_bytes;
        const auto links = target.history_links(descendant.hash);
        REQUIRE(links.has_value());
        CHECK(links->previous == child.hash);
        CHECK(target.diagnostics().materialization_cache_bytes == resident_before);

        REQUIRE(target.import_history(*parent_entry));
        REQUIRE(target.store_commit(sibling));
        auto common = target.history_common_ancestor(descendant.hash, sibling.hash);
        REQUIRE(common.has_value());
        CHECK(*common == parent.hash);
    }

    MetadataReplica reopened(target_path, keys.storage);
    auto common = reopened.history_common_ancestor(descendant.hash, sibling.hash);
    REQUIRE(common.has_value());
    CHECK(*common == parent.hash);
}

MACHA_FAST_TEST("storage_metadata", test_compacted_direct_predecessor_cannot_resurface_as_head) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "compacted-stale-head";

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;

    const auto genesis = genesis_metadata();
    auto parent_snapshot = decode_snapshot(genesis.payload);
    parent_snapshot.metadata_write_replicas_required = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    parent_snapshot.entries["/parent"] = directory;

    MetadataRecord parent;
    parent.generation = genesis.generation + 1;
    parent.previous = genesis.hash;
    parent.payload = encode_snapshot(parent_snapshot);
    parent.hash = metadata_hash(parent.generation, parent.previous, parent.payload);
    const MetadataAcceptance parent_accept{parent.generation, parent.hash, 2, {a, b}};

    auto child_snapshot = parent_snapshot;
    child_snapshot.entries["/child"] = directory;
    MetadataRecord child;
    child.generation = parent.generation + 1;
    child.previous = parent.hash;
    child.payload = encode_snapshot(child_snapshot);
    child.hash = metadata_hash(child.generation, child.previous, child.payload);
    const MetadataAcceptance child_accept{child.generation, child.hash, 2, {a, b}};

    MetadataHistoryEntry stale_parent;
    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(parent));
        REQUIRE(replica.accept_commit(parent_accept));
        REQUIRE(replica.store_commit(child));
        REQUIRE(replica.accept_commit(child_accept));
        auto heads = replica.accepted_heads();
        REQUIRE(heads.size() == 1);
        CHECK(heads.front().hash == child.hash);

        auto entry = replica.history_entry(parent.hash);
        REQUIRE(entry.has_value());
        stale_parent = *entry;
        REQUIRE(replica.compact_history_if_safe(1, 1));
        CHECK(!replica.historical(parent.hash).has_value());
        CHECK(replica.history_is_ancestor(parent.hash, child.hash));

        // Simulate a restarted peer advertising an old but still-valid accepted
        // head after this node compacted away the predecessor's payload.
        REQUIRE(replica.import_history(stale_parent));
        REQUIRE(replica.accept_commit(parent_accept));
        heads = replica.accepted_heads();
        REQUIRE(heads.size() == 1);
        CHECK(heads.front().hash == child.hash);

        const auto common = replica.history_common_ancestor(parent.hash, child.hash);
        REQUIRE(common.has_value());
        CHECK(*common == parent.hash);
    }

    MetadataReplica reopened(path, keys.storage);
    const auto heads = reopened.accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == child.hash);
}

MACHA_FAST_TEST("storage_metadata",
               test_unreconstructable_accepted_head_is_rate_limited_not_hammered) {
    // Once a head fails to reconstruct and its cooldown is set, later calls
    // neither re-attempt reconstruction nor re-throw.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "unreconstructable-head-backoff";

    const auto genesis = genesis_metadata();
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    auto make_child = [&](const MetadataRecord& parent, std::string name) {
        auto snapshot = decode_snapshot(parent.payload);
        snapshot.entries[std::move(name)] = directory;
        MetadataRecord record;
        record.generation = parent.generation + 1;
        record.previous = parent.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    // Siblings forked from genesis: each accept_commit() below is a real
    // conflicting-branch acceptance. head_z's higher generation makes it the
    // committed head, so head_a is an accepted head the cache does not pin.
    const auto head_a = make_child(genesis, "/a");
    const auto head_b = make_child(genesis, "/b");
    const auto head_c = make_child(genesis, "/c");
    auto head_z = make_child(genesis, "/z");
    head_z.generation = 5;
    head_z.hash = metadata_hash(head_z.generation, head_z.previous, head_z.payload);

    // Only the committed head is cached, so reconstructing head_a reads its
    // history frame.
    MetadataReplica replica(path, keys.storage, {}, true, 1);
    const auto history = path / "metadata" / "history.log";
    REQUIRE(replica.store_commit(head_a));
    const auto head_a_frame_end = std::filesystem::file_size(history);
    REQUIRE(replica.accept_commit(MetadataAcceptance{head_a.generation, head_a.hash, 0, {}}));
    REQUIRE(replica.store_commit(head_z));
    REQUIRE(replica.accept_commit(MetadataAcceptance{head_z.generation, head_z.hash, 0, {}}));
    REQUIRE(replica.committed().hash == head_z.hash);
    REQUIRE(replica.accepted_heads().size() == 2);

    // head_a's frame stops authenticating.
    flip_history_frame(history, head_a_frame_end);

    REQUIRE(replica.store_commit(head_b));
    bool threw = false;
    try {
        replica.accept_commit(MetadataAcceptance{head_b.generation, head_b.hash, 0, {}});
    } catch (const std::exception& error) {
        threw = true;
        CHECK(std::string(error.what()).find("cannot be reconstructed") != std::string::npos);
    }
    CHECK(threw); // The first occurrence still surfaces.
    REQUIRE(replica.unreconstructable_heads() == std::vector<Hash256>{head_a.hash});

    // The frame is whole again: from here a reconstruction of head_a would
    // succeed, so only the cooldown keeps it excluded.
    flip_history_frame(history, head_a_frame_end);

    // A second fork accepted shortly after neither re-attempts reconstructing
    // head_a nor re-throws.
    REQUIRE(replica.store_commit(head_c));
    bool threw_again = false;
    try {
        REQUIRE(replica.accept_commit(MetadataAcceptance{head_c.generation, head_c.hash, 0, {}}));
    } catch (const std::exception&) {
        threw_again = true;
    }
    CHECK(!threw_again);

    // accepted_heads(), the hottest path, also stays quiet under repeated calls
    // and returns the reconstructable heads.
    for (int i = 0; i < 5; ++i) {
        std::vector<MetadataRecord> heads;
        bool heads_threw = false;
        try {
            heads = replica.accepted_heads();
        } catch (const std::exception&) {
            heads_threw = true;
        }
        CHECK(!heads_threw);
        REQUIRE(heads.size() == 3);
        for (const auto& head : heads)
            CHECK(head.hash != head_a.hash);
    }
    // Not retried within the cooldown.
    CHECK(replica.unreconstructable_heads() == std::vector<Hash256>{head_a.hash});
}

MACHA_FAST_TEST("storage_metadata", test_history_transfer_follows_only_materialization_dependencies) {
    Hash256 previous{}, merge_parent{};
    previous.bytes[0] = 1;
    merge_parent.bytes[0] = 2;

    MetadataHistoryEntry checkpoint;
    checkpoint.generation = 10;
    checkpoint.previous = previous;
    checkpoint.previous_known = true;
    checkpoint.merge_parents = {merge_parent};
    checkpoint.body = MetadataHistoryEntry::Body::full;
    CHECK(metadata_history_materialization_dependencies(checkpoint).empty());

    MetadataHistoryEntry delta = checkpoint;
    delta.body = MetadataHistoryEntry::Body::delta;
    const auto required = metadata_history_materialization_dependencies(delta);
    REQUIRE(required.size() == 1);
    CHECK(required.front() == previous);
}

MACHA_FAST_TEST("storage_metadata", test_pristine_joiner_adopts_compacted_cluster_head) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto established_path = t.path() / "established";
    const auto joiner_path = t.path() / "joiner";

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;

    const auto genesis = genesis_metadata();
    auto snapshot = decode_snapshot(genesis.payload);
    snapshot.metadata_write_replicas_required = 2;
    snapshot.mutation_sequences[a] = 7;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    snapshot.entries["/established"] = directory;

    MetadataRecord established;
    established.generation = 50;
    established.previous = genesis.hash;
    established.payload = encode_snapshot(snapshot);
    established.hash =
        metadata_hash(established.generation, established.previous, established.payload);
    const MetadataAcceptance established_accept{
        established.generation, established.hash, 2, {a, b}};

    MetadataHistoryEntry compacted_head;
    {
        MetadataReplica replica(established_path, keys.storage);
        REQUIRE(replica.store_commit(established));
        REQUIRE(replica.accept_commit(established_accept));
        REQUIRE(replica.compact_history_if_safe(1, 1));
        auto entry = replica.history_entry(established.hash);
        REQUIRE(entry.has_value());
        compacted_head = *entry;
        CHECK(!compacted_head.previous_known);
        CHECK(!replica.historical(genesis.hash).has_value());
    }

    MetadataHistoryEntry genesis_entry;
    {
        MetadataReplica joiner(joiner_path, keys.storage, {}, false);
        CHECK(joiner.accepted_heads().empty());
        auto entry = joiner.history_entry(genesis.hash);
        REQUIRE(entry.has_value());
        genesis_entry = *entry;

        // A pristine node imports a valid head whose ancestry was compacted away;
        // canonical genesis is a semantic ancestor, so adoption is immediate.
        REQUIRE(joiner.import_history(compacted_head));
        REQUIRE(joiner.accept_commit(established_accept));
        const auto heads = joiner.accepted_heads();
        REQUIRE(heads.size() == 1);
        CHECK(heads.front().hash == established.hash);
        CHECK(joiner.committed().hash == established.hash);
    }

    {
        MetadataReplica replica(established_path, keys.storage);
        // Exercise the opposite race: an established replica sees the joiner's
        // genesis certificate before the joiner has adopted the cluster head.
        REQUIRE(replica.import_history(genesis_entry));
        REQUIRE(replica.accept_commit({genesis.generation, genesis.hash, 0, {}}));
        const auto heads = replica.accepted_heads();
        REQUIRE(heads.size() == 1);
        CHECK(heads.front().hash == established.hash);
    }

    MetadataReplica reopened_joiner(joiner_path, keys.storage, {}, false);
    auto joiner_heads = reopened_joiner.accepted_heads();
    REQUIRE(joiner_heads.size() == 1);
    CHECK(joiner_heads.front().hash == established.hash);

    MetadataReplica reopened_established(established_path, keys.storage);
    auto established_heads = reopened_established.accepted_heads();
    REQUIRE(established_heads.size() == 1);
    CHECK(established_heads.front().hash == established.hash);
}

namespace {

// A record holding `paths` as directories under the clock `clock`.
MetadataRecord record_with_clock(uint64_t generation, const Hash256& previous,
                                 std::map<NodeId, uint64_t> clock,
                                 std::initializer_list<const char*> paths) {
    auto snapshot = decode_snapshot(genesis_metadata().payload);
    snapshot.metadata_write_replicas_required = 2;
    snapshot.mutation_sequences = std::move(clock);
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    for (const auto* path : paths)
        snapshot.entries[path] = directory;
    MetadataRecord record;
    record.generation = generation;
    record.previous = previous;
    record.payload = encode_snapshot(snapshot);
    record.hash = metadata_hash(record.generation, record.previous, record.payload);
    return record;
}

// The record as a full history entry with no known previous.
MetadataHistoryEntry rootless_entry(const MetadataRecord& record) {
    MetadataHistoryEntry entry;
    entry.generation = record.generation;
    entry.previous = record.previous;
    entry.hash = record.hash;
    entry.previous_known = false;
    entry.body = MetadataHistoryEntry::Body::full;
    entry.payload.assign(record.payload.begin(), record.payload.end());
    return entry;
}

NodeId author(uint8_t id) {
    NodeId node{};
    node.bytes[15] = id;
    return node;
}

MetadataAcceptance accepted_by_two(const MetadataRecord& record) {
    return {record.generation, record.hash, 2, {author(1), author(2)}};
}

std::vector<Hash256> hashes_of(const std::vector<MetadataRecord>& heads) {
    std::vector<Hash256> out;
    for (const auto& head : heads)
        out.push_back(head.hash);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// A head with no clock says nothing about what it holds, so no other head's
// clock makes it an ancestor: it stays a head, to be merged.
MACHA_FAST_TEST("storage_metadata", test_a_head_with_no_clock_is_nobodys_ancestor) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto genesis = genesis_metadata();
    Hash256 unseen{};
    unseen.bytes[0] = 0x55;
    const auto clockless = record_with_clock(genesis.generation + 4, unseen, {}, {"/only-here"});
    const auto other =
        record_with_clock(genesis.generation + 2, genesis.hash, {{author(2), 1}}, {"/other"});
    MetadataReplica replica(t.path() / "clockless", keys.storage);
    REQUIRE(replica.import_history(rootless_entry(clockless)));
    REQUIRE(replica.accept_commit(accepted_by_two(clockless)));
    REQUIRE(replica.import_history(rootless_entry(other)));
    REQUIRE(replica.accept_commit(accepted_by_two(other)));
    CHECK(hashes_of(replica.accepted_heads()) == hashes_of({clockless, other}));
}

MACHA_FAST_TEST("storage_metadata", test_a_head_from_before_truncation_is_recognised_by_its_clock) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "truncated";
    const auto a = author(1), b = author(2);

    const auto genesis = genesis_metadata();
    const auto old = record_with_clock(genesis.generation + 1, genesis.hash, {{a, 1}}, {"/old"});
    const auto between =
        record_with_clock(old.generation + 1, old.hash, {{a, 2}}, {"/old", "/between"});
    const auto now = record_with_clock(between.generation + 1, between.hash, {{a, 3}},
                                       {"/old", "/between", "/now"});
    // Written on `old` by another author: `now` does not carry it.
    const auto side =
        record_with_clock(old.generation + 1, old.hash, {{a, 1}, {b, 1}}, {"/old", "/side"});

    {
        MetadataReplica replica(path, keys.storage);
        for (const auto* record : {&old, &between, &now}) {
            REQUIRE(replica.store_commit(*record));
            REQUIRE(replica.accept_commit(accepted_by_two(*record)));
        }
        REQUIRE(replica.compact_history_if_safe(1, 1));
        CHECK(replica.diagnostics().history_records == 1);
        CHECK(!replica.historical(old.hash).has_value());

        // A peer still holds `old` and offers it. No edge in history joins
        // the two; the clock of `now` covers it.
        REQUIRE(replica.import_history(rootless_entry(old)));
        REQUIRE(replica.accept_commit(accepted_by_two(old)));
        CHECK(!replica.history_is_ancestor(old.hash, now.hash));
        CHECK(hashes_of(replica.accepted_heads()) == std::vector<Hash256>{now.hash});
        CHECK(replica.committed().hash == now.hash);

        // A head whose clock `now` does not cover stays beside it, to be merged.
        REQUIRE(replica.import_history(rootless_entry(side)));
        REQUIRE(replica.accept_commit(accepted_by_two(side)));
        CHECK(hashes_of(replica.accepted_heads()) == hashes_of({now, side}));

        // History is not truncated while two heads are held.
        const auto records = replica.diagnostics().history_records;
        CHECK(!replica.compact_history_if_safe(1, 1));
        CHECK(replica.diagnostics().history_records == records);
    }

    MetadataReplica reopened(path, keys.storage);
    CHECK(hashes_of(reopened.accepted_heads()) == hashes_of({now, side}));
}

MACHA_FAST_TEST("storage_metadata",
                test_returning_replica_adopts_a_head_whose_ancestry_a_peer_truncated) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto a = author(1), b = author(2);

    const auto genesis = genesis_metadata();
    const auto floor =
        record_with_clock(genesis.generation + 1, genesis.hash, {{a, 1}}, {"/floor"});
    const auto between =
        record_with_clock(floor.generation + 1, floor.hash, {{a, 2}}, {"/floor", "/between"});
    const auto beyond = record_with_clock(between.generation + 1, between.hash, {{a, 2}, {b, 1}},
                                          {"/floor", "/between", "/beyond"});

    // The peer went on from `floor` and truncated: its head is all it serves.
    MetadataHistoryEntry served;
    {
        MetadataReplica peer(t.path() / "peer", keys.storage);
        for (const auto* record : {&floor, &between, &beyond}) {
            REQUIRE(peer.store_commit(*record));
            REQUIRE(peer.accept_commit(accepted_by_two(*record)));
        }
        REQUIRE(peer.compact_history_if_safe(1, 1));
        auto entry = peer.history_entry(beyond.hash);
        REQUIRE(entry.has_value());
        served = *entry;
        CHECK(!served.previous_known);
    }

    // The returning replica holds `floor` and never saw `between`.
    {
        MetadataReplica replica(t.path() / "returning", keys.storage);
        REQUIRE(replica.store_commit(floor));
        REQUIRE(replica.accept_commit(accepted_by_two(floor)));
        REQUIRE(replica.import_history(served));
        REQUIRE(replica.accept_commit(accepted_by_two(beyond)));
        CHECK(!replica.history_is_ancestor(floor.hash, beyond.hash));
        CHECK(hashes_of(replica.accepted_heads()) == std::vector<Hash256>{beyond.hash});
        CHECK(replica.committed().hash == beyond.hash);
    }

    // A later head that does not carry this replica's mutations is a second
    // head, whatever its generation.
    {
        Hash256 unseen{};
        unseen.bytes[0] = 0x77;
        const auto foreign =
            record_with_clock(floor.generation + 5, unseen, {{b, 4}}, {"/foreign"});
        MetadataReplica replica(t.path() / "diverged", keys.storage);
        REQUIRE(replica.store_commit(floor));
        REQUIRE(replica.accept_commit(accepted_by_two(floor)));
        REQUIRE(replica.import_history(rootless_entry(foreign)));
        REQUIRE(replica.accept_commit(accepted_by_two(foreign)));
        CHECK(hashes_of(replica.accepted_heads()) == hashes_of({floor, foreign}));
    }
}

MACHA_FAST_TEST("storage_metadata", test_a_checkpoint_write_holds_no_reader_of_the_replica) {
#if defined(__linux__)
    // Setting a head aside moves the committed head and rewrites the
    // checkpoint. The write is held at its fsync; a reader of the replica
    // still answers, and the new head is in memory before the write lands.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "checkpoint-off-lock";
    const auto genesis = genesis_metadata();
    const auto mine =
        record_with_clock(genesis.generation + 1, genesis.hash, {{author(1), 1}}, {"/mine"});
    const auto theirs =
        record_with_clock(genesis.generation + 3, genesis.hash, {{author(2), 1}}, {"/theirs"});
    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(mine));
        REQUIRE(replica.accept_commit(accepted_by_two(mine)));
        REQUIRE(replica.import_history(rootless_entry(theirs)));
        REQUIRE(replica.accept_commit(accepted_by_two(theirs)));
        REQUIRE(replica.committed().hash != mine.hash);

        auto& hold = test_support::fsync_hold;
        {
            std::lock_guard lock(hold.mutex);
            hold.path = (path / "metadata" / "checkpoint.meta").string();
            hold.armed = true;
        }
        std::thread writer([&] { CHECK(replica.set_aside(theirs.hash, 10'000)); });
        bool held = false;
        {
            std::unique_lock lock(hold.mutex);
            held = hold.changed.wait_for(lock, scaled(5s), [&] { return hold.holding; });
        }
        CHECK(held);

        auto reader = std::async(std::launch::async, [&] {
            return std::make_pair(replica.committed().hash,
                                  hashes_of(replica.usable_heads()));
        });
        const bool answered = reader.wait_for(scaled(2s)) == std::future_status::ready;
        {
            std::lock_guard lock(hold.mutex);
            hold.armed = false;
        }
        hold.changed.notify_all();
        writer.join();
        CHECK(answered);
        const auto [committed, usable] = reader.get();
        CHECK(committed == mine.hash);
        CHECK(usable == std::vector<Hash256>{mine.hash});
    }
    // The checkpoint written off the lock opens; a set-aside is this run's
    // alone, so a restart holds both heads again.
    MetadataReplica reopened(path, keys.storage);
    CHECK(hashes_of(reopened.accepted_heads()) == hashes_of({mine, theirs}));
#endif
}

MACHA_FAST_TEST("storage_metadata", test_a_journal_compaction_holds_no_reader_of_the_replica) {
#if defined(__linux__)
    // Compaction writes the checkpoint and empties the journal; the write
    // is held at its fsync and a reader of the replica still answers.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "compaction-off-lock";
    {
        MetadataReplica replica(path, keys.storage);
        for (size_t i = 0; i < 65; ++i) {
            auto base = replica.current();
            auto before = decode_snapshot(base.payload);
            auto after = before;
            after.entries["/"].mtime_ns = static_cast<int64_t>(i + 1);
            auto d = metadata_delta(before, after);
            REQUIRE(d.has_value());
            MetadataRecord proposal;
            REQUIRE(replica.cas_delta(base.generation, base.hash, encode_metadata_delta(*d),
                                      &proposal));
            REQUIRE(replica.remember_current_committed(proposal.generation, proposal.hash));
        }
        const auto head = replica.committed().hash;

        auto& hold = test_support::fsync_hold;
        {
            std::lock_guard lock(hold.mutex);
            hold.path = (path / "metadata" / "checkpoint.meta").string();
            hold.armed = true;
        }
        std::thread compactor([&] { replica.compact(); });
        bool held = false;
        {
            std::unique_lock lock(hold.mutex);
            held = hold.changed.wait_for(lock, scaled(5s), [&] { return hold.holding; });
        }
        CHECK(held);
        auto reader = std::async(std::launch::async, [&] { return replica.committed().hash; });
        const bool answered = reader.wait_for(scaled(2s)) == std::future_status::ready;
        {
            std::lock_guard lock(hold.mutex);
            hold.armed = false;
        }
        hold.changed.notify_all();
        compactor.join();
        CHECK(answered);
        CHECK(reader.get() == head);
    }
    CHECK(std::filesystem::file_size(path / "metadata" / "journal.log") < 4096);
    MetadataReplica reopened(path, keys.storage);
    CHECK(reopened.current().generation == 66);
    CHECK(reopened.current().hash == reopened.committed().hash);
#endif
}

MACHA_FAST_TEST("storage_metadata", test_a_head_set_aside_for_the_horizon_is_dropped) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "set-aside";

    const auto genesis = genesis_metadata();
    const auto mine =
        record_with_clock(genesis.generation + 1, genesis.hash, {{author(1), 1}}, {"/mine"});
    const auto theirs =
        record_with_clock(genesis.generation + 3, genesis.hash, {{author(2), 1}}, {"/theirs"});
    const uint64_t first = 10'000;
    const auto horizon = 1000ms;

    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(mine));
        REQUIRE(replica.accept_commit(accepted_by_two(mine)));

        // The only head is never set aside, and nothing expires.
        CHECK(!replica.set_aside(mine.hash, first));
        CHECK(replica.expire_set_aside(first + 100'000, horizon) == 0);
        CHECK(hashes_of(replica.accepted_heads()) == std::vector<Hash256>{mine.hash});

        REQUIRE(replica.import_history(rootless_entry(theirs)));
        REQUIRE(replica.accept_commit(accepted_by_two(theirs)));
        CHECK(hashes_of(replica.usable_heads()) == hashes_of({mine, theirs}));
        CHECK(replica.set_aside_generation() == 0);

        REQUIRE(replica.set_aside(theirs.hash, first));
        CHECK(hashes_of(replica.usable_heads()) == std::vector<Hash256>{mine.hash});
        CHECK(replica.set_aside_generation() == theirs.generation);
        // A node keeps a head to work on.
        CHECK(!replica.set_aside(mine.hash, first));

        CHECK(replica.expire_set_aside(first + 999, horizon) == 0);
        CHECK(hashes_of(replica.accepted_heads()) == hashes_of({mine, theirs}));
        CHECK(replica.set_aside_generation() == theirs.generation);
    }
    {
        // A reopened replica holds both heads as usable again, and still
        // counts the wait from the first time the head was set aside.
        MetadataReplica replica(path, keys.storage);
        CHECK(hashes_of(replica.usable_heads()) == hashes_of({mine, theirs}));
        CHECK(replica.set_aside_generation() == 0);
        REQUIRE(replica.set_aside(theirs.hash, first + 600));
        CHECK(replica.expire_set_aside(first + 999, horizon) == 0);
        CHECK(replica.expire_set_aside(first + 1000, horizon) == 1);
        CHECK(hashes_of(replica.accepted_heads()) == std::vector<Hash256>{mine.hash});
        CHECK(hashes_of(replica.usable_heads()) == std::vector<Hash256>{mine.hash});
        CHECK(replica.set_aside_generation() == 0);
        CHECK(replica.committed().hash == mine.hash);
        CHECK(replica.expire_set_aside(first + 100'000, horizon) == 0);
    }
    MetadataReplica reopened(path, keys.storage);
    CHECK(hashes_of(reopened.accepted_heads()) == std::vector<Hash256>{mine.hash});
}

MACHA_FAST_TEST("storage_metadata", test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "set-aside-forgotten";
    const auto a = author(1), b = author(2), c = author(3);

    const auto genesis = genesis_metadata();
    const auto mine = record_with_clock(genesis.generation + 1, genesis.hash, {{a, 1}}, {"/mine"});
    const auto theirs =
        record_with_clock(genesis.generation + 3, genesis.hash, {{b, 1}}, {"/theirs"});
    // Carries both.
    const auto merged = record_with_clock(theirs.generation + 1, mine.hash,
                                          {{a, 1}, {b, 1}, {c, 1}}, {"/mine", "/theirs"});
    const uint64_t first = 10'000;
    const auto horizon = 1000ms;
    const auto times = path / "metadata" / "set-aside.meta";

    MetadataReplica replica(path, keys.storage);
    REQUIRE(replica.store_commit(mine));
    REQUIRE(replica.accept_commit(accepted_by_two(mine)));
    REQUIRE(replica.import_history(rootless_entry(theirs)));
    REQUIRE(replica.accept_commit(accepted_by_two(theirs)));
    REQUIRE(replica.set_aside(theirs.hash, first));
    const auto recorded = std::filesystem::file_size(times);

    REQUIRE(replica.import_history(rootless_entry(merged)));
    REQUIRE(replica.accept_commit(accepted_by_two(merged)));
    CHECK(hashes_of(replica.accepted_heads()) == std::vector<Hash256>{merged.hash});
    CHECK(replica.set_aside_generation() == 0);

    CHECK(replica.expire_set_aside(first + 100'000, horizon) == 0);
    CHECK(hashes_of(replica.accepted_heads()) == std::vector<Hash256>{merged.hash});
    CHECK(std::filesystem::file_size(times) < recorded);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_recovery_cache_seed_is_not_accepted_authority) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "cache-recovery";

    auto base = genesis_metadata();
    auto snapshot = decode_snapshot(base.payload);
    FsEntry cached_entry;
    cached_entry.type = EntryType::directory;
    cached_entry.mode = 0755;
    snapshot.entries["/cached-only"] = cached_entry;
    MetadataRecord cached;
    cached.generation = base.generation + 1;
    cached.previous = base.hash;
    cached.payload = encode_snapshot(snapshot);
    cached.hash = metadata_hash(cached.generation, cached.previous, cached.payload);

    // Force primary-state recovery while providing a valid persistent-cache seed.
    // The seed may materialise a read/recovery snapshot, but it must not acquire
    // accepted-head authority without a certificate from another replica.
    std::filesystem::create_directories(path / "metadata");
    {
        std::ofstream corrupt(path / "metadata" / "checkpoint.meta", std::ios::binary);
        corrupt << "not encrypted metadata";
    }
    {
        MetadataReplica recovered(path, keys.storage, cached);
        CHECK(recovered.recovery_required());
        CHECK(recovered.committed().hash == cached.hash);
        CHECK(recovered.accepted_heads().empty());
        CHECK(!recovered.acceptance(cached.hash).has_value());
    }

    // The durable recovery marker must preserve the same fail-closed state on
    // another restart; merely being able to decrypt the fallback checkpoint does
    // not turn it into historical acceptance evidence.
    MetadataReplica reopened(path, keys.storage, cached);
    CHECK(reopened.recovery_required());
    CHECK(reopened.committed().hash == cached.hash);
    CHECK(reopened.accepted_heads().empty());
    CHECK(!reopened.acceptance(cached.hash).has_value());
}

MACHA_FAST_TEST("storage_metadata", test_metadata_journal_mid_frame_corruption_truncates_not_reseeds) {
    // An unauthenticatable frame mid-journal: the journal is a CAS chain, so the
    // prefix before it is exactly a crash before that append. The prefix is kept,
    // the tail quarantined, and startup proceeds normally.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto genesis = genesis_metadata();
    const auto path = t.path() / "mid-journal-corruption";

    auto snapshot = decode_snapshot(genesis.payload);
    FsEntry dir;
    dir.type = EntryType::directory;
    dir.mode = 0755;
    snapshot.entries["/first"] = dir;
    MetadataRecord first;
    first.generation = genesis.generation + 1;
    first.previous = genesis.hash;
    first.payload = encode_snapshot(snapshot);
    first.hash = metadata_hash(first.generation, first.previous, first.payload);

    snapshot.entries["/second"] = dir;
    MetadataRecord second;
    second.generation = first.generation + 1;
    second.previous = first.hash;
    second.payload = encode_snapshot(snapshot);
    second.hash = metadata_hash(second.generation, second.previous, second.payload);

    const auto journal = path / "metadata" / "journal.log";
    uint64_t first_frame_end = 0;
    {
        MetadataReplica replica(path, keys.storage);
        MetadataRecord observed;
        REQUIRE(replica.cas(genesis.generation, genesis.hash, first.payload, &observed));
        first_frame_end = std::filesystem::file_size(journal);
        REQUIRE(first_frame_end > 4);
        REQUIRE(replica.cas(first.generation, first.hash, second.payload, &observed));
        CHECK(replica.current().hash == second.hash);
    }
    REQUIRE(std::filesystem::file_size(journal) > first_frame_end);

    // Flip a ciphertext byte inside the first frame; a complete, valid frame
    // follows it, so this is not a torn tail.
    {
        std::fstream file(journal, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file.good());
        file.seekp(static_cast<std::streamoff>(first_frame_end / 2));
        char byte{};
        file.seekg(static_cast<std::streamoff>(first_frame_end / 2));
        file.read(&byte, 1);
        byte ^= 0x5a;
        file.seekp(static_cast<std::streamoff>(first_frame_end / 2));
        file.write(&byte, 1);
        file.flush();
        REQUIRE(file.good());
    }

    MetadataReplica reopened(path, keys.storage);
    CHECK(!reopened.recovery_required());
    CHECK(reopened.current().hash == genesis.hash);
    CHECK(reopened.committed().hash == genesis.hash);
    CHECK(std::filesystem::file_size(journal) == 0);
    CHECK(std::filesystem::exists(path / "metadata" / "checkpoint.meta"));
    bool quarantined = false;
    for (const auto& entry : std::filesystem::directory_iterator(path / "metadata"))
        if (entry.path().filename().string().find("journal.log.corrupt.") == 0)
            quarantined = true;
    CHECK(quarantined);

    // The replica is usable: the same mutation applies again.
    MetadataRecord observed;
    REQUIRE(reopened.cas(genesis.generation, genesis.hash, first.payload, &observed));
    CHECK(observed.hash == first.hash);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_conflict_alternatives_are_gc_roots) {
    MetadataSnapshot snapshot;
    const auto effective_root = object_id(pattern(101));
    const auto base_root = object_id(pattern(102));
    const auto left_root = object_id(pattern(103));
    const auto right_root = object_id(pattern(104));
    snapshot.catalogue_root = effective_root;

    const auto base_extent = object_id(pattern(201));
    const auto left_extent = object_id(pattern(202));
    const auto right_extent = object_id(pattern(203));
    FsEntry base_entry;
    base_entry.type = EntryType::file;
    base_entry.size = 1;
    base_entry.extents.push_back({0, 1, base_extent, false});
    auto left_entry = base_entry;
    left_entry.extents.front().id = left_extent;
    auto right_entry = base_entry;
    right_entry.extents.front().id = right_extent;

    MetadataConflict namespace_conflict;
    namespace_conflict.kind = MetadataConflictKind::namespace_entry;
    namespace_conflict.key = "/conflicted.bin";
    namespace_conflict.base_entry = base_entry;
    namespace_conflict.left_entry = left_entry;
    namespace_conflict.right_entry = right_entry;
    snapshot.conflicts.emplace("namespace", namespace_conflict);

    MetadataConflict catalogue_conflict;
    catalogue_conflict.kind = MetadataConflictKind::catalogue_root;
    catalogue_conflict.key = "catalogue_root";
    catalogue_conflict.base_catalogue_root = base_root;
    catalogue_conflict.left_catalogue_root = left_root;
    catalogue_conflict.right_catalogue_root = right_root;
    snapshot.conflicts.emplace("catalogue", catalogue_conflict);

    const auto extent_roots = metadata_conflict_extent_roots(snapshot);
    CHECK(extent_roots == std::set<ObjectId>({base_extent, left_extent, right_extent}));
    const auto catalogue_roots = metadata_catalogue_root_set(snapshot);
    CHECK(catalogue_roots ==
          std::set<ObjectId>({effective_root, base_root, left_root, right_root}));
}

MACHA_FAST_TEST("storage_metadata",
                test_metadata_legacy_prepare_journal_replays_deterministically) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto genesis = genesis_metadata();

    auto first_snapshot = decode_snapshot(genesis.payload);
    FsEntry first;
    first.type = EntryType::directory;
    first.mode = 0755;
    first_snapshot.entries["/first"] = first;

    auto second_snapshot = decode_snapshot(genesis.payload);
    FsEntry second = first;
    second_snapshot.entries["/second"] = second;

    auto child = [&](const MetadataSnapshot& snapshot) {
        MetadataRecord record;
        record.generation = genesis.generation + 1;
        record.previous = genesis.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    auto a = child(first_snapshot);
    auto b = child(second_snapshot);
    REQUIRE(a.hash != b.hash);
    const auto& low = a.hash < b.hash ? a : b;
    const auto& high = a.hash < b.hash ? b : a;

    const auto path = t.path() / "prepare-race";
    {
        MetadataReplica replica(path, keys.storage);
        MetadataRecord observed;
        REQUIRE(replica.cas(genesis.generation, genesis.hash, high.payload, &observed));
        CHECK(observed.hash == high.hash);

        // Legacy PREPARE journal state stays readable; protocol 20 never uses
        // MetadataReplica::cas() for live publication.
        REQUIRE(replica.cas(genesis.generation, genesis.hash, low.payload, &observed));
        CHECK(observed.hash == low.hash);
        CHECK(!replica.cas(genesis.generation, genesis.hash, high.payload, &observed));
        CHECK(observed.hash == low.hash);
        CHECK(replica.committed().hash == genesis.hash);
    }

    // A legacy PREPARE replacement sequence still replays, so an interrupted
    // legacy journal recovers without data loss.
    MetadataReplica reopened(path, keys.storage);
    CHECK(reopened.current().hash == low.hash);
    CHECK(reopened.committed().hash == genesis.hash);
    MetadataRecord observed;
    REQUIRE(reopened.cas(genesis.generation, genesis.hash, low.payload, &observed));
    CHECK(observed.hash == low.hash);
    REQUIRE(reopened.remember_current_committed(low.generation, low.hash));
    CHECK(reopened.committed().hash == low.hash);
}

MACHA_FAST_TEST("storage_metadata",
                test_protocol20_checkpoint_without_acceptance_never_self_promotes) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "missing-head-certificate";

    const auto genesis = genesis_metadata();
    auto snapshot = decode_snapshot(genesis.payload);
    snapshot.metadata_write_replicas_required = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    snapshot.entries["/accepted"] = directory;
    MetadataRecord record;
    record.generation = genesis.generation + 1;
    record.previous = genesis.hash;
    record.payload = encode_snapshot(snapshot);
    record.hash = metadata_hash(record.generation, record.previous, record.payload);

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(record));
        REQUIRE(replica.accept_commit({record.generation, record.hash, 2, {a, b}}));
        CHECK(replica.committed().hash == record.hash);
    }

    // The acceptance-proof file is lost while the SM12 checkpoint/history stays
    // readable: the checkpoint is recovery material, never authority.
    REQUIRE(std::filesystem::remove(path / "metadata" / "heads.meta"));
    MetadataReplica reopened(path, keys.storage);
    CHECK(reopened.committed().hash == record.hash);
    CHECK(reopened.recovery_required());
    CHECK(reopened.accepted_heads().empty());
    CHECK(!reopened.acceptance(record.hash).has_value());
}

// A node authors under its node id, its sequence never regresses, and a
// restart continues it.
MACHA_FAST_TEST("storage_metadata", test_an_author_sequence_never_regresses) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "author";
    const auto node = random_node_id();
    const auto other = random_node_id();
    {
        MetadataReplica replica(path, keys.storage);
        CHECK((replica.reserve_mutation_dot(node, {}) == MetadataDot{node, 1}));
        // A head that has seen further (merged from a peer) moves it on.
        CHECK((replica.reserve_mutation_dot(node, {{node, 5}, {other, 9}}) ==
               MetadataDot{node, 6}));
        // A head that has seen less does not move it back.
        CHECK((replica.reserve_mutation_dot(node, {{node, 2}}) == MetadataDot{node, 7}));
    }
    MetadataReplica reopened(path, keys.storage);
    CHECK((reopened.reserve_mutation_dot(node, {{node, 1}}) == MetadataDot{node, 8}));
    CHECK((reopened.reserve_mutation_dot(node, {{node, 100}}) == MetadataDot{node, 101}));
    CHECK((reopened.author_ids(node, {}) == std::vector<NodeId>{node}));
}

// One author's commits form a chain. A head that lacks a mutation this node
// had accepted cannot be extended under the same author: the node takes a
// new author id and starts again, and keeps doing so across a restart.
MACHA_FAST_TEST("storage_metadata", test_a_broken_author_chain_starts_a_new_author) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "author";
    const auto node = random_node_id();
    NodeId second{};
    {
        MetadataReplica replica(path, keys.storage);
        const auto first = replica.reserve_mutation_dot(node, {});
        replica.note_author_accepted(first);
        const auto next = replica.reserve_mutation_dot(node, {{node, 1}});
        CHECK((next == MetadataDot{node, 2}));
        replica.note_author_accepted(next);

        // The head on offer carries only the first.
        const auto fresh = replica.reserve_mutation_dot(node, {{node, 1}});
        CHECK(fresh.author != node);
        CHECK(fresh.sequence == 1);
        second = fresh.author;
        CHECK((replica.author_ids(node, {}) == std::vector<NodeId>{second, node}));
        // The new author continues normally.
        replica.note_author_accepted(fresh);
        CHECK((replica.reserve_mutation_dot(node, {{node, 1}, {second, 1}}) ==
               MetadataDot{second, 2}));
    }
    MetadataReplica reopened(path, keys.storage);
    CHECK((reopened.author_ids(node, {}) == std::vector<NodeId>{second, node}));
    CHECK((reopened.reserve_mutation_dot(node, {{second, 2}}) == MetadataDot{second, 3}));
}

// A node whose author record is gone while a head shows it has authored
// before may not continue the old sequence.
MACHA_FAST_TEST("storage_metadata", test_a_lost_author_record_starts_a_new_author) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto node = random_node_id();
    MetadataReplica replica(t.path() / "author", keys.storage);
    const auto dot = replica.reserve_mutation_dot(node, {{node, 41}});
    CHECK(dot.author != node);
    CHECK(dot.sequence == 1);
    CHECK((replica.author_ids(node, {}) == std::vector<NodeId>{dot.author, node}));

    CHECK(clock_covers({{node, 41}}, MetadataDot{node, 41}));
    CHECK(!clock_covers({{node, 41}}, MetadataDot{node, 42}));
    CHECK(!clock_covers({{node, 41}}, dot));
    CHECK(!static_cast<bool>(MetadataDot{}));
}

MACHA_FAST_TEST("storage_metadata", test_protocol20_delta_preserves_governance_snapshot_encoding) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto replica_id = random_node_id();

    auto parent_snapshot = decode_snapshot(genesis_metadata().payload);
    parent_snapshot.metadata_voters.clear();
    parent_snapshot.metadata_write_replicas_required = 1;
    parent_snapshot.retention_baseline_complete = true;

    const auto genesis = genesis_metadata();
    MetadataRecord parent;
    parent.generation = 2;
    parent.previous = genesis.hash;
    parent.payload = encode_snapshot(parent_snapshot);
    parent.hash = metadata_hash(parent.generation, parent.previous, parent.payload);

    MetadataReplica replica(t.path() / "protocol20-delta", keys.storage);
    REQUIRE(replica.store_commit(parent));
    REQUIRE(replica.accept_commit({parent.generation, parent.hash, 1, {replica_id}}));

    auto child_snapshot = parent_snapshot;
    child_snapshot.mutation_sequences[replica_id] = 1;
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.version = 1;
    child_snapshot.entries["/delta.bin"] = file;

    auto delta = metadata_delta(parent_snapshot, child_snapshot);
    REQUIRE(delta.has_value());
    auto encoded_delta = encode_metadata_delta(*delta);
    REQUIRE(encoded_delta.size() >= 8);
    CHECK(encoded_delta[7] == '5');
    CHECK(encode_snapshot(
              apply_metadata_delta(parent_snapshot, decode_metadata_delta(encoded_delta))) ==
          encode_snapshot(child_snapshot));

    MetadataRecord child;
    child.generation = parent.generation + 1;
    child.previous = parent.hash;
    child.payload = encode_snapshot(child_snapshot);
    child.hash = metadata_hash(child.generation, child.previous, child.payload);
    REQUIRE(replica.store_commit(child, encoded_delta));
    REQUIRE(replica.accept_commit({child.generation, child.hash, 1, {replica_id}}));
    REQUIRE(replica.historical(child.hash).has_value());
    CHECK(replica.historical(child.hash)->payload == child.payload);

    MetadataReplica reopened(t.path() / "protocol20-delta", keys.storage);
    REQUIRE(reopened.historical(child.hash).has_value());
    CHECK(reopened.historical(child.hash)->payload == child.payload);
    REQUIRE(reopened.acceptance(child.hash).has_value());
}

MACHA_FAST_TEST("storage_metadata", test_metadata_delta_chain_reuses_bounded_materialized_head) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto origin = random_node_id();

    auto snapshot = decode_snapshot(genesis_metadata().payload);
    snapshot.metadata_voters.clear();
    snapshot.metadata_write_replicas_required = 1;

    MetadataRecord head;
    head.generation = 2;
    head.previous = genesis_metadata().hash;
    head.payload = encode_snapshot(snapshot);
    head.hash = metadata_hash(head.generation, head.previous, head.payload);

    const auto path = t.path() / "delta-reconstruction-amplification";
    constexpr size_t chain_length = 200;
    std::optional<MetadataRecord> early_record;
    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(head));
        for (size_t i = 0; i < chain_length; ++i) {
            auto next_snapshot = snapshot;
            next_snapshot.mutation_sequences[origin] = i + 1;
            FsEntry entry;
            entry.type = EntryType::file;
            entry.mode = 0644;
            entry.version = 1;
            next_snapshot.entries["/delta-" + std::to_string(i)] = entry;

            auto delta = metadata_delta(snapshot, next_snapshot);
            REQUIRE(delta.has_value());
            auto encoded_delta = encode_metadata_delta(*delta);

            MetadataRecord child;
            child.generation = head.generation + 1;
            child.previous = head.hash;
            child.payload = encode_snapshot(next_snapshot);
            child.hash = metadata_hash(child.generation, child.previous, child.payload);
            REQUIRE(replica.store_commit(child, encoded_delta));
            if (i == 5)
                early_record = child;
            snapshot = std::move(next_snapshot);
            head = std::move(child);
        }
    }

    // Reopening removes any process-local materializations produced while the
    // chain was built. Concurrent cache misses share one off-lock computation;
    // later callers reuse the validated immutable result.
    MetadataReplica replica(path, keys.storage);
    const auto before = replica.diagnostics();
    constexpr size_t concurrent_readers = 8;
    std::array<std::shared_ptr<const MetadataMaterialization>, concurrent_readers> results;
    std::atomic_bool start{};
    std::vector<std::jthread> readers;
    for (size_t index = 0; index < concurrent_readers; ++index) {
        readers.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            results[index] = replica.materialized(head.hash);
        });
    }
    start.store(true, std::memory_order_release);
    readers.clear();
    const auto middle = replica.diagnostics();
    auto second = replica.historical(head.hash);
    const auto after = replica.diagnostics();
    REQUIRE(second.has_value());
    CHECK(second->payload == head.payload);
    for (const auto& result : results) {
        REQUIRE(result != nullptr);
        CHECK(result->record.payload == head.payload);
        CHECK(result == results.front());
    }

    CHECK(middle.historical_requests - before.historical_requests == concurrent_readers);
    CHECK(after.historical_requests - middle.historical_requests == 1);
    CHECK(middle.historical_reconstructions - before.historical_reconstructions == 1);
    CHECK(after.historical_reconstructions - middle.historical_reconstructions == 0);
    CHECK(middle.historical_deltas_applied - before.historical_deltas_applied == chain_length);
    CHECK(after.historical_deltas_applied - middle.historical_deltas_applied == 0);
    CHECK(middle.materialization_cache_hits - before.materialization_cache_hits ==
          concurrent_readers - 1);
    CHECK(after.materialization_cache_hits - middle.materialization_cache_hits == 1);
    CHECK(middle.materialization_cache_misses - before.materialization_cache_misses == 1);
    CHECK(after.materialization_cache_entries <= 64);
    // Replaying a long delta chain caches only the requested result, not every
    // intermediate namespace.
    CHECK(middle.materialization_cache_entries <= before.materialization_cache_entries + 1);
    CHECK(middle.materialization_cache_bytes >= results.front()->resident_bytes);

    auto first_materialized = replica.materialized(head.hash);
    auto second_materialized = replica.materialized(head.hash);
    REQUIRE(first_materialized != nullptr);
    REQUIRE(second_materialized != nullptr);
    CHECK(first_materialized == second_materialized);
    CHECK(first_materialized->snapshot == second_materialized->snapshot);
    CHECK(encode_snapshot(*first_materialized->snapshot) == head.payload);

    // Materialization is an optimization, never authority. This history was
    // deliberately stored without an acceptance certificate.
    CHECK(!replica.acceptance(head.hash).has_value());
    const auto accepted = replica.accepted_heads();
    CHECK(std::none_of(accepted.begin(), accepted.end(),
                       [&](const MetadataRecord& record) { return record.hash == head.hash; }));

    // The bounded cache evicts old, non-authoritative materializations. Such a
    // record remains reconstructible from durable history and must return
    // byte-identical state when requested again.
    REQUIRE(early_record.has_value());
    const auto before_evicted_lookup = replica.diagnostics();
    auto reconstructed_early = replica.historical(early_record->hash);
    const auto after_evicted_lookup = replica.diagnostics();
    REQUIRE(reconstructed_early.has_value());
    CHECK(reconstructed_early->payload == early_record->payload);
    CHECK(after_evicted_lookup.historical_reconstructions -
              before_evicted_lookup.historical_reconstructions ==
          1);
    CHECK(after_evicted_lookup.materialization_cache_entries <= 64);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_history_payloads_are_disk_backed_and_byte_bounded) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "disk-backed-history";
    const auto origin = random_node_id();

    auto snapshot = decode_snapshot(genesis_metadata().payload);
    snapshot.metadata_voters.clear();
    snapshot.metadata_write_replicas_required = 1;
    for (size_t i = 0; i < 1000; ++i) {
        FsEntry entry;
        entry.type = EntryType::file;
        entry.mode = 0644;
        entry.size = i * 4096;
        entry.version = 1;
        snapshot.entries["/library/long-title-" + std::to_string(i)] = entry;
    }

    MetadataRecord head = genesis_metadata();
    {
        MetadataReplica writer(path, keys.storage);
        for (uint64_t generation = 2; generation <= 42; ++generation) {
            snapshot.mutation_sequences[origin] = generation;
            MetadataRecord next;
            next.generation = generation;
            next.previous = head.hash;
            next.payload = encode_snapshot(snapshot);
            next.hash = metadata_hash(next.generation, next.previous, next.payload);
            REQUIRE(writer.store_commit(next));
            head = std::move(next);
        }
    }

    constexpr uint64_t cache_limit = 64 * 1024;
    MetadataReplica reopened(path, keys.storage, {}, true, cache_limit);
    const auto cold = reopened.diagnostics();
    CHECK(cold.history_records >= 41);
    CHECK(cold.history_file_bytes > 1024 * 1024);
    CHECK(cold.history_resident_payload_bytes == 0);
    CHECK(cold.materialization_cache_limit_bytes == cache_limit);
    CHECK(cold.materialization_cache_bytes <= cache_limit);

    auto recovered = reopened.materialized(head.hash);
    REQUIRE(recovered != nullptr);
    CHECK(recovered->record.payload == head.payload);
    const uint64_t structural_floor =
        recovered->record.payload.size() + sizeof(MetadataSnapshot) +
        recovered->snapshot->entries.size() *
            (sizeof(decltype(snapshot.entries)::value_type) + 4 * sizeof(void*));
    CHECK(recovered->resident_bytes >= structural_floor);
    const auto after = reopened.diagnostics();
    // This historical full snapshot is larger than the cache budget. It is
    // returned to the caller but not retained as unbounded process state.
    CHECK(after.materialization_cache_bytes <= cache_limit);
    CHECK(after.history_resident_payload_bytes == 0);
}

MACHA_FAST_TEST("storage_metadata",
                test_cold_accepted_head_validation_caches_only_owned_results) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "cold-accepted-head-ownership";
    auto witness = random_node_id();

    auto snapshot = decode_snapshot(genesis_metadata().payload);
    snapshot.metadata_voters.clear();
    snapshot.metadata_write_replicas_required = 1;
    MetadataRecord head = genesis_metadata();
    constexpr size_t chain_length = 80;
    {
        MetadataReplica writer(path, keys.storage);
        MetadataRecord policy_root;
        policy_root.generation = head.generation + 1;
        policy_root.previous = head.hash;
        policy_root.payload = encode_snapshot(snapshot);
        policy_root.hash = metadata_hash(policy_root.generation, policy_root.previous,
                                         policy_root.payload);
        REQUIRE(writer.store_commit(policy_root));
        head = std::move(policy_root);
        for (size_t i = 0; i < chain_length; ++i) {
            auto next_snapshot = snapshot;
            next_snapshot.mutation_sequences[witness] = i + 1;
            FsEntry entry;
            entry.type = EntryType::file;
            entry.mode = 0644;
            entry.version = 1;
            next_snapshot.entries["/owned-" + std::to_string(i)] = entry;
            auto delta = metadata_delta(snapshot, next_snapshot);
            REQUIRE(delta.has_value());

            MetadataRecord child;
            child.generation = head.generation + 1;
            child.previous = head.hash;
            child.payload = encode_snapshot(next_snapshot);
            child.hash = metadata_hash(child.generation, child.previous, child.payload);
            REQUIRE(writer.store_commit(child, encode_metadata_delta(*delta)));
            snapshot = std::move(next_snapshot);
            head = std::move(child);
        }
        REQUIRE(writer.accept_commit(
            MetadataAcceptance{head.generation, head.hash, 1, {witness}}));
    }

    // Constructor-time accepted-head validation uses materialized_locked(). It
    // may retain the accepted result and its direct policy parent, but never one
    // complete tree for every delta traversed to obtain them.
    MetadataReplica reopened(path, keys.storage);
    const auto diagnostics = reopened.diagnostics();
    CHECK(diagnostics.materialization_cache_entries <= 2);
    auto accepted = reopened.accepted_heads();
    REQUIRE(accepted.size() == 1);
    CHECK(accepted.front().hash == head.hash);
    auto materialized = reopened.materialized(head.hash);
    REQUIRE(materialized != nullptr);
    CHECK(materialized->record.payload == head.payload);
}

MACHA_FAST_TEST("storage_metadata",
                test_metadata_materialization_cache_never_validates_corrupt_delta) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);

    auto parent_snapshot = decode_snapshot(genesis_metadata().payload);
    parent_snapshot.metadata_voters.clear();
    parent_snapshot.metadata_write_replicas_required = 1;

    MetadataRecord parent;
    parent.generation = 2;
    parent.previous = genesis_metadata().hash;
    parent.payload = encode_snapshot(parent_snapshot);
    parent.hash = metadata_hash(parent.generation, parent.previous, parent.payload);

    MetadataReplica replica(t.path() / "corrupt-delta-cache", keys.storage);
    REQUIRE(replica.store_commit(parent));
    auto cached_parent = replica.materialized(parent.hash);
    REQUIRE(cached_parent != nullptr);
    const auto cache_after_parent = replica.diagnostics().materialization_cache_entries;

    auto child_snapshot = parent_snapshot;
    FsEntry child_directory;
    child_directory.type = EntryType::directory;
    child_directory.mode = 0755;
    child_snapshot.entries["/valid-child"] = child_directory;
    const auto child_delta = metadata_delta(parent_snapshot, child_snapshot);
    REQUIRE(child_delta.has_value());

    MetadataRecord child;
    child.generation = parent.generation + 1;
    child.previous = parent.hash;
    child.payload = encode_snapshot(child_snapshot);
    child.hash = metadata_hash(child.generation, child.previous, child.payload);

    // This delta is well-formed and applicable to the cached parent, but it
    // reconstructs a different successor than the claimed child hash. A cache
    // hit for the parent must not turn that false history edge into authority.
    auto wrong_snapshot = parent_snapshot;
    wrong_snapshot.entries["/wrong-child"] = child_directory;
    const auto wrong_delta = metadata_delta(parent_snapshot, wrong_snapshot);
    REQUIRE(wrong_delta.has_value());

    MetadataHistoryEntry corrupt;
    corrupt.generation = child.generation;
    corrupt.previous = parent.hash;
    corrupt.hash = child.hash;
    corrupt.previous_known = true;
    corrupt.merge_parents = child_snapshot.merge_parents;
    corrupt.body = MetadataHistoryEntry::Body::delta;
    corrupt.payload = encode_metadata_delta(*wrong_delta);

    CHECK(!replica.import_history(corrupt));
    CHECK(!replica.history_contains(child.hash));
    CHECK(replica.materialized(child.hash) == nullptr);
    CHECK(replica.diagnostics().materialization_cache_entries == cache_after_parent);
    CHECK(!replica.accept_commit({child.generation, child.hash, 1, {random_node_id()}}));

    auto valid = corrupt;
    valid.payload = encode_metadata_delta(*child_delta);
    REQUIRE(replica.import_history(valid));
    auto materialized_child = replica.materialized(child.hash);
    REQUIRE(materialized_child != nullptr);
    CHECK(materialized_child->record.payload == child.payload);
    CHECK(encode_snapshot(*materialized_child->snapshot) == child.payload);

    // A later corrupt duplicate with the same claimed identity is rejected
    // before the existing valid history/cache entry is consulted as success.
    CHECK(!replica.import_history(corrupt));
    auto after_duplicate = replica.materialized(child.hash);
    REQUIRE(after_duplicate != nullptr);
    CHECK(after_duplicate == materialized_child);
    CHECK(after_duplicate->record.payload == child.payload);
}

namespace {
MetadataSnapshot dlt7_base_snapshot() {
    auto base = decode_snapshot(genesis_metadata().payload);
    base.metadata_voters.clear();
    base.metadata_write_replicas_required = 1;
    base.retention_baseline_complete = true;
    return base;
}

GarbageRef tombstone(uint8_t salt) {
    return GarbageRef{object_id(pattern(64, salt)), 1000 + salt, random_node_id()};
}
} // namespace

MACHA_FAST_TEST("storage_metadata", test_metadata_dlt7_presence_flags_round_trip) {
    // DLT7 carries a presence flag per topology set (and one for canonical
    // tombstone order). Each combination must replay to exactly the target
    // snapshot bytes, and the plain DLT5 case must stay DLT5.
    auto parent = dlt7_base_snapshot();
    MetadataConflict standing;
    standing.kind = MetadataConflictKind::namespace_entry;
    standing.key = "/standing";
    standing.left_head = sha256(pattern(51));
    standing.right_head = sha256(pattern(52));
    parent.conflicts.emplace(metadata_conflict_id(standing), standing);
    parent.merge_parents = {sha256(pattern(53))};
    // Append order, made non-canonical by reversing a sort.
    parent.garbage = {tombstone(3), tombstone(1), tombstone(2)};
    canonicalise_garbage(parent.garbage);
    std::reverse(parent.garbage.begin(), parent.garbage.end());
    REQUIRE(!garbage_is_canonical(parent.garbage));

    const auto round_trip = [&](const MetadataSnapshot& after, char expected_version) {
        auto delta = metadata_delta(parent, after);
        REQUIRE(delta.has_value());
        const auto encoded = encode_metadata_delta(*delta);
        REQUIRE(encoded.size() >= 8);
        if (encoded[7] != expected_version)
            std::cout << "dlt7 round trip: got DLT" << encoded[7] << " expected DLT"
                      << expected_version << " parents=" << delta->replace_merge_parents.has_value()
                      << " conflicts=" << delta->replace_conflicts.has_value()
                      << " canonical=" << delta->canonical_garbage << '\n';
        CHECK(encoded[7] == expected_version);
        const auto decoded = decode_metadata_delta(encoded);
        CHECK(decoded.replace_merge_parents.has_value() == delta->replace_merge_parents.has_value());
        CHECK(decoded.replace_conflicts.has_value() == delta->replace_conflicts.has_value());
        CHECK(decoded.canonical_garbage == delta->canonical_garbage);
        CHECK(encode_snapshot(apply_metadata_delta(parent, decoded)) == encode_snapshot(after));
        return encoded.size();
    };

    // Nothing topological, tombstones untouched: DLT5.
    auto plain = parent;
    plain.mutation_sequences[random_node_id()] = 1;
    round_trip(plain, '5');

    // Only merge_parents changed.
    auto parents_only = parent;
    parents_only.merge_parents.clear();
    const auto parents_bytes = round_trip(parents_only, '7');
    CHECK(parents_bytes < 512);

    // Only conflicts changed.
    auto conflicts_only = parent;
    conflicts_only.conflicts.clear();
    round_trip(conflicts_only, '7');

    // Both changed: still DLT6.
    auto both = parent;
    both.merge_parents.clear();
    both.conflicts.clear();
    round_trip(both, '6');

    // Canonical tombstone order alone (a reconciliation's union over an
    // append-ordered primary parent) is expressible as a delta.
    auto canonical = parent;
    canonicalise_garbage(canonical.garbage);
    REQUIRE(garbage_is_canonical(canonical.garbage));
    auto delta = metadata_delta(parent, canonical);
    REQUIRE(delta.has_value());
    CHECK(delta->canonical_garbage);
    CHECK(delta->upsert_garbage.empty());
    CHECK(delta->erase_garbage.empty());
    round_trip(canonical, '7');

    // Canonical order plus a new tombstone and an erased one.
    auto edited = canonical;
    edited.garbage.erase(edited.garbage.begin());
    edited.garbage.push_back(tombstone(0));
    canonicalise_garbage(edited.garbage);
    round_trip(edited, '7');

    // A hand-built DLT6 body with both lists still decodes to both sets.
    MetadataDelta six;
    six.replace_merge_parents = std::vector<Hash256>{};
    six.replace_conflicts = std::map<std::string, MetadataConflict, std::less<>>{};
    const auto six_encoded = encode_metadata_delta(six);
    CHECK(six_encoded[7] == '6');
    const auto six_decoded = decode_metadata_delta(six_encoded);
    CHECK(six_decoded.replace_merge_parents.has_value());
    CHECK(six_decoded.replace_conflicts.has_value());
    CHECK(!six_decoded.canonical_garbage);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_dlt8_append_extents_delta) {
    // A large file publishes one quantum at a time: DLT8 carries only the
    // appended extents and new attributes, not the whole extent table.
    auto before = dlt7_base_snapshot();
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.version = 3;
    file.size = 3000;
    for (uint8_t i = 0; i < 3; ++i)
        file.extents.push_back({i * 1000ULL, 1000, object_id(pattern(32, i)), false});
    before.entries["/big.mkv"] = file;

    auto after = before;
    auto& grown = after.entries["/big.mkv"];
    for (uint8_t i = 3; i < 5; ++i)
        grown.extents.push_back({i * 1000ULL, 1000, object_id(pattern(32, i)), false});
    grown.size = 5000;
    grown.version = 4;
    grown.mtime_ns = 777;
    grown.ctime_ns = 778;

    auto delta = metadata_delta(before, after);
    REQUIRE(delta.has_value());
    CHECK(delta->upsert_entries.empty());
    REQUIRE(delta->append_entries.contains("/big.mkv"));
    CHECK(delta->append_entries.at("/big.mkv").base_extents == 3);
    CHECK(delta->append_entries.at("/big.mkv").extents.size() == 2);
    const auto encoded = encode_metadata_delta(*delta);
    REQUIRE(encoded.size() >= 8);
    CHECK(encoded[7] == '8');
    CHECK(encoded.size() < 256);
    const auto decoded = decode_metadata_delta(encoded);
    CHECK(decoded.append_entries.size() == 1);
    CHECK(encode_snapshot(apply_metadata_delta(before, decoded)) == encode_snapshot(after));

    // The same edit through the exact-delta helper.
    MetadataDelta exact;
    record_entry_change(exact, "/big.mkv", &before.entries.at("/big.mkv"), grown);
    CHECK(exact.upsert_entries.empty());
    CHECK(exact.append_entries.size() == 1);
    CHECK(encode_snapshot(apply_metadata_delta(before, exact)) == encode_snapshot(after));

    // A base that does not match (the file changed underneath) refuses to
    // replay rather than producing a wrong table.
    auto other = before;
    other.entries["/big.mkv"].extents.pop_back();
    bool refused = false;
    try {
        (void)apply_metadata_delta(other, decoded);
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);

    // A rewrite (not an append) still goes as a whole entry, DLT5.
    auto rewritten = before;
    rewritten.entries["/big.mkv"].extents[0].id = object_id(pattern(32, 99));
    auto rewrite_delta = metadata_delta(before, rewritten);
    REQUIRE(rewrite_delta.has_value());
    CHECK(rewrite_delta->append_entries.empty());
    CHECK(rewrite_delta->upsert_entries.contains("/big.mkv"));
    CHECK(encode_metadata_delta(*rewrite_delta)[7] == '5');
}

MACHA_FAST_TEST("storage_metadata", test_metadata_merge_over_append_ordered_tombstones_is_a_delta) {
    // A merge canonicalises the tombstone union by ObjectId while the primary
    // parent is in append order; that reorder must still go as a delta.
    auto base = dlt7_base_snapshot();
    base.garbage = {tombstone(9), tombstone(4), tombstone(7)};
    canonicalise_garbage(base.garbage);
    std::reverse(base.garbage.begin(), base.garbage.end());
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    auto left = base;
    Mutation(left, 1).put("/left", directory);
    left.garbage.push_back(tombstone(2));
    auto right = base;
    Mutation(right, 2).put("/right", directory);
    right.garbage.push_back(tombstone(5));
    REQUIRE(!garbage_is_canonical(left.garbage));

    auto merged = merge_metadata_heads(left, right, sha256(pattern(61)), sha256(pattern(62)));
    CHECK(garbage_is_canonical(merged.snapshot.garbage));
    CHECK(merged.snapshot.garbage.size() == 5);
    merged.snapshot.merge_parents = {sha256(pattern(62))};

    auto delta = metadata_delta(left, merged.snapshot);
    REQUIRE(delta.has_value());
    CHECK(delta->canonical_garbage);
    const auto encoded = encode_metadata_delta(*delta);
    CHECK(encoded[7] == '7');
    CHECK(encoded.size() < encode_snapshot(merged.snapshot).size());
    CHECK(encode_snapshot(apply_metadata_delta(left, decode_metadata_delta(encoded))) ==
          encode_snapshot(merged.snapshot));
}

MACHA_FAST_TEST("storage_metadata", test_metadata_superseded_conflicts_leave_the_snapshot) {
    // A conflict whose subject a later mutation rewrote is decided: the later
    // write is the resolution, and the record leaves the snapshot.
    auto base = dlt7_base_snapshot();
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.size = 10;
    file.version = 1;
    {
        Mutation mutation(base, 9);
        mutation.put("/song.mp3", file);
        mutation.catalogue(object_id(pattern(70)));
    }

    // Two authors change the file and the catalogue root, neither having
    // seen the other.
    const auto change = [&](uint8_t author, uint64_t size, int64_t mtime, uint8_t root) {
        auto head = base;
        auto entry = head.entries.at("/song.mp3");
        entry.size = size;
        entry.version = 2;
        entry.mtime_ns = mtime;
        Mutation mutation(head, author);
        mutation.put("/song.mp3", entry);
        mutation.catalogue(object_id(pattern(root)));
        return head;
    };
    const auto left = change(1, 20, 100, 71);
    const auto right = change(2, 30, 200, 72);

    auto merged = merge_metadata_heads(left, right, sha256(pattern(81)), sha256(pattern(82)));
    REQUIRE(merged.conflicts_created == 2);
    CHECK(merged.conflicts_superseded == 0);
    REQUIRE(merged.snapshot.conflicts.size() == 2);
    // The later file is in place, and one of the two catalogue roots.
    CHECK(merged.snapshot.entries.at("/song.mp3") == right.entries.at("/song.mp3"));
    CHECK((merged.snapshot.catalogue_root == left.catalogue_root ||
           merged.snapshot.catalogue_root == right.catalogue_root));

    // Nothing decided yet: pruning is a no-op.
    auto untouched = merged.snapshot;
    CHECK(prune_superseded_conflicts(untouched) == 0);
    CHECK(untouched.conflicts.size() == 2);

    // A later write to the path decides the namespace conflict only.
    auto rewritten = merged.snapshot;
    {
        auto entry = rewritten.entries.at("/song.mp3");
        entry.size = 40;
        entry.version = 3;
        Mutation(rewritten, 3).put("/song.mp3", entry);
    }
    CHECK(prune_superseded_conflicts(rewritten) == 1);
    REQUIRE(rewritten.conflicts.size() == 1);
    CHECK(rewritten.conflicts.begin()->second.kind == MetadataConflictKind::catalogue_root);

    // Removing the path decides it too; a new catalogue root decides the other.
    auto removed = merged.snapshot;
    {
        Mutation mutation(removed, 3);
        mutation.erase("/song.mp3");
        mutation.catalogue(object_id(pattern(73)));
    }
    CHECK(prune_superseded_conflicts(removed) == 2);
    CHECK(removed.conflicts.empty());

    // A head that has not seen the decision still carries the record: the
    // merge with it keeps the decision and drops the record.
    auto undecided = merged.snapshot;
    Mutation(undecided, 4).put("/another", file);
    const auto later = merge_metadata_heads(rewritten, undecided, sha256(pattern(83)),
                                            sha256(pattern(84)));
    CHECK(later.conflicts_created == 0);
    CHECK(later.conflicts_superseded == 1);
    CHECK(later.snapshot.conflicts.size() == 1);
    CHECK(later.snapshot.entries.at("/song.mp3").size == 40);
    CHECK(later.snapshot.entries.contains("/another"));

    // A record with a base and nothing installed, whose alternatives hold the
    // same bytes (duplicate media), settles at pruning.
    FsEntry same = file;
    same.extents.push_back({0, 10, object_id(pattern(90)), false});
    same.version = 4;
    same.mtime_ns = 100;
    MetadataConflict legacy;
    legacy.kind = MetadataConflictKind::namespace_entry;
    legacy.key = "/dup.mkv";
    legacy.left_head = sha256(pattern(85));
    legacy.right_head = sha256(pattern(86));
    legacy.left_entry = same;
    same.version = 7;
    same.mtime_ns = 200;
    legacy.right_entry = same;
    auto with_legacy = dlt7_base_snapshot();
    with_legacy.conflicts.emplace(metadata_conflict_id(legacy), legacy);
    CHECK(prune_superseded_conflicts(with_legacy) == 1);
    CHECK(with_legacy.conflicts.empty());
    REQUIRE(with_legacy.entries.contains("/dup.mkv"));
    CHECK(same_content(with_legacy.entries.at("/dup.mkv"), *legacy.left_entry));
}

MACHA_FAST_TEST("storage_metadata", test_metadata_merge_delta_preserves_standing_conflicts) {
    // Both parents of a reconciliation carry the same unresolved conflict. DLT6
    // cannot say "unchanged": an empty list replays as "replace with nothing",
    // so the delta must not drop the standing conflict.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);

    const auto genesis = genesis_metadata();
    auto base = decode_snapshot(genesis.payload);
    base.metadata_voters.clear();
    base.metadata_write_replicas_required = 1;
    base.retention_baseline_complete = true;
    MetadataConflict standing;
    standing.kind = MetadataConflictKind::namespace_entry;
    standing.key = "/standing";
    standing.left_head = sha256(pattern(31));
    standing.right_head = sha256(pattern(32));
    const auto standing_id = metadata_conflict_id(standing);
    base.conflicts.emplace(standing_id, standing);

    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    auto left = base;
    Mutation(left, 1).put("/left", directory);
    auto right = base;
    Mutation(right, 2).put("/right", directory);

    MetadataRecord base_record;
    base_record.generation = genesis.generation + 1;
    base_record.previous = genesis.hash;
    base_record.payload = encode_snapshot(base);
    base_record.hash =
        metadata_hash(base_record.generation, base_record.previous, base_record.payload);
    auto make_child = [&](const MetadataSnapshot& snapshot) {
        MetadataRecord record;
        record.generation = base_record.generation + 1;
        record.previous = base_record.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    const auto left_record = make_child(left);
    const auto right_record = make_child(right);
    REQUIRE(left_record.hash != right_record.hash);

    auto merged = merge_metadata_heads(left, right, left_record.hash, right_record.hash);
    CHECK(merged.conflicts_created == 0);
    REQUIRE(merged.snapshot.conflicts.contains(standing_id));
    merged.snapshot.merge_parents = {right_record.hash};

    MetadataRecord merge;
    merge.generation = left_record.generation + 1;
    merge.previous = left_record.hash;
    merge.payload = encode_snapshot(merged.snapshot);
    merge.hash = metadata_hash(merge.generation, merge.previous, merge.payload);

    auto delta = metadata_delta(left, merged.snapshot);
    REQUIRE(delta.has_value());
    const auto encoded = encode_metadata_delta(*delta);
    REQUIRE(encoded.size() >= 8);
    // DLT7: of the two topology sets only merge_parents changed, so the
    // standing conflict set is not carried.
    CHECK(encoded[7] == '7');
    CHECK(delta->replace_merge_parents.has_value());
    CHECK(!delta->replace_conflicts.has_value());
    CHECK(encoded.size() < 1024);
    const auto replayed = apply_metadata_delta(left, decode_metadata_delta(encoded));
    CHECK(replayed.conflicts.contains(standing_id));
    CHECK(encode_snapshot(replayed) == merge.payload);

    // The replica's exact-reconstruction check accepts the merge as a delta.
    MetadataReplica replica(t.path() / "standing-conflict", keys.storage);
    REQUIRE(replica.store_commit(base_record));
    REQUIRE(replica.store_commit(left_record));
    REQUIRE(replica.store_commit(right_record));
    REQUIRE(replica.store_commit(merge, encoded));
    const auto stored = replica.history_entry(merge.hash);
    REQUIRE(stored.has_value());
    CHECK(stored->body == MetadataHistoryEntry::Body::delta);
    REQUIRE(replica.historical(merge.hash).has_value());
    CHECK(replica.historical(merge.hash)->payload == merge.payload);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_delta_child_of_merge_commit_reconstructs) {
    // The first mutation after a reconciliation clears merge_parents. DLT6
    // expresses that, and with a standing conflict on the merge commit the delta
    // carries the conflict set too.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto origin = random_node_id();

    const auto genesis = genesis_metadata();
    auto parent = decode_snapshot(genesis.payload);
    parent.metadata_voters.clear();
    parent.metadata_write_replicas_required = 1;
    parent.retention_baseline_complete = true;
    MetadataConflict standing;
    standing.kind = MetadataConflictKind::namespace_entry;
    standing.key = "/standing";
    standing.left_head = sha256(pattern(41));
    standing.right_head = sha256(pattern(42));
    const auto standing_id = metadata_conflict_id(standing);
    parent.conflicts.emplace(standing_id, standing);
    parent.merge_parents = {sha256(pattern(77))};

    MetadataRecord parent_record;
    parent_record.generation = genesis.generation + 1;
    parent_record.previous = genesis.hash;
    parent_record.payload = encode_snapshot(parent);
    parent_record.hash = metadata_hash(parent_record.generation, parent_record.previous,
                                       parent_record.payload);

    auto child = parent;
    child.merge_parents.clear();
    child.mutation_sequences[origin] = 1;
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.version = 1;
    child.entries["/after-merge"] = file;

    MetadataRecord child_record;
    child_record.generation = parent_record.generation + 1;
    child_record.previous = parent_record.hash;
    child_record.payload = encode_snapshot(child);
    child_record.hash =
        metadata_hash(child_record.generation, child_record.previous, child_record.payload);

    auto delta = metadata_delta(parent, child);
    REQUIRE(delta.has_value());
    const auto encoded = encode_metadata_delta(*delta);
    REQUIRE(encoded.size() >= 8);
    CHECK(encoded[7] == '7');
    CHECK(!delta->replace_conflicts.has_value());
    const auto replayed = apply_metadata_delta(parent, decode_metadata_delta(encoded));
    CHECK(replayed.merge_parents.empty());
    CHECK(replayed.conflicts.contains(standing_id));
    CHECK(encode_snapshot(replayed) == child_record.payload);

    const auto path = t.path() / "delta-after-merge";
    {
        MetadataReplica replica(path, keys.storage);
        REQUIRE(replica.store_commit(parent_record));
        REQUIRE(replica.store_commit(child_record, encoded));
        const auto stored = replica.history_entry(child_record.hash);
        REQUIRE(stored.has_value());
        CHECK(stored->body == MetadataHistoryEntry::Body::delta);
    }
    // Cold replay walks the delta over the merge-commit anchor and checks the
    // reconstructed merge_parents against the indexed ones.
    MetadataReplica reopened(path, keys.storage);
    REQUIRE(reopened.historical(child_record.hash).has_value());
    CHECK(reopened.historical(child_record.hash)->payload == child_record.payload);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_delta_tombstone_edits_are_linear) {
    // Tombstone edits apply through an index: the same result as a naive
    // reference on a small input, and bounded time on a large one.
    auto make_id = [](uint64_t n) {
        ObjectId id{};
        for (int b = 0; b < 8; ++b)
            id.bytes[static_cast<size_t>(b)] = static_cast<uint8_t>((n >> (8 * b)) & 0xff);
        return id;
    };
    auto tombstone = [&](uint64_t n, int64_t retired) {
        GarbageRef ref;
        ref.id = make_id(n);
        ref.retired_at_ns = retired;
        return ref;
    };
    auto naive_apply = [](MetadataSnapshot& out, const MetadataDelta& delta) {
        for (const auto& id : delta.erase_garbage)
            std::erase_if(out.garbage, [&](const GarbageRef& g) { return g.id == id; });
        for (const auto& garbage : delta.upsert_garbage) {
            auto it = std::find_if(out.garbage.begin(), out.garbage.end(),
                                   [&](const GarbageRef& v) { return v.id == garbage.id; });
            if (it == out.garbage.end())
                out.garbage.push_back(garbage);
            else
                *it = garbage;
        }
    };
    auto build = [&](uint64_t count, uint64_t edits, MetadataSnapshot& before,
                     MetadataDelta& delta) {
        before = decode_snapshot(genesis_metadata().payload);
        before.garbage.reserve(count);
        for (uint64_t i = 0; i < count; ++i)
            before.garbage.push_back(tombstone(i, static_cast<int64_t>(i) + 1));
        // A duplicate id, so "erase removes every copy" is exercised too.
        before.garbage.push_back(tombstone(7, 700));
        for (uint64_t i = 0; i < edits; ++i) {
            delta.erase_garbage.push_back(make_id(i * 10 + 7));          // existing, incl. id 7
            delta.upsert_garbage.push_back(tombstone(i * 10 + 3, -1));    // replace in place
            delta.upsert_garbage.push_back(tombstone(count + i, 5));      // append
        }
    };

    {
        MetadataSnapshot before;
        MetadataDelta delta;
        build(2000, 60, before, delta);
        auto expected = before;
        naive_apply(expected, delta);
        const auto actual = apply_metadata_delta(before, delta);
        REQUIRE(actual.garbage.size() == expected.garbage.size());
        CHECK(actual.garbage == expected.garbage);
    }

    {
        constexpr uint64_t count = 200000;
        constexpr uint64_t edits = 20000;
        MetadataSnapshot before;
        MetadataDelta delta;
        build(count, edits, before, delta);
        const auto started = std::chrono::steady_clock::now();
        const auto after = apply_metadata_delta(before, delta);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        // A linear scan would need ~10^10 comparisons here; 10 s is generous on a
        // loaded host.
        CHECK(elapsed < scaled(10s));
        CHECK(after.garbage.size() == count + 1 - (edits + 1) + edits);
        CHECK(std::none_of(after.garbage.begin(), after.garbage.end(),
                           [&](const GarbageRef& g) { return g.id == make_id(7); }));
        // Retained tombstones keep their relative order (ids were inserted in
        // index order, so decoded indexes must still ascend); appended ones
        // follow in delta order.
        auto index_of = [](const ObjectId& id) {
            uint64_t n = 0;
            for (int b = 7; b >= 0; --b)
                n = (n << 8) | id.bytes[static_cast<size_t>(b)];
            return n;
        };
        REQUIRE(after.garbage.size() > edits);
        size_t order_violations = 0;
        for (uint64_t i = 1; i + edits < after.garbage.size(); ++i)
            if (index_of(after.garbage[i - 1].id) >= index_of(after.garbage[i].id))
                ++order_violations;
        CHECK(order_violations == 0);
        for (uint64_t i = 0; i < edits; ++i)
            CHECK(after.garbage[after.garbage.size() - edits + i].id == make_id(count + i));
        CHECK(after.garbage[3].retired_at_ns == -1);
    }
}

// An entry's provenance survives the snapshot and delta encodings, an entry
// without any encodes as it always has, and a flagged entry with no
// provenance behind the flag is refused.
MACHA_FAST_TEST("storage_metadata", test_an_entrys_provenance_survives_snapshot_and_delta) {
    const auto genesis = genesis_metadata();
    auto plain = decode_snapshot(genesis.payload);
    FsEntry file;
    file.type = EntryType::file;
    file.size = 123;
    plain.entries["/a"] = file;
    plain.entries["/b"] = file;
    const auto plain_bytes = encode_snapshot(plain);

    auto stamped = plain;
    NodeId author{};
    author.bytes[3] = 9;
    auto& entry = stamped.entries["/a"];
    entry.provenance.file_id = legacy_file_id("/a");
    entry.provenance.content = {author, 4};
    entry.provenance.name = {author, 2};
    const auto bytes = encode_snapshot(stamped);
    CHECK(bytes.size() == plain_bytes.size() + 16 + 24 + 24);
    const auto decoded = decode_snapshot(bytes);
    CHECK(decoded.entries.at("/a") == entry);
    CHECK(decoded.entries.at("/b").provenance.empty());
    CHECK(decode_snapshot(plain_bytes).entries.at("/a").provenance.empty());

    const auto delta = metadata_delta(plain, stamped);
    REQUIRE(delta.has_value());
    CHECK(delta->upsert_entries.at("/a") == entry);
    const auto carried = decode_metadata_delta(encode_metadata_delta(*delta));
    CHECK(carried.upsert_entries.at("/a").provenance == entry.provenance);

    CHECK(legacy_file_id("/a") == legacy_file_id("/a"));
    CHECK(legacy_file_id("/a") != legacy_file_id("/b"));
    CHECK(legacy_file_id("/a") != NodeId{});
}

// The rules by which a mutation stamps what it writes.
MACHA_FAST_TEST("storage_metadata", test_a_written_entry_is_stamped_against_what_was_there) {
    NodeId author{};
    author.bytes[0] = 1;
    const MetadataDot dot{author, 7};
    FsEntry file;
    file.type = EntryType::file;
    file.size = 10;

    // Nothing at the path: a new file, with an identity of its own.
    auto created = file;
    stamp_entry_provenance(created, "/new", nullptr, dot);
    CHECK(created.provenance.file_id != NodeId{});
    CHECK(created.provenance.file_id != legacy_file_id("/new"));
    CHECK(created.provenance.content == dot);
    CHECK(created.provenance.name == dot);

    // Changed in place: same identity and name, new content dot.
    const MetadataDot later{author, 9};
    auto changed = created;
    changed.size = 11;
    changed.provenance = {};
    stamp_entry_provenance(changed, "/new", &created, later);
    CHECK(changed.provenance.file_id == created.provenance.file_id);
    CHECK(changed.provenance.name == dot);
    CHECK(changed.provenance.content == later);

    // Rewritten unchanged: exactly what it was.
    auto same = created;
    same.provenance = {};
    stamp_entry_provenance(same, "/new", &created, later);
    CHECK(same == created);

    // Arriving from another path with its identity: a rename. The content
    // dot comes with it and the name dot is this mutation's.
    auto renamed = created;
    stamp_entry_provenance(renamed, "/elsewhere", nullptr, later);
    CHECK(renamed.provenance.file_id == created.provenance.file_id);
    CHECK(renamed.provenance.content == dot);
    CHECK(renamed.provenance.name == later);

    // A rename onto another file's path is new at that path.
    FsEntry other = file;
    stamp_entry_provenance(other, "/other", nullptr, dot);
    auto replacing = created;
    stamp_entry_provenance(replacing, "/other", &other, later);
    CHECK(replacing.provenance.file_id == created.provenance.file_id);
    CHECK(replacing.provenance.name == later);
    CHECK(replacing.provenance.content == dot);

    // An entry from before provenance: untouched while unchanged, and on its
    // first change it takes the identity of its path, a content dot and no
    // name dot.
    const FsEntry legacy = file;
    auto untouched = legacy;
    stamp_entry_provenance(untouched, "/old", &legacy, dot);
    CHECK(untouched.provenance.empty());
    auto touched = legacy;
    touched.mode = 0600;
    stamp_entry_provenance(touched, "/old", &legacy, dot);
    CHECK(touched.provenance.file_id == legacy_file_id("/old"));
    CHECK(touched.provenance.content == dot);
    CHECK(!static_cast<bool>(touched.provenance.name));

    // An append gives the entry its dot, and a legacy entry its identity.
    MetadataDelta::EntryAppend append;
    append.base_extents = 0;
    append.extents.push_back({0, 5, object_id(pattern(5, 1)), false});
    append.size = 5;
    append.content = later;
    auto appended = legacy;
    appended.extents.clear();
    apply_entry_append(appended, "/old", append);
    CHECK(appended.provenance.content == later);
    CHECK(appended.provenance.file_id == legacy_file_id("/old"));
    CHECK(appended.extents.size() == 1);
    bool refused = false;
    try {
        apply_entry_append(appended, "/old", append);
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
}

// The legacy clock and an append's dot travel in a delta and in both
// snapshot forms.
MACHA_FAST_TEST("storage_metadata", test_the_legacy_clock_survives_snapshot_and_delta) {
    const auto genesis = genesis_metadata();
    auto before = decode_snapshot(genesis.payload);
    NodeId author{};
    author.bytes[0] = 2;
    before.mutation_sequences[author] = 3;
    FsEntry file;
    file.type = EntryType::file;
    file.extents.push_back({0, 4, object_id(pattern(4, 1)), false});
    file.size = 4;
    before.entries["/f"] = file;
    CHECK(!decode_snapshot(encode_snapshot(before)).legacy_clock.has_value());

    auto after = before;
    after.legacy_clock = before.mutation_sequences;
    after.mutation_sequences[author] = 4;
    auto& grown = after.entries["/f"];
    grown.extents.push_back({4, 4, object_id(pattern(4, 2)), false});
    grown.size = 8;
    grown.provenance.content = {author, 4};
    grown.provenance.file_id = legacy_file_id("/f");

    const auto decoded = decode_snapshot(encode_snapshot(after));
    REQUIRE(decoded.legacy_clock.has_value());
    CHECK(*decoded.legacy_clock == before.mutation_sequences);
    // Set but empty is not unset.
    auto empty_clock = before;
    empty_clock.legacy_clock.emplace();
    const auto empty_decoded = decode_snapshot(encode_snapshot(empty_clock));
    REQUIRE(empty_decoded.legacy_clock.has_value());
    CHECK(empty_decoded.legacy_clock->empty());

    const auto delta = metadata_delta(before, after);
    REQUIRE(delta.has_value());
    REQUIRE(delta->set_legacy_clock.has_value());
    REQUIRE(delta->append_entries.contains("/f"));
    CHECK((delta->append_entries.at("/f").content == MetadataDot{author, 4}));
    const auto carried = decode_metadata_delta(encode_metadata_delta(*delta));
    CHECK(carried.set_legacy_clock == delta->set_legacy_clock);
    CHECK(carried.append_entries.at("/f").content == delta->append_entries.at("/f").content);
    CHECK(encode_snapshot(apply_metadata_delta(before, carried)) == encode_snapshot(after));

    // The clock is set once: a delta cannot change it.
    auto moved = after;
    (*moved.legacy_clock)[author] = 99;
    CHECK(!metadata_delta(after, moved).has_value());
}

MACHA_FAST_TEST("storage_metadata", test_protocol20_state_rejects_legacy_authority_mutators) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    MetadataReplica replica(t.path() / "legacy-backdoor", keys.storage);
    const auto genesis = genesis_metadata();
    auto snapshot = decode_snapshot(genesis.payload);
    snapshot.metadata_write_replicas_required = 2;
    MetadataRecord established;
    established.generation = genesis.generation + 1;
    established.previous = genesis.hash;
    established.payload = encode_snapshot(snapshot);
    established.hash =
        metadata_hash(established.generation, established.previous, established.payload);
    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    REQUIRE(replica.store_commit(established));
    REQUIRE(replica.accept_commit({established.generation, established.hash, 2, {a, b}}));

    auto child_snapshot = snapshot;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    child_snapshot.entries["/must-not-seed"] = directory;
    MetadataRecord child;
    child.generation = established.generation + 1;
    child.previous = established.hash;
    child.payload = encode_snapshot(child_snapshot);
    child.hash = metadata_hash(child.generation, child.previous, child.payload);

    CHECK(!replica.seed(child));
    CHECK(!replica.remember_committed(child));
    MetadataRecord observed;
    CHECK(!replica.cas(established.generation, established.hash, child.payload, &observed));
    CHECK(replica.committed().hash == established.hash);
    auto heads = replica.accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == established.hash);
}

MACHA_TEST("storage_metadata", test_local_store) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "store";
    auto plain = pattern(1024 * 1024 + 37);
    auto id = object_id(plain);
    uint64_t accounted_used = 0;

    {
        LocalStore store(root, 64 * 1024 * 1024, keys.storage);
        REQUIRE(store.put(id, plain));
        CHECK(store.has(id));
        REQUIRE(store.get(id).has_value());
        CHECK(*store.get(id) == plain);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        accounted_used = store.used();
        CHECK(accounted_used > plain.size());

        bool found_plain = false;
        for (auto& file : std::filesystem::recursive_directory_iterator(root / "objects")) {
            if (!file.is_regular_file())
                continue;
            std::ifstream in(file.path(), std::ios::binary);
            Bytes disk(std::istreambuf_iterator<char>(in), {});
            auto needle = std::span<const uint8_t>(plain).subspan(100, 128);
            found_plain =
                std::search(disk.begin(), disk.end(), needle.begin(), needle.end()) != disk.end();
        }
        CHECK(!found_plain);

        const auto reaffirm_before = store.diagnostics();
        REQUIRE(store.put(id, plain));
        const auto reaffirm_after = store.diagnostics();
        CHECK(reaffirm_after.loose_reaffirmation_fast_paths ==
              reaffirm_before.loose_reaffirmation_fast_paths + 1);
        CHECK(reaffirm_after.loose_reaffirmation_full_validations ==
              reaffirm_before.loose_reaffirmation_full_validations);

        // A successful PUT acknowledgement means the named immutable replica
        // contains the requested bytes, not merely that its pathname exists.
        corrupt_object(root, id);
        const auto repair_before = store.diagnostics();
        REQUIRE(store.put(id, plain));
        const auto repair_after = store.diagnostics();
        CHECK(repair_after.loose_reaffirmation_full_validations ==
              repair_before.loose_reaffirmation_full_validations + 1);
        auto repaired = store.get(id);
        REQUIRE(repaired.has_value());
        CHECK(*repaired == plain);
    }

    // A clean restart restores exact accounting from the small journal without
    // starting an O(number-of-objects) tree scan.
    CHECK(std::filesystem::exists(root / ".macha.accounting"));
    CHECK(std::filesystem::file_size(root / ".macha.accounting") >= 256);
    {
        LocalStore reopened(root, 64 * 1024 * 1024, keys.storage);
        CHECK(reopened.scan_complete());
        CHECK(reopened.used() == accounted_used);
        REQUIRE(reopened.get(id).has_value());
        CHECK(*reopened.get(id) == plain);
    }

    // Corrupt or missing state falls back to one full reconciliation, then writes
    // a trusted checkpoint again.
    {
        std::ofstream out(root / ".macha.accounting", std::ios::binary | std::ios::trunc);
        Bytes junk(256, 0x5a);
        out.write(reinterpret_cast<const char*>(junk.data()),
                  static_cast<std::streamsize>(junk.size()));
    }
    {
        LocalStore recovered(root, 64 * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return recovered.scan_complete(); }));
        CHECK(recovered.used() == accounted_used);
        REQUIRE(recovered.get(id).has_value());
    }
    {
        LocalStore recovered_restart(root, 64 * 1024 * 1024, keys.storage);
        CHECK(recovered_restart.scan_complete());
        CHECK(recovered_restart.used() == accounted_used);
    }

    {
        StorageLock first(t.path() / "locked");
        bool rejected = false;
        try {
            StorageLock second(t.path() / "locked");
        } catch (...) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

MACHA_TEST("storage_metadata", test_storage_pool_and_persistent_cache) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto state = t.path() / "state";
    auto disk1 = t.path() / "disk1";
    auto disk2 = t.path() / "disk2";
    auto disk3 = t.path() / "disk3";
    std::filesystem::create_directories(disk1);
    std::filesystem::create_directories(disk2);
    std::filesystem::create_directories(disk3);

    auto node = load_or_create_node_id(state);
    StoragePool pool(state, node, {{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}},
                     keys.storage);
    CHECK(pool.online_backends() == 2);
    CHECK(pool.limit() == 128ULL * 1024 * 1024);

    std::vector<std::pair<ObjectId, Bytes>> objects;
    for (size_t i = 0; i < 32; ++i) {
        auto data = pattern(128 * 1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        objects.push_back({id, std::move(data)});
    }
    for (const auto& [id, data] : objects) {
        auto got = pool.get(id);
        REQUIRE(got.has_value());
        CHECK(*got == data);
    }

    // Maintenance traversal is resumable. A one-object slice must not rebuild
    // or consume the whole object namespace, and a complete pass eventually
    // visits every physical object without blocking foreground pool operations.
    {
        StoragePool::Cursor cursor;
        std::set<ObjectId> seen;
        bool complete = false;
        size_t calls = 0;
        while (!complete && calls++ < 256) {
            auto id = pool.next_object(cursor, complete);
            if (id)
                seen.insert(*id);
        }
        CHECK(complete);
        CHECK(seen.size() == objects.size());
    }
    {
        size_t slices = 0;
        bool complete = false;
        while (!complete && slices++ < 256) {
            auto step = pool.scrub_step(0, 1);
            CHECK(step.objects <= 1);
            complete = step.complete;
        }
        CHECK(complete);
        CHECK(slices > 1);
    }
    {
        auto yielded = pool.rebalance_step(0, 1, [] { return true; });
        CHECK(yielded.yielded);
        CHECK(yielded.objects == 0);
        CHECK(yielded.bytes == 0);
    }

    // Reachability GC is a separate bounded physical cursor. It preserves live
    // and recently-retired objects, ignores young uncommitted objects, and
    // removes an old orphan that has no committed reference or tombstone.
    auto put_gc_object = [&](uint8_t tag) {
        auto data = pattern(96 * 1024 + tag);
        data[0] ^= tag;
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        return std::pair<ObjectId, Bytes>{id, std::move(data)};
    };
    auto gc_live_object = put_gc_object(0x31);
    auto gc_protected_object = put_gc_object(0x32);
    auto gc_orphan_object = put_gc_object(0x33);
    auto gc_young_object = put_gc_object(0x34);
    const auto gc_live = gc_live_object.first;
    const auto gc_protected = gc_protected_object.first;
    const auto gc_orphan = gc_orphan_object.first;
    const auto gc_young = gc_young_object.first;
    auto age_object = [&](const ObjectId& id) {
        for (const auto& disk : {disk1, disk2}) {
            auto path = object_path(disk, id);
            if (std::filesystem::exists(path))
                std::filesystem::last_write_time(
                    path, std::filesystem::file_time_type::clock::now() - 48h);
        }
    };
    age_object(gc_live);
    age_object(gc_protected);
    age_object(gc_orphan);

    std::vector<ObjectId> gc_live_set{gc_live};
    std::vector<ObjectId> gc_protected_set{gc_protected};
    std::sort(gc_live_set.begin(), gc_live_set.end());
    std::sort(gc_protected_set.begin(), gc_protected_set.end());
    bool gc_complete = false;
    size_t gc_slices = 0;
    uint64_t gc_reclaimed = 0;
    while (!gc_complete && gc_slices++ < 256) {
        auto step = pool.gc_step(gc_live_set, gc_protected_set, 24h, 3);
        CHECK(step.objects <= 3);
        gc_reclaimed += step.bytes;
        gc_complete = step.complete;
    }
    CHECK(gc_complete);
    CHECK(gc_slices > 1);
    CHECK(gc_reclaimed > 0);
    CHECK(pool.has(gc_live));
    CHECK(pool.has(gc_protected));
    CHECK(!pool.has(gc_orphan));
    CHECK(pool.has(gc_young));
    auto gc_yielded = pool.gc_step(gc_live_set, gc_protected_set, 24h, 1, [] { return true; });
    CHECK(gc_yielded.yielded);
    CHECK(gc_yielded.objects == 0);

    // Re-putting an identical hash reaffirms its physical age, so a new
    // uncommitted write cannot reuse an old orphan about to be collected.
    auto gc_reaffirmed_object = put_gc_object(0x35);
    const auto gc_reaffirmed = gc_reaffirmed_object.first;
    age_object(gc_reaffirmed);
    CHECK(pool.older_than(gc_reaffirmed, 24h));
    REQUIRE(pool.put(gc_reaffirmed, gc_reaffirmed_object.second));
    CHECK(!pool.older_than(gc_reaffirmed, 24h));

    // Add a third disk live and migrate local placement without changing the
    // node identity or DHT replica accounting.
    pool.reconfigure(
        {{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}, {disk3, 64ULL * 1024 * 1024}});
    pool.refresh();
    CHECK(pool.online_backends() == 3);
    (void)pool.rebalance_once();
    size_t on_disk3 = 0;
    for (const auto& [id, _] : objects)
        on_disk3 += std::filesystem::exists(object_path(disk3, id)) ? 1 : 0;
    CHECK(on_disk3 > 0);

    // A temporary disappearance does not change placement weight. The node can
    // keep serving/falling back to surviving disks without remapping the whole
    // pool; the returning disk resumes its old share and rebalance converges.
    auto parked = t.path() / "disk2.offline";
    std::filesystem::rename(disk2, parked);
    pool.refresh();
    CHECK(pool.online_backends() == 2);
    CHECK(pool.limit() == 192ULL * 1024 * 1024);
    CHECK(load_or_create_node_id(state) == node);
    std::filesystem::rename(parked, disk2);
    pool.refresh();
    CHECK(pool.online_backends() == 3);
    (void)pool.rebalance_once();

    // Configuration removal and later re-addition of a backend is also live.
    pool.reconfigure({{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}});
    pool.refresh();
    CHECK(pool.online_backends() == 2);
    CHECK(pool.limit() == 128ULL * 1024 * 1024);
    pool.reconfigure(
        {{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}, {disk3, 64ULL * 1024 * 1024}});
    pool.refresh();
    CHECK(pool.online_backends() == 3);

    // Local authoritative placement uses the same capacity weighting. A backend
    // eight times larger should receive overwhelmingly more objects, without
    // using live free space as part of the score.
    auto weighted_state = t.path() / "weighted-state";
    auto small_disk = t.path() / "weighted-small";
    auto large_disk = t.path() / "weighted-large";
    std::filesystem::create_directories(small_disk);
    std::filesystem::create_directories(large_disk);
    auto weighted_node = load_or_create_node_id(weighted_state);
    StoragePool weighted(weighted_state, weighted_node,
                         {{small_disk, 8ULL * 1024 * 1024}, {large_disk, 64ULL * 1024 * 1024}},
                         keys.storage);
    size_t small_objects = 0, large_objects = 0;
    for (size_t i = 0; i < 512; ++i) {
        auto data = pattern(1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        data[1] ^= static_cast<uint8_t>(i >> 8U);
        auto id = object_id(data);
        REQUIRE(weighted.put(id, data));
        small_objects += std::filesystem::exists(object_path(small_disk, id)) ? 1 : 0;
        large_objects += std::filesystem::exists(object_path(large_disk, id)) ? 1 : 0;
    }
    CHECK(small_objects + large_objects == 512);
    CHECK(large_objects > small_objects * 5);

    auto cache_root = t.path() / "cache";
    MetadataRecord cached_metadata;
    {
        PersistentBlockCache cache({cache_root, 2, true}, keys.storage);
        auto a = pattern(8192);
        auto b = pattern(8193);
        auto c = pattern(8194);
        a[0] ^= 0x11;
        b[0] ^= 0x22;
        c[0] ^= 0x33;
        auto ia = object_id(a), ib = object_id(b), ic = object_id(c);
        REQUIRE(cache.put(ia, a));
        std::this_thread::sleep_for(2ms);
        REQUIRE(cache.put(ib, b));
        REQUIRE(cache.get(ia).has_value()); // a is now the hotter block.
        std::this_thread::sleep_for(2ms);
        REQUIRE(cache.put(ic, c));
        CHECK(cache.blocks() == 2);
        CHECK(cache.has(ia));
        CHECK(cache.has(ic));
        CHECK(!cache.has(ib));

        cached_metadata = genesis_metadata();
        cache.remember_metadata(cached_metadata);
        REQUIRE(cache.metadata().has_value());
        CHECK(cache.metadata()->hash == cached_metadata.hash);
    }
    {
        PersistentBlockCache reopened({cache_root, 2, true}, keys.storage);
        CHECK(reopened.blocks() == 2);
        REQUIRE(reopened.metadata().has_value());
        CHECK(reopened.metadata()->hash == cached_metadata.hash);
        reopened.reconfigure({cache_root, 0, true});
        CHECK(!reopened.enabled());
        CHECK(!reopened.metadata().has_value());
        reopened.reconfigure({cache_root, 2, true});
        CHECK(reopened.enabled());
        REQUIRE(reopened.metadata().has_value());
    }
}

MACHA_HEAVY_TEST("storage_metadata", test_metadata_codec_and_replica) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    MetadataSnapshot snap = decode_snapshot(genesis_metadata().payload);
    auto a = random_node_id();
    auto b = random_node_id();
    auto c = random_node_id();
    snap.metadata_voters = {a, b, c};
    snap.mutation_sequences[a] = 7;
    snap.mutation_sequences[b] = 11;
    FsEntry file;
    file.type = EntryType::file;
    file.size = 123;
    snap.entries["/movie.mkv"] = file;
    const std::string unicode_dir = "/Music/Caf\xc3\xa9 del Mar pack 1 (1999-2004)";
    const std::string unicode_file = unicode_dir + "/01.Clannad - Na Buachaill\xc3\xad lainn.mp3";
    FsEntry unicode_directory_entry;
    unicode_directory_entry.type = EntryType::directory;
    snap.entries[unicode_dir] = unicode_directory_entry;
    snap.entries[unicode_file] = file;
    auto garbage_id = object_id(pattern(4096));
    auto retirement_id = random_node_id();
    snap.garbage.push_back({garbage_id, 123456789, retirement_id});
    PersistedNodeStatus node_status;
    node_status.boot_id = random_node_id();
    node_status.observed_unix_ms = 1712345678901ULL;
    node_status.version = "0.18.2";
    node_status.host = "node-a.example";
    node_status.failure_domain = "rack-a";
    node_status.port = 57401;
    node_status.storage_capacity = 12ULL * 1024 * 1024 * 1024;
    node_status.storage_used = 7ULL * 1024 * 1024 * 1024;
    node_status.cache_capacity = 1024 * 1024;
    node_status.cache_used = 512 * 1024;
    node_status.metadata_generation = 44;
    node_status.storage_backends_online = 2;
    snap.node_status[a] = node_status;
    IdentityAssociationReset identity_reset;
    identity_reset.host = "10.44.1.50";
    identity_reset.port = 57401;
    identity_reset.stale_node_id = b;
    identity_reset.epoch = 3;
    identity_reset.reset_unix_ms = 1712345679999ULL;
    identity_reset.reset_by = a;
    identity_reset.reason = "endpoint reassigned";
    snap.identity_resets[identity_reset_key(identity_reset.host, identity_reset.port)] =
        identity_reset;
    auto encoded = encode_snapshot(snap);
    auto decoded = decode_snapshot(encoded);
    CHECK(decoded.metadata_voters == snap.metadata_voters);
    CHECK(decoded.mutation_sequences == snap.mutation_sequences);
    CHECK(decoded.entries.at("/movie.mkv").size == 123);
    CHECK(decoded.node_status == snap.node_status);
    CHECK(decoded.identity_resets == snap.identity_resets);
    REQUIRE(decoded.entries.contains(unicode_dir));
    REQUIRE(decoded.entries.contains(unicode_file));
    CHECK(decoded.entries.at(unicode_dir).type == EntryType::directory);
    CHECK(decoded.entries.at(unicode_file).type == EntryType::file);
    // Persistent metadata remains byte-preserving. The macOS FUSE adapter owns
    // the platform presentation rule and decomposes only names returned to macFUSE.
    const std::string cafe_name = "Caf\xc3\xa9 del Mar";
#if defined(__APPLE__)
    CHECK(macos_fuse_decomposed_name(cafe_name) == "Cafe\xcc\x81 del Mar");
    CHECK(macos_fuse_decomposed_name("Na Buachaill\xc3\xad lainn.mp3") ==
          "Na Buachailli\xcc\x81 lainn.mp3");
    CHECK(macos_fuse_composed_name("Cafe\xcc\x81 del Mar") == cafe_name);
    CHECK(macos_fuse_composed_name("Na Buachailli\xcc\x81 lainn.mp3") ==
          "Na Buachaill\xc3\xad lainn.mp3");
#else
    CHECK(macos_fuse_decomposed_name(cafe_name) == cafe_name);
#endif
    REQUIRE(decoded.garbage.size() == 1);
    CHECK(decoded.garbage.front().id == garbage_id);
    CHECK(decoded.garbage.front().retired_at_ns == 123456789);
    CHECK(decoded.garbage.front().retirement_id == retirement_id);

    auto delta_target = decoded;
    delta_target.mutation_sequences[a] = 8;
    delta_target.entries.at("/movie.mkv").size = 456;
    FsEntry extra;
    extra.type = EntryType::file;
    extra.size = 999;
    delta_target.entries["/extra.mkv"] = extra;
    auto catalogue_id = object_id(pattern(1024));
    delta_target.catalogue_root = catalogue_id;
    auto garbage_id_2 = object_id(pattern(2048));
    delta_target.garbage.push_back({garbage_id_2, 987654321, random_node_id()});
    delta_target.node_status.at(a).storage_used += 1024;
    delta_target.node_status[b] = node_status;
    delta_target.node_status[b].host = "node-b.example";
    auto replacement_reset = identity_reset;
    replacement_reset.epoch = 4;
    replacement_reset.reset_unix_ms += 1;
    delta_target.identity_resets[identity_reset_key(identity_reset.host, identity_reset.port)] =
        replacement_reset;
    auto compact = metadata_delta(decoded, delta_target);
    REQUIRE(compact.has_value());
    CHECK(compact->upsert_node_status.size() == 2);
    CHECK(compact->upsert_identity_resets.size() == 1);
    auto encoded_delta = encode_metadata_delta(*compact);
    auto decoded_delta = decode_metadata_delta(encoded_delta);
    auto reconstructed = apply_metadata_delta(decoded, decoded_delta);
    CHECK(encode_snapshot(reconstructed) == encode_snapshot(delta_target));
    CHECK(encoded_delta.size() < encode_snapshot(delta_target).size());

    // The current compact-delta format represents tombstone replacement and
    // pruning directly while preserving the parent's canonical snapshot family.
    auto garbage_compacted = delta_target;
    garbage_compacted.garbage.erase(garbage_compacted.garbage.begin());
    garbage_compacted.garbage.front().retired_at_ns += 1;
    garbage_compacted.garbage.front().retirement_id = random_node_id();
    auto garbage_delta = metadata_delta(delta_target, garbage_compacted);
    REQUIRE(garbage_delta.has_value());
    CHECK(garbage_delta->erase_garbage.size() == 1);
    CHECK(garbage_delta->upsert_garbage.size() == 1);
    auto garbage_delta_roundtrip = decode_metadata_delta(encode_metadata_delta(*garbage_delta));
    CHECK(encode_snapshot(apply_metadata_delta(delta_target, garbage_delta_roundtrip)) ==
          encode_snapshot(garbage_compacted));

    // Legacy SM7 snapshots remain valid; their tombstones decode without
    // retirement time/id and are stamped by GC.
    Writer old_v7;
    const std::array<uint8_t, 8> old_v7_magic{'D', 'H', 'T', 'M', 'E', 'T', 'A', '7'};
    old_v7.raw(old_v7_magic);
    old_v7.u32(1);
    old_v7.fixed(a.bytes);
    old_v7.u32(1);
    old_v7.u64(4ULL * 1024 * 1024);
    old_v7.u32(1);
    old_v7.fixed(a.bytes);
    old_v7.u64(7);
    old_v7.u32(1);
    old_v7.string("/");
    old_v7.u8(static_cast<uint8_t>(EntryType::directory));
    old_v7.u32(0755);
    old_v7.u32(0);
    old_v7.u32(0);
    old_v7.u64(0);
    old_v7.i64(0);
    old_v7.i64(0);
    old_v7.u64(1);
    old_v7.u32(0);
    old_v7.u8(0);
    old_v7.u32(1);
    old_v7.fixed(garbage_id.bytes);
    auto upgraded_v7 = decode_snapshot(old_v7.data());
    REQUIRE(upgraded_v7.garbage.size() == 1);
    CHECK(upgraded_v7.garbage.front().id == garbage_id);
    CHECK(upgraded_v7.garbage.front().retired_at_ns == 0);
    CHECK(upgraded_v7.garbage.front().retirement_id == NodeId{});

    // DLT1 is accepted only from persisted journals: encoders emit DLT5, and old
    // records still replay byte-for-byte.
    Writer old_delta;
    const std::array<uint8_t, 8> old_delta_magic{'D', 'H', 'T', 'M', 'D', 'L', 'T', '1'};
    old_delta.raw(old_delta_magic);
    old_delta.u32(0);
    old_delta.u32(0);
    old_delta.u32(0);
    old_delta.u8(static_cast<uint8_t>(CatalogueDelta::unchanged));
    old_delta.u32(1);
    auto legacy_delta_id = object_id(pattern(3072));
    old_delta.fixed(legacy_delta_id.bytes);
    auto decoded_old_delta = decode_metadata_delta(old_delta.data());
    REQUIRE(decoded_old_delta.upsert_garbage.size() == 1);
    CHECK(decoded_old_delta.upsert_garbage.front().id == legacy_delta_id);
    CHECK(decoded_old_delta.upsert_garbage.front().retired_at_ns == 0);
    CHECK(decoded_old_delta.upsert_garbage.front().retirement_id == NodeId{});

    // Storage compatibility includes the authenticated delta journal, not just
    // accepting old payloads in isolation. DLT1 successor hashes were computed
    // over SM7 bytes, so replay must reconstruct that exact historical encoding.
    auto legacy_journal_path = t.path() / "legacy-journal-node";
    MetadataReplica legacy_journal(legacy_journal_path, keys.storage);
    MetadataRecord legacy_seed;
    legacy_seed.generation = 17;
    legacy_seed.previous = object_id(pattern(211));
    legacy_seed.payload = old_v7.data();
    legacy_seed.hash =
        metadata_hash(legacy_seed.generation, legacy_seed.previous, legacy_seed.payload);
    REQUIRE(legacy_journal.seed(legacy_seed));
    REQUIRE(legacy_journal.remember_current_committed(legacy_seed.generation, legacy_seed.hash));
    MetadataRecord legacy_successor;
    REQUIRE(legacy_journal.cas_delta(legacy_seed.generation, legacy_seed.hash, old_delta.data(),
                                     &legacy_successor));
    REQUIRE(legacy_journal.remember_current_committed(legacy_successor.generation,
                                                      legacy_successor.hash));
    CHECK(std::equal(legacy_successor.payload.begin(), legacy_successor.payload.begin() + 8,
                     old_v7_magic.begin()));
    {
        MetadataReplica replayed_legacy(legacy_journal_path, keys.storage);
        CHECK(replayed_legacy.current().hash == legacy_successor.hash);
        CHECK(replayed_legacy.committed().hash == legacy_successor.hash);
        auto replayed_snapshot = decode_snapshot(replayed_legacy.current().payload);
        REQUIRE(replayed_snapshot.garbage.size() == 2);
        CHECK(replayed_snapshot.garbage.back().id == legacy_delta_id);
        CHECK(replayed_snapshot.garbage.back().retired_at_ns == 0);
    }

    // Snapshots without a catalogue-root field decode to an empty catalogue,
    // without migration.
    Writer old;
    const std::array<uint8_t, 8> old_magic{'D', 'H', 'T', 'M', 'E', 'T', 'A', '5'};
    old.raw(old_magic);
    old.u32(0); // metadata voters
    old.u32(1); // data replication
    old.u64(4ULL * 1024 * 1024);
    old.u32(1); // root entry
    old.string("/");
    old.u8(static_cast<uint8_t>(EntryType::directory));
    old.u32(0755);
    old.u32(0);
    old.u32(0);
    old.u64(0);
    old.i64(0);
    old.i64(0);
    old.u64(0);
    old.u32(0); // extents
    old.u32(0); // garbage
    auto upgraded = decode_snapshot(old.data());
    CHECK(!upgraded.catalogue_root.has_value());
    CHECK(upgraded.entries.contains("/"));

    auto replica_path = t.path() / "node";
    MetadataReplica replica(replica_path, keys.storage);
    auto current = replica.current();
    CHECK(replica.current_identity().generation == current.generation);
    CHECK(replica.current_identity().hash == current.hash);
    CHECK(replica.committed_identity().generation == replica.committed().generation);
    CHECK(replica.committed_identity().hash == replica.committed().hash);
    CHECK(replica.committed().hash == current.hash);
    MetadataRecord next;
    REQUIRE(replica.cas(current.generation, current.hash, encoded, &next));
    CHECK(next.generation == current.generation + 1);
    CHECK(decode_snapshot(next.payload).metadata_voters.size() == 3);
    // A successful replica CAS is not yet a committed cluster checkpoint. The
    // committed head advances only after MetadataManager has observed the write floor.
    CHECK(replica.committed().hash == current.hash);
    REQUIRE(replica.remember_current_committed(next.generation, next.hash));
    CHECK(replica.committed().hash == next.hash);
    CHECK(!replica.remember_current_committed(next.generation, current.hash));

    MetadataReplica reopened(replica_path, keys.storage);
    CHECK(reopened.current().hash == next.hash);
    CHECK(reopened.committed().hash == next.hash);
    CHECK(std::filesystem::exists(replica_path / "metadata" / "checkpoint.meta"));
    CHECK(std::filesystem::exists(replica_path / "metadata" / "journal.log"));

    // Ordinary mutation is a compact delta proposal. It survives restart
    // through the encrypted journal and does not require a full snapshot file
    // rewrite for either prepare or commit.
    auto before_delta = decode_snapshot(reopened.current().payload);
    auto after_delta = before_delta;
    after_delta.entries.at("/movie.mkv").size = 456;
    after_delta.mutation_sequences[a] = 8;
    auto delta = metadata_delta(before_delta, after_delta);
    REQUIRE(delta.has_value());
    auto delta_bytes = encode_metadata_delta(*delta);
    const auto journal_before_delta =
        std::filesystem::file_size(replica_path / "metadata" / "journal.log");
    MetadataRecord delta_next;
    REQUIRE(reopened.cas_delta(reopened.current().generation, reopened.current().hash, delta_bytes,
                               &delta_next));
    CHECK(reopened.committed().hash == next.hash);
    REQUIRE(reopened.remember_current_committed(delta_next.generation, delta_next.hash));
    auto journal_size = std::filesystem::file_size(replica_path / "metadata" / "journal.log");
    const auto journal_growth = journal_size - journal_before_delta;
    // For tiny snapshots the two frames' fixed overhead can exceed the snapshot;
    // growth must track the compact delta plus bounded framing, not the full
    // successor snapshot.
    CHECK(delta_bytes.size() < delta_next.payload.size());
    CHECK(journal_growth >= delta_bytes.size());
    CHECK(journal_growth < delta_bytes.size() + 512);

    MetadataReplica reopened_again(replica_path, keys.storage);
    CHECK(reopened_again.current().hash == delta_next.hash);
    CHECK(reopened_again.committed().hash == delta_next.hash);
    CHECK(decode_snapshot(reopened_again.current().payload).entries.at("/movie.mkv").size == 456);

    // Accepted-but-uncommitted state remains current after restart but does not
    // become a recovery witness. An interrupted trailing journal append is
    // discarded without losing the last complete proposal.
    auto uncommitted_path = t.path() / "uncommitted-node";
    MetadataRecord uncommitted_next;
    Hash256 uncommitted_base_hash;
    {
        MetadataReplica uncommitted(uncommitted_path, keys.storage);
        auto base = uncommitted.current();
        uncommitted_base_hash = base.hash;
        auto before = decode_snapshot(base.payload);
        auto after = before;
        after.entries["/pending"] = extra;
        auto d = metadata_delta(before, after);
        REQUIRE(d.has_value());
        auto dbytes = encode_metadata_delta(*d);
        REQUIRE(uncommitted.cas_delta(base.generation, base.hash, dbytes, &uncommitted_next));
        CHECK(uncommitted.committed().hash == uncommitted_base_hash);
    }
    {
        std::ofstream tail(uncommitted_path / "metadata" / "journal.log",
                           std::ios::binary | std::ios::app);
        tail.write("bad", 3);
    }
    {
        MetadataReplica recovered(uncommitted_path, keys.storage);
        CHECK(recovered.current().hash == uncommitted_next.hash);
        CHECK(recovered.committed().hash == uncommitted_base_hash);
    }

    // A crash can extend the file to the complete frame length while leaving
    // the final encrypted bytes/tag unauthenticated. Preserve that tail for
    // diagnosis and replay the authenticated prefix; a bad frame in the middle
    // remains fatal because skipping it would break the metadata chain.
    const auto authenticated_journal_size =
        std::filesystem::file_size(uncommitted_path / "metadata" / "journal.log");
    {
        Writer envelope;
        std::array<uint8_t, 12> nonce{};
        std::array<uint8_t, 16> tag{};
        envelope.fixed(nonce);
        envelope.fixed(tag);
        const Bytes ciphertext{0x42};
        envelope.bytes(ciphertext);
        auto payload = envelope.take();
        Writer frame;
        frame.u32(static_cast<uint32_t>(payload.size()));
        frame.raw(payload);
        auto bytes = frame.take();
        std::ofstream tail(uncommitted_path / "metadata" / "journal.log",
                           std::ios::binary | std::ios::app);
        tail.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    {
        MetadataReplica recovered(uncommitted_path, keys.storage);
        CHECK(recovered.current().hash == uncommitted_next.hash);
        CHECK(recovered.committed().hash == uncommitted_base_hash);
        CHECK(std::filesystem::file_size(uncommitted_path / "metadata" / "journal.log") ==
              authenticated_journal_size);
        bool preserved = false;
        for (const auto& entry :
             std::filesystem::directory_iterator(uncommitted_path / "metadata")) {
            preserved |= entry.path().filename().string().starts_with("journal.log.corrupt.");
        }
        CHECK(preserved);
    }

    // A prepared proposal which never reached the write floor has no authority.
    // A later proposal based on the last committed head must be able to pre-empt
    // it, and that rollback/replacement must survive another restart.
    MetadataRecord replacement;
    {
        MetadataReplica recovered(uncommitted_path, keys.storage);
        const auto base = recovered.committed();
        auto before = decode_snapshot(base.payload);
        auto after = before;
        after.entries["/replacement"] = extra;
        auto d = metadata_delta(before, after);
        REQUIRE(d.has_value());
        const auto dbytes = encode_metadata_delta(*d);
        REQUIRE(recovered.cas_delta(base.generation, base.hash, dbytes, &replacement));
        CHECK(recovered.current().hash == replacement.hash);
        CHECK(recovered.committed().hash == base.hash);
        REQUIRE(recovered.remember_current_committed(replacement.generation, replacement.hash));
    }
    {
        MetadataReplica recovered(uncommitted_path, keys.storage);
        CHECK(recovered.current().hash == replacement.hash);
        CHECK(recovered.committed().hash == replacement.hash);
        CHECK(decode_snapshot(recovered.committed().payload).entries.contains("/replacement"));
        CHECK(!decode_snapshot(recovered.committed().payload).entries.contains("/pending"));
    }

    // The persistent metadata cache is an independently encrypted committed
    // snapshot. If the primary checkpoint is damaged, it may seed startup but
    // is explicitly marked non-authoritative until replica checkpointing clears
    // the recovery marker. The damaged primary files remain quarantined.
    auto damaged_checkpoint_path = t.path() / "damaged-checkpoint-node";
    MetadataRecord recovery_seed;
    {
        MetadataReplica replica(damaged_checkpoint_path, keys.storage);
        recovery_seed = replica.committed();
    }
    {
        auto checkpoint = damaged_checkpoint_path / "metadata" / "checkpoint.meta";
        std::fstream file(checkpoint, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file.good());
        file.seekg(40);
        char byte{};
        file.read(&byte, 1);
        REQUIRE(file.good());
        byte ^= 0x5a;
        file.seekp(40);
        file.write(&byte, 1);
        file.flush();
        REQUIRE(file.good());
    }
    {
        MetadataReplica recovered(damaged_checkpoint_path, keys.storage, recovery_seed);
        CHECK(recovered.recovery_required());
        CHECK(recovered.committed().hash == recovery_seed.hash);
        bool preserved = false;
        for (const auto& entry :
             std::filesystem::directory_iterator(damaged_checkpoint_path / "metadata")) {
            preserved |= entry.path().filename().string().starts_with("checkpoint.meta.corrupt.");
        }
        CHECK(preserved);
        recovered.mark_recovered();
        CHECK(!recovered.recovery_required());
    }
    CHECK(!std::filesystem::exists(damaged_checkpoint_path / "metadata" / "recovery.required"));
    {
        MetadataReplica reopened_recovered(damaged_checkpoint_path, keys.storage);
        CHECK(reopened_recovered.committed().hash == recovery_seed.hash);
    }

    // Without an independent recovery seed, primary authentication failure is
    // still fail-closed and identifies the exact file rather than surfacing a
    // context-free AES error.
    auto no_seed_path = t.path() / "damaged-checkpoint-no-seed";
    {
        MetadataReplica replica(no_seed_path, keys.storage);
    }
    {
        auto checkpoint = no_seed_path / "metadata" / "checkpoint.meta";
        std::fstream file(checkpoint, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file.good());
        file.seekg(40);
        char byte{};
        file.read(&byte, 1);
        REQUIRE(file.good());
        byte ^= 0x5a;
        file.seekp(40);
        file.write(&byte, 1);
        file.flush();
        REQUIRE(file.good());
    }
    try {
        MetadataReplica should_fail(no_seed_path, keys.storage);
        CHECK(false);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        CHECK(message.find("checkpoint.meta") != std::string::npos);
        CHECK(message.find("AES-GCM authentication failed") != std::string::npos);
    }

    // Periodic compaction bounds replay. 64 committed mutations produce 128
    // prepare+commit journal records; the threshold checkpoints and truncates
    // them rather than allowing an unbounded replay log.
    auto compact_path = t.path() / "compact-node";
    {
        MetadataReplica compacted(compact_path, keys.storage);
        for (size_t i = 0; i < 65; ++i) {
            auto base = compacted.current();
            auto before = decode_snapshot(base.payload);
            auto after = before;
            after.entries["/"].mtime_ns = static_cast<int64_t>(i + 1);
            auto d = metadata_delta(before, after);
            REQUIRE(d.has_value());
            auto dbytes = encode_metadata_delta(*d);
            MetadataRecord proposal;
            REQUIRE(compacted.cas_delta(base.generation, base.hash, dbytes, &proposal));
            REQUIRE(compacted.remember_current_committed(proposal.generation, proposal.hash));
        }
        CHECK(std::filesystem::file_size(compact_path / "metadata" / "journal.log") > 4096);
        compacted.compact();
    }
    CHECK(std::filesystem::file_size(compact_path / "metadata" / "journal.log") < 4096);
    MetadataReplica compacted_again(compact_path, keys.storage);
    CHECK(compacted_again.current().generation == 66);
    CHECK(compacted_again.current().hash == compacted_again.committed().hash);
}

MACHA_TEST("storage_metadata", test_metadata_identity_rpc) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto c1 = config_for(cluster.path() / "identity-1", cluster.keyfile(), free_port());
    auto c2 = config_for(cluster.path() / "identity-2", cluster.keyfile(), free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(n1.wait_local_state_ready(10s));
    REQUIRE(n2.wait_local_state_ready(10s));
    REQUIRE(wait_until([&] { return n1.membership().active().size() >= 2; }, 5s));

    auto peers = n1.membership().active();
    auto found = std::find_if(peers.begin(), peers.end(),
                              [&](const NodeInfo& peer) { return peer.id == n2.node_id(); });
    REQUIRE(found != peers.end());
    auto reply = n1.call(*found, MessageType::get_metadata_identity, {}, FrameType::speculative);
    REQUIRE(reply.message.type == MessageType::metadata_identity_reply);
    CHECK(reply.message.payload.size() == sizeof(uint64_t) + 32);
    Reader reader(reply.message.payload);
    MetadataIdentity observed;
    observed.generation = reader.u64();
    observed.hash.bytes = reader.fixed<32>();
    reader.finish();
    CHECK(observed == n2.metadata_replica().current_identity());

    n2.stop();
    n1.stop();
}

MACHA_TEST("storage_metadata", test_repair_step_is_bounded_and_yields) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    // Both nodes own every object, so the one object below is repair's to
    // push to n2; n2's own repair never pulls it first, having no credit.
    c1.replication = c2.replication = 2;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.maintenance.idle_bandwidth_fraction = 0.0;
    c2.maintenance.idle_bandwidth_fraction = 0.0;

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Reachability precedes DATA readiness, and repair is a storage-plane
    // operation, so wait for local state.
    REQUIRE(s1.node().wait_local_state_ready(std::chrono::seconds{10}));
    REQUIRE(s2.node().wait_local_state_ready(std::chrono::seconds{10}));

    auto bytes = pattern(512 * 1024);
    auto id = object_id(bytes);
    REQUIRE(s1.local_state().data().put(id, bytes));
    REQUIRE(!s2.local_state().data().has(id));
    std::vector<ObjectId> live{id};
    DistributedStore repair(s1.node(), s1.local_state(), s1.resources().activity, s1.resources().data, s1.resources().memory, s1.resources().events);
    const auto full_lists_before = s1.local_state().data().full_list_scans();

    auto yielded =
        repair.repair_step(8ULL * 1024 * 1024, 8, live, [] { return true; });
    CHECK(yielded.yielded);
    CHECK(!yielded.complete);
    CHECK(yielded.bytes_transferred == 0);
    CHECK(!s2.local_state().data().has(id));

    // One remote operation is enough to probe but not both probe and upload.
    // The pass must report itself incomplete rather than being mistaken for a
    // quiescent namespace simply because it transferred zero bytes.
    auto bounded = repair.repair_step(8ULL * 1024 * 1024, 1, live);
    CHECK(!bounded.complete);
    CHECK(bounded.bytes_transferred == 0);
    CHECK(bounded.remote_operations == 1);
    CHECK(!s2.local_state().data().has(id));

    auto completed = repair.repair_step(8ULL * 1024 * 1024, 8, live);
    CHECK(completed.bytes_transferred == bytes.size());
    CHECK(completed.complete);
    CHECK(completed.remote_operations <= 8);
    CHECK(s2.local_state().data().has(id));
    CHECK(s1.local_state().data().full_list_scans() == full_lists_before);
    CHECK(yielded.push_examined <= 64);
    CHECK(bounded.push_examined <= 64);
    CHECK(completed.push_examined <= 64);
    CHECK(yielded.pull_examined <= 64);
    CHECK(bounded.pull_examined <= 64);
    CHECK(completed.pull_examined <= 64);
    CHECK(yielded.push_examined + yielded.pull_examined <= 64);
    CHECK(bounded.push_examined + bounded.pull_examined <= 64);
    CHECK(completed.push_examined + completed.pull_examined <= 64);

    s2.stop();
    s1.stop();
}

MACHA_TEST("storage_metadata", test_genesis_root_configuration) {
    TestService fixture("single");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.filesystem.root_uid = 501;
    config.filesystem.root_gid = 20;
    config.filesystem.root_mode = 0750;

    auto& service = fixture.start();
    auto root = service.filesystem().getattr("/");
    CHECK(root.type == EntryType::directory);
    CHECK(root.uid == 501);
    CHECK(root.gid == 20);
    CHECK(root.mode == 0750);
    CHECK(root.ctime_ns > 0);
    CHECK(root.mtime_ns > 0);
}

MACHA_FAST_TEST("storage_metadata", test_metadata_delta_rejects_unrepresentable_garbage_reordering) {
    auto before = decode_snapshot(genesis_metadata().payload);
    GarbageRef first{object_id(pattern(4097)), 10, random_node_id()};
    GarbageRef second{object_id(pattern(8193)), 20, random_node_id()};
    if (first.id < second.id)
        std::swap(first, second);
    before.garbage = {first, second};

    auto reordered = before;
    std::sort(reordered.garbage.begin(), reordered.garbage.end(),
              [](const GarbageRef& a, const GarbageRef& b) { return a.id < b.id; });
    REQUIRE(reordered.garbage != before.garbage);

    // DLT5/6 cannot reorder retained tombstones and DLT7 only into canonical
    // ObjectId order; any other reorder is not compact, since replay would
    // produce a byte-different record.
    auto canonical = metadata_delta(before, reordered);
    REQUIRE(canonical.has_value());
    CHECK(canonical->canonical_garbage);
    CHECK(encode_snapshot(apply_metadata_delta(before, *canonical)) == encode_snapshot(reordered));

    GarbageRef third{object_id(pattern(12289)), 30, random_node_id()};
    auto three = reordered;
    three.garbage.push_back(third);
    canonicalise_garbage(three.garbage);
    auto scrambled = three;
    std::swap(scrambled.garbage[0], scrambled.garbage[2]);
    REQUIRE(!garbage_is_canonical(scrambled.garbage));
    CHECK(!metadata_delta(three, scrambled).has_value());
}

MACHA_TEST("storage_metadata", test_local_metadata_store_falls_back_from_invalid_delta) {
    TestService fixture("local-delta-fallback");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;

    auto& service = fixture.start();
    MetadataManager metadata(service.node(), service.local_state(), service.metadata_server());
    auto committed = metadata.mutate_delta([](MetadataSnapshot& snapshot, MetadataDelta&) {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        snapshot.entries["/full-fallback"] = entry;
        // The supplied exact delta omits the entry: the local replica must reject it
        // and retry with the full record, as a remote replica does.
    });

    CHECK(service.filesystem().getattr("/full-fallback").type == EntryType::directory);
    auto history = service.local_state().replica().history_entry(committed.hash);
    REQUIRE(history.has_value());
    CHECK(history->body == MetadataHistoryEntry::Body::full);
}

MACHA_FAST_TEST("storage_metadata", test_decoded_extent_vectors_carry_no_allocator_slack) {
    // The decoded head stays pinned in the materialization cache, so extent
    // vectors must carry no capacity beyond their size.
    auto snapshot = decode_snapshot(genesis_metadata().payload);
    // Sizes either side of a power of two: geometric growth is only visible
    // when the final count is not itself the capacity the doubling lands on.
    for (const auto count : {1u, 3u, 100u, 1000u, 1025u}) {
        FsEntry file;
        file.type = EntryType::file;
        file.size = static_cast<uint64_t>(count) * 16;
        for (uint32_t i = 0; i < count; ++i)
            file.extents.push_back(
                {i * 16ull, 16, object_id(pattern(32, static_cast<uint8_t>(i))), false});
        snapshot.entries["/f" + std::to_string(count) + ".mkv"] = std::move(file);
    }

    const auto decoded = decode_snapshot(encode_snapshot(snapshot));
    CHECK(decoded.entries == snapshot.entries);
    for (const auto& [path, entry] : decoded.entries) {
        if (entry.extents.empty())
            continue;
        CHECK(entry.extents.capacity() == entry.extents.size());
    }

    // The extent count is caller-supplied, so it must never size an allocation
    // on its own. A payload claiming ten million extents with nothing behind it
    // has to be refused on the truncated input, not reserved for first.
    static constexpr std::array<uint8_t, 8> sm13{'D', 'H', 'T', 'M', 'E', 'T', 'B', '3'};
    Writer forged;
    forged.fixed(sm13);
    forged.u32(0); // metadata_voters
    forged.u32(snapshot.data_replication);
    forged.u64(snapshot.extent_size);
    forged.u32(0); // mutation_sequences
    forged.u32(1); // one entry
    forged.string("/liar.mkv");
    forged.u8(static_cast<uint8_t>(EntryType::file));
    forged.u32(0644);
    forged.u32(0);
    forged.u32(0);
    forged.u64(0);
    forged.i64(0);
    forged.i64(0);
    forged.u64(1);
    forged.u32(10000000); // claimed extents; none follow
    bool refused = false;
    try {
        (void)decode_snapshot(forged.data());
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
}

} // namespace

namespace {
TorrentRequest sample_torrent_request(const std::string& id, const std::string& hash, uint64_t created) {
    TorrentRequest r;
    r.id = id;
    r.info_hash = hash;
    r.source = "magnet:?xt=urn:btih:" + hash;
    r.created_unix_ms = created;
    r.created_by = NodeId{};
    r.name = "Sample";
    return r;
}
} // namespace

MACHA_FAST_TEST("storage_metadata", test_torrent_requests_join_without_conflicts) {
    // Metadata writes are not compare-and-swap and history branches, so two
    // replicas can change one request at once. Every field has a rule and the
    // merge is a join: the same answer from either side, and again.
    const auto a_node = random_node_id();
    const auto b_node = random_node_id();
    auto base = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);

    {
        Writer w;
        auto full = base;
        full.pinned_node_id = a_node;
        full.remove_after_ms = 0;
        full.claim = TorrentClaim{b_node, 2, 500};
        full.phase = TorrentPhase::importing;
        full.phase_epoch = 2;
        full.ingest_job_id = "ingest";
        full.error_code = "code";
        full.removed_unix_ms = 9;
        encode_torrent_request(w, full);
        const auto bytes = w.take();
        Reader r(bytes);
        CHECK(decode_torrent_request(r) == full);
        r.finish();
    }

    auto paused = base;
    paused.desired = TorrentDesired::paused;
    paused.desired_changed_unix_ms = 200;
    paused.desired_changed_by = a_node;
    auto cancelled = base;
    cancelled.desired = TorrentDesired::cancelled;
    cancelled.desired_changed_unix_ms = 150; // earlier, and still final
    cancelled.desired_changed_by = b_node;
    auto claimed = base;
    claimed.claim = TorrentClaim{a_node, 1, 300};
    claimed.phase = TorrentPhase::downloading;
    claimed.phase_epoch = 1;
    claimed.progress_unix_ms = 300;
    auto rival = base;
    rival.claim = TorrentClaim{b_node, 1, 290}; // same epoch, earlier: wins
    auto takeover = base;
    takeover.claim = TorrentClaim{b_node, 2, 900};
    takeover.phase = TorrentPhase::downloading;
    takeover.phase_epoch = 2;
    takeover.progress_unix_ms = 900;
    auto removed = base;
    removed.removed_unix_ms = 1000;

    const std::vector<TorrentRequest> variants{base, paused, cancelled, claimed, rival, takeover, removed};
    for (const auto& x : variants) {
        CHECK(merge_torrent_request(x, x) == x);
        for (const auto& y : variants) {
            CHECK(merge_torrent_request(x, y) == merge_torrent_request(y, x));
            for (const auto& z : variants)
                CHECK(merge_torrent_request(merge_torrent_request(x, y), z) ==
                      merge_torrent_request(x, merge_torrent_request(y, z)));
        }
    }
    CHECK(merge_torrent_request(paused, cancelled).desired == TorrentDesired::cancelled);
    CHECK(merge_torrent_request(claimed, rival).claim->node_id == b_node);
    CHECK(merge_torrent_request(claimed, takeover).claim->epoch == 2);
    CHECK(merge_torrent_request(claimed, takeover).phase_epoch == 2);
    CHECK(merge_torrent_request(claimed, removed).removed_unix_ms == 1000);

    // Collections: a tombstone erased on one side stays erased unless the
    // other changed it; two live requests for one torrent keep the earlier.
    std::map<std::string, TorrentRequest, std::less<>> m_base{{base.id, removed}};
    std::map<std::string, TorrentRequest, std::less<>> erased{};
    std::map<std::string, TorrentRequest, std::less<>> untouched{{base.id, removed}};
    CHECK(merge_torrent_requests(m_base, erased, untouched).empty());
    auto touched = removed;
    touched.desired = TorrentDesired::cancelled;
    touched.desired_changed_unix_ms = 2000;
    CHECK(merge_torrent_requests(m_base, erased, {{base.id, touched}}).size() == 1);

    const auto first = sample_torrent_request(std::string(32, 'b'), std::string(40, '2'), 100);
    const auto second = sample_torrent_request(std::string(32, 'c'), std::string(40, '2'), 101);
    const auto both = merge_torrent_requests({}, {{first.id, first}}, {{second.id, second}});
    REQUIRE(both.size() == 2);
    CHECK(both.at(first.id).desired == TorrentDesired::active);
    CHECK(both.at(second.id).desired == TorrentDesired::cancelled);
    CHECK(both.at(second.id).error_code == "duplicate_torrent");
    CHECK(merge_torrent_requests({}, {{second.id, second}}, {{first.id, first}}) == both);
}

// A tree-backed head names tombstone batches (SM19, only while it names one)
// and DLT11 names and drops them; a delta's successor re-encodes to the same
// bytes, a merge is the union, and a head's tombstones are read from its
// batches, the latest retirement per object.
MACHA_FAST_TEST("storage_metadata", test_tombstone_batches_ride_the_head_and_its_deltas) {
    const auto magic = [](const Bytes& bytes) { return std::string(bytes.begin(), bytes.begin() + 8); };
    MetadataSnapshot base = decode_snapshot(genesis_metadata().payload);
    base.metadata_write_replicas_required = 1;
    base.entries.clear();
    base.namespace_root = object_id(pattern(64));
    CHECK(magic(encode_snapshot_v14(base)) == "DHTMETB4");

    std::map<ObjectId, Bytes> objects;
    const auto batch_of = [&](int64_t retired, std::vector<ObjectId> ids) {
        std::sort(ids.begin(), ids.end());
        const auto bytes = encode_tombstone_batch(retired, ids);
        const auto id = object_id(bytes);
        objects[id] = bytes;
        return TombstoneBatch{id, retired, static_cast<uint32_t>(ids.size())};
    };
    const auto a = object_id(pattern(10, 1)), b = object_id(pattern(10, 2)),
               c = object_id(pattern(10, 3));
    const auto first = batch_of(100, {a, b});
    const auto second = batch_of(200, {b, c});

    auto with = base;
    with.tombstone_batches = {first, second};
    std::sort(with.tombstone_batches.begin(), with.tombstone_batches.end(),
              [](const TombstoneBatch& x, const TombstoneBatch& y) { return x.id < y.id; });
    const auto with_bytes = encode_snapshot_v14(with);
    CHECK(magic(with_bytes) == "DHTMETB9");
    CHECK(decode_snapshot(with_bytes).tombstone_batches == with.tombstone_batches);
    CHECK(encode_snapshot_v14(decode_snapshot(with_bytes)) == with_bytes);
    // The legacy clock still rides SM19.
    auto clocked = with;
    clocked.legacy_clock = std::map<NodeId, uint64_t>{{random_node_id(), 3}};
    CHECK(decode_snapshot(encode_snapshot_v14(clocked)).legacy_clock == clocked.legacy_clock);

    const NamespaceDeltaApplier same_tree = [](const ObjectId& root, const MetadataDelta&) {
        return root;
    };
    const auto added = metadata_delta(base, with);
    REQUIRE(added.has_value());
    const auto added_bytes = encode_metadata_delta(*added);
    CHECK(magic(added_bytes) == "DHTMDLTB");
    CHECK(encode_snapshot_v14(apply_metadata_delta(base, decode_metadata_delta(added_bytes),
                                                   same_tree)) == with_bytes);
    auto dropped = with;
    std::erase(dropped.tombstone_batches, first);
    const auto drop = metadata_delta(with, dropped);
    REQUIRE(drop.has_value());
    CHECK(drop->drop_tombstone_batches == std::vector<ObjectId>{first.id});
    CHECK(encode_snapshot_v14(apply_metadata_delta(
              with, decode_metadata_delta(encode_metadata_delta(*drop)), same_tree)) ==
          encode_snapshot_v14(dropped));

    // A merge (over materialised namespaces) is the union of both heads'
    // batches.
    auto left = base;
    left.namespace_root.reset();
    left.tombstone_batches = {first};
    auto right = left;
    right.tombstone_batches = {second};
    Hash256 left_head{}, right_head{};
    left_head.bytes[0] = 1;
    right_head.bytes[0] = 2;
    CHECK(merge_metadata_heads(left, right, left_head, right_head).snapshot.tombstone_batches ==
          with.tombstone_batches);

    // Tombstones: the latest retirement per object; an unreadable batch is
    // counted and left out.
    size_t unreadable = 0;
    const auto read = [&](const ObjectId& id) -> std::optional<Bytes> {
        const auto found = objects.find(id);
        return found == objects.end() ? std::nullopt : std::optional(found->second);
    };
    const auto all = tombstones_of(with, read, &unreadable);
    CHECK(unreadable == 0);
    REQUIRE(all.size() == 3);
    for (const auto& tombstone : all)
        CHECK(tombstone.retired_at_ns == (tombstone.id == a ? 100 : 200));
    CHECK(tombstone_count(with) == 4);
    objects.erase(second.id);
    unreadable = 0;
    CHECK(tombstones_of(with, read, &unreadable).size() == 2);
    CHECK(unreadable == 1);

    // An inline namespace cannot carry batches; a tree-backed mutation's
    // retirements wait for the commit.
    const auto throws = [](const auto& call) {
        try {
            call();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    auto inline_head = decode_snapshot(genesis_metadata().payload);
    inline_head.tombstone_batches = {first};
    CHECK(throws([&] { (void)encode_snapshot(inline_head); }));
    MetadataDelta delta;
    auto tree_head = base;
    retire_objects(tree_head, delta, {a, b});
    retire_objects(tree_head, delta, {c});
    CHECK(tree_head.garbage.empty());
    REQUIRE(delta.pending_tombstones.size() == 1);
    CHECK(delta.pending_tombstones.front().ids.size() == 3);
    CHECK(throws([&] { (void)encode_metadata_delta(delta); }));
}

MACHA_FAST_TEST("storage_metadata", test_torrent_requests_ride_the_snapshot_and_its_deltas) {
    // SM15 (inline namespace), SM16 (tree-backed) and DLT9 carry the
    // collection; a cluster with none keeps its exact encodings.
    const auto magic = [](const Bytes& bytes) { return std::string(bytes.begin(), bytes.begin() + 8); };
    MetadataSnapshot base = decode_snapshot(genesis_metadata().payload);
    base.metadata_write_replicas_required = 1;
    const auto base_bytes = encode_snapshot(base);
    CHECK(magic(base_bytes) != "DHTMETB5");

    auto with = base;
    auto request = sample_torrent_request(std::string(32, 'd'), std::string(40, '3'), 100);
    request.claim = TorrentClaim{random_node_id(), 1, 150};
    with.torrent_requests[request.id] = request;
    const auto with_bytes = encode_snapshot(with);
    CHECK(magic(with_bytes) == "DHTMETB5");
    CHECK(decode_snapshot(with_bytes).torrent_requests == with.torrent_requests);
    CHECK(encode_snapshot(decode_snapshot(with_bytes)) == with_bytes);

    const auto added = metadata_delta(base, with);
    REQUIRE(added.has_value());
    CHECK(added->upsert_torrent_requests.size() == 1);
    const auto added_bytes = encode_metadata_delta(*added);
    CHECK(magic(added_bytes) == "DHTMDLT9");
    CHECK(encode_snapshot(apply_metadata_delta(base, decode_metadata_delta(added_bytes))) == with_bytes);

    auto changed = with;
    changed.torrent_requests[request.id].phase = TorrentPhase::downloading;
    changed.torrent_requests[request.id].phase_epoch = 1;
    const auto update = metadata_delta(with, changed);
    REQUIRE(update.has_value());
    CHECK(encode_snapshot(apply_metadata_delta(with, decode_metadata_delta(encode_metadata_delta(*update)))) ==
          encode_snapshot(changed));

    // Erased again: the snapshot returns to its pre-request encoding.
    const auto erased = metadata_delta(changed, base);
    REQUIRE(erased.has_value());
    CHECK(erased->erase_torrent_requests == std::vector<std::string>{request.id});
    CHECK(encode_snapshot(apply_metadata_delta(changed, decode_metadata_delta(encode_metadata_delta(*erased)))) ==
          base_bytes);

    // A delta that does not touch the collection is still DLT5-8.
    auto unrelated = with;
    unrelated.mutation_sequences[random_node_id()] = 5;
    const auto quiet = metadata_delta(with, unrelated);
    REQUIRE(quiet.has_value());
    CHECK(magic(encode_metadata_delta(*quiet)) != "DHTMDLT9");
    CHECK(encode_snapshot(apply_metadata_delta(with, decode_metadata_delta(encode_metadata_delta(*quiet)))) ==
          encode_snapshot(unrelated));

    // Tree-backed: SM16, and SM14 while empty.
    auto tree = with;
    tree.entries.clear();
    tree.namespace_root = object_id(pattern(64));
    const auto tree_bytes = encode_snapshot_v14(tree);
    CHECK(magic(tree_bytes) == "DHTMETB6");
    CHECK(decode_snapshot(tree_bytes).torrent_requests == tree.torrent_requests);
    tree.torrent_requests.clear();
    CHECK(magic(encode_snapshot_v14(tree)) == "DHTMETB4");

    // Reconciliation joins the collection instead of recording a conflict.
    auto left = with;
    left.torrent_requests[request.id].desired = TorrentDesired::paused;
    left.torrent_requests[request.id].desired_changed_unix_ms = 300;
    auto right = with;
    right.torrent_requests[request.id].phase = TorrentPhase::downloading;
    right.torrent_requests[request.id].phase_epoch = 1;
    right.torrent_requests[request.id].progress_unix_ms = 400;
    Hash256 left_head{}, right_head{};
    left_head.bytes[0] = 1;
    right_head.bytes[0] = 2;
    const auto merged = merge_metadata_heads(left, right, left_head, right_head);
    CHECK(merged.conflicts_created == 0);
    const auto& joined = merged.snapshot.torrent_requests.at(request.id);
    CHECK(joined.desired == TorrentDesired::paused);
    CHECK(joined.phase == TorrentPhase::downloading);
}
