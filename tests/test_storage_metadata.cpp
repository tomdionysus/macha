// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include <optional>
#include <string_view>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

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

        // A removal may clear only claims which are causally visible in its
        // metadata mutation clock. A concurrent branch claim from B therefore
        // survives an A-only delete context.
        retention.retain(RetentionClass::data, object, {origin_b, 1});
        std::vector<ObjectId> no_live;
        CHECK(retention.release_unreferenced(RetentionClass::data, no_live,
                                             RetentionClock{{origin_a, 1}}, 16) == 1);
        CHECK(retention.retained(RetentionClass::data, object));

        // Once a reconciled metadata view has observed both branch dots the
        // now-unreferenced object may lose both claims.
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
        // Compaction must preserve exactly the same causal state while bounding
        // the append-only foreground journal.
        CHECK(retention.compact_if_needed(1));
        CHECK(!retention.compact_if_needed(1));
    }

    // Claim and remove contexts are crash/restart state, not process-local GC
    // hints. Reopening must preserve both the live later DATA claim and CONTROL
    // claim, as well as the tombstone which suppresses a delayed A:1 replay.
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

    // The first two tombstones are still backed by physical objects. A fixed
    // budget must nevertheless make forward progress to the later dead rows,
    // rather than restarting at map.begin() forever.
    auto exists = [&](const ObjectId& id) { return id == objects[0] || id == objects[1]; };
    CHECK(retention.prune_unclaimed(RetentionClass::data, exists, 2) == 0);
    CHECK(retention.prune_unclaimed(RetentionClass::data, exists, 2) == 2);

    // A pruned causal tombstone no longer suppresses an ancient replay. This is
    // a useful externally visible probe that the later row was actually erased.
    retention.retain(RetentionClass::data, objects[2], {origin, 1});
    CHECK(retention.retained(RetentionClass::data, objects[2]));

    // The protected leading row was examined but retained, so its remove clock
    // must still suppress the same old dot.
    retention.retain(RetentionClass::data, objects[0], {origin, 1});
    CHECK(!retention.retained(RetentionClass::data, objects[0]));
}

MACHA_FAST_TEST("storage_metadata", test_retention_checkpoint_is_hash_sharded_and_restartable) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto state = t.path() / "state";
    const auto origin = random_node_id();
    std::vector<ObjectId> objects;
    objects.reserve(512);
    for (uint16_t shard = 0; shard < 256; ++shard) {
        for (uint16_t suffix = 0; suffix < 2; ++suffix) {
            ObjectId id{};
            id.bytes[0] = static_cast<uint8_t>(shard);
            id.bytes[30] = static_cast<uint8_t>(suffix);
            id.bytes[31] = static_cast<uint8_t>(255 - shard);
            objects.push_back(id);
        }
    }

    {
        RetentionStore retention(state, keys.storage);
        retention.retain_batch(RetentionClass::data, objects, {origin, 7});
        REQUIRE(retention.compact_if_needed(1));
    }

    const auto manifest = state / "retention" / "claims.current";
    REQUIRE(std::filesystem::exists(manifest));
    CHECK(!std::filesystem::exists(state / "retention" / "claims.meta"));
    CHECK(std::filesystem::file_size(state / "retention" / "claims.log") == 0);

    std::ifstream in(manifest);
    std::string generation;
    std::getline(in, generation);
    REQUIRE(!generation.empty());
    const auto generation_path = state / "retention" / "checkpoints" / generation;
    size_t shards = 0;
    for (const auto& entry : std::filesystem::directory_iterator(generation_path))
        if (entry.is_regular_file())
            ++shards;
    CHECK(shards == 256);

    RetentionStore reopened(state, keys.storage);
    for (const auto& id : objects)
        CHECK(reopened.retained(RetentionClass::data, id));
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

    // Metadata has now causally observed and removed the only reference. The
    // claim can disappear first; only then may ordinary physical GC reclaim it.
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

    // Regression for the Raspberry Pi OOM: retaining many MetadataRecord values
    // must retain one immutable namespace payload, not one payload allocation per
    // record. The pointer identity check is portable; Linux additionally guards
    // the process-level resident-memory consequence.
    record.payload = Bytes(8 * 1024 * 1024, 0xa5);
    const auto large_backing = record.payload.data();
    const auto rss_before = process_rss_kib();
    std::vector<MetadataRecord> copies(64, record);
    for (const auto& retained : copies)
        CHECK(retained.payload.data() == large_backing);
    if (rss_before) {
        const auto rss_after = process_rss_kib();
        REQUIRE(rss_after.has_value());
        // A deep-copy regression would add roughly 512 MiB here. Leave ample
        // allocator/test-runner headroom while still failing that failure mode.
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

    // Journal replay must reproduce every state transition that seed() accepted
    // while the process was live. This specifically guards the replacement-node
    // restart path exercised by test_three_node_cluster.
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

    auto left_snapshot = base;
    left_snapshot.entries["/left"] = directory;
    FsEntry left_conflict;
    left_conflict.type = EntryType::file;
    left_conflict.mode = 0644;
    left_conflict.size = 11;
    left_conflict.version = 1;
    left_snapshot.entries["/same"] = left_conflict;
    left_snapshot.catalogue_root = object_id(pattern(111));

    auto right_snapshot = base;
    right_snapshot.entries["/right"] = directory;
    auto right_conflict = left_conflict;
    right_conflict.size = 22;
    right_conflict.version = 2;
    right_snapshot.entries["/same"] = right_conflict;
    right_snapshot.catalogue_root = object_id(pattern(222));

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

    const auto merged =
        merge_metadata_snapshots(base, left_snapshot, right_snapshot, left.hash, right.hash);
    CHECK(merged.snapshot.entries.contains("/left"));
    CHECK(merged.snapshot.entries.contains("/right"));
    CHECK(!merged.snapshot.entries.contains("/same"));
    CHECK(!merged.snapshot.catalogue_root.has_value());
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

    auto make_child = [&](std::string name) {
        auto snapshot = decode_snapshot(genesis.payload);
        snapshot.metadata_write_replicas_required = 2;
        snapshot.entries[std::move(name)] = directory;
        MetadataRecord record;
        record.generation = genesis.generation + 1;
        record.previous = genesis.hash;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    const auto left = make_child("/left");
    const auto right = make_child("/right");
    REQUIRE(left.hash != right.hash);
    CHECK(decode_snapshot(left.payload).metadata_write_replicas_required == 2);

    NodeId a{}, b{}, c{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    c.bytes[15] = 3;
    MetadataAcceptance left_accept{left.generation, left.hash, 2, {a, b}};
    MetadataAcceptance right_accept{right.generation, right.hash, 2, {b, c}};

    // The transitional SM13 `metadata_participants` roster is migration
    // bookkeeping, not an eligibility/voter set. A certificate may therefore
    // name any real metadata replica even when an old snapshot's roster does not.
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
        MetadataAcceptance understrength{left.generation, left.hash, 1, {a}};
        CHECK(!replica.accept_commit(understrength));
        CHECK(replica.diagnostics().accepted_head_persistence_writes ==
              before_acceptance.accepted_head_persistence_writes);
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

        auto merged = merge_metadata_snapshots(
            decode_snapshot(genesis.payload), decode_snapshot(left.payload),
            decode_snapshot(right.payload), left.hash, right.hash);
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

    // A globally-converged owner may establish a new local ancestry floor. The
    // sole accepted committed head remains fully reconstructable, while obsolete
    // branch history no longer consumes disk or restart RSS indefinitely.
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

        // The current head's direct predecessor exists. The missing ancestry is
        // one level deeper, matching the live failure where head-only healing
        // stopped before reaching a compacted checkpoint boundary.
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

        // A pristine node imports a valid established head whose physical
        // ancestry was compacted away. Canonical genesis is a semantic ancestor,
        // not a competing rootless branch, so adoption is immediate.
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

MACHA_FAST_TEST("storage_metadata", test_history_checkpoint_proof_only_trusted_once_committed) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "checkpoint-proof-ack";

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;

    MetadataReplica replica(path, keys.storage);
    CHECK(!replica.checkpoint_proof().has_value());

    HistoryCheckpointProof proposal;
    proposal.floor_hash = replica.committed().hash;
    proposal.floor_generation = replica.committed().generation;
    proposal.epoch.bytes[0] = 0x11;
    proposal.participants = {a, b};

    // Committing before any ack for this exact (floor_hash, epoch) is refused.
    CHECK(!replica.record_checkpoint_commit(proposal.floor_hash, proposal.epoch));
    CHECK(!replica.checkpoint_proof().has_value());

    replica.record_checkpoint_ack(proposal);
    // The ack is durably visible, but only ever as `acked` -- callers that
    // gate on authority (compact_history_if_safe() and the
    // accepted_head_is_ancestor_locked() checkpoint-floor trust rule) must
    // check status == committed themselves; this accessor does not filter.
    auto acked = replica.checkpoint_proof();
    REQUIRE(acked.has_value());
    CHECK(acked->status == HistoryCheckpointProof::Status::acked);

    // A commit for the right floor but a different epoch (a membership
    // change mid-round) is refused: no acked record matches it exactly.
    auto other_epoch = proposal.epoch;
    other_epoch.bytes[1] = 0x99;
    CHECK(!replica.record_checkpoint_commit(proposal.floor_hash, other_epoch));
    CHECK(replica.checkpoint_proof()->status == HistoryCheckpointProof::Status::acked);

    REQUIRE(replica.record_checkpoint_commit(proposal.floor_hash, proposal.epoch));
    auto proof = replica.checkpoint_proof();
    REQUIRE(proof.has_value());
    CHECK(proof->status == HistoryCheckpointProof::Status::committed);
    CHECK(proof->floor_hash == proposal.floor_hash);
    CHECK(proof->epoch == proposal.epoch);
}

MACHA_FAST_TEST("storage_metadata", test_history_checkpoint_proof_reload_validates_against_committed) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "checkpoint-proof-reload";
    NodeId a{};
    a.bytes[15] = 1;

    MetadataRecord committed_at_commit;
    {
        MetadataReplica replica(path, keys.storage);
        committed_at_commit = replica.committed();
        HistoryCheckpointProof proposal;
        proposal.floor_hash = committed_at_commit.hash;
        proposal.floor_generation = committed_at_commit.generation;
        proposal.epoch.bytes[0] = 0x42;
        proposal.participants = {a};
        replica.record_checkpoint_ack(proposal);
        REQUIRE(replica.record_checkpoint_commit(proposal.floor_hash, proposal.epoch));
    }
    {
        // A restart must validate the proof before using it: it still
        // matches the current committed head here, so it survives.
        MetadataReplica reopened(path, keys.storage);
        auto proof = reopened.checkpoint_proof();
        REQUIRE(proof.has_value());
        CHECK(proof->status == HistoryCheckpointProof::Status::committed);
        CHECK(proof->floor_hash == committed_at_commit.hash);
    }

    // Advance committed_ past the proof's floor via an ordinary mutation.
    {
        MetadataReplica replica(path, keys.storage);
        auto snapshot = decode_snapshot(replica.committed().payload);
        FsEntry directory;
        directory.type = EntryType::directory;
        directory.mode = 0755;
        snapshot.entries["/advanced"] = directory;
        MetadataRecord advanced;
        advanced.generation = replica.committed().generation + 1;
        advanced.previous = replica.committed().hash;
        advanced.payload = encode_snapshot(snapshot);
        advanced.hash = metadata_hash(advanced.generation, advanced.previous, advanced.payload);
        REQUIRE(replica.store_commit(advanced));
        REQUIRE(replica.accept_commit({advanced.generation, advanced.hash, 0, {}}));
    }
    {
        // The proof's floor_hash no longer matches committed_ -- a restart
        // must never trust it, not even partially.
        MetadataReplica reopened(path, keys.storage);
        CHECK(!reopened.checkpoint_proof().has_value());
    }
}

MACHA_FAST_TEST("storage_metadata", test_history_checkpoint_proof_corrupt_file_ignored_on_restart) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "checkpoint-proof-corrupt";
    NodeId a{};
    a.bytes[15] = 1;

    MetadataRecord committed_at_commit;
    {
        MetadataReplica replica(path, keys.storage);
        committed_at_commit = replica.committed();
        HistoryCheckpointProof proposal;
        proposal.floor_hash = committed_at_commit.hash;
        proposal.floor_generation = committed_at_commit.generation;
        proposal.epoch.bytes[0] = 0x07;
        proposal.participants = {a};
        replica.record_checkpoint_ack(proposal);
        REQUIRE(replica.record_checkpoint_commit(proposal.floor_hash, proposal.epoch));
    }
    const auto proof_path = path / "metadata" / "checkpoint-proof.meta";
    REQUIRE(std::filesystem::exists(proof_path));
    {
        std::ofstream corrupt(proof_path, std::ios::binary | std::ios::trunc);
        corrupt << "not a valid checkpoint proof file";
    }

    MetadataReplica reopened(path, keys.storage);
    CHECK(!reopened.checkpoint_proof().has_value());
    // A corrupt, unrelated proof file must never disturb ordinary replica
    // startup or state.
    CHECK(reopened.committed().hash == committed_at_commit.hash);
}

MACHA_FAST_TEST("storage_metadata",
                test_returning_node_adopts_head_beyond_pruned_ancestry_only_with_committed_proof) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);

    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;

    const auto genesis = genesis_metadata();
    auto floor_snapshot = decode_snapshot(genesis.payload);
    floor_snapshot.metadata_write_replicas_required = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    floor_snapshot.entries["/floor"] = directory;

    MetadataRecord floor;
    floor.generation = genesis.generation + 1;
    floor.previous = genesis.hash;
    floor.payload = encode_snapshot(floor_snapshot);
    floor.hash = metadata_hash(floor.generation, floor.previous, floor.payload);
    const MetadataAcceptance floor_accept{floor.generation, floor.hash, 2, {a, b}};

    // A later cluster head descending from `floor` via mutations this node
    // never witnessed, itself already compacted a second time on the peer
    // side: its recorded direct predecessor is some further intermediate
    // record this node never had either, not `floor` itself. This is the
    // real gap -- history_is_ancestor_locked() already walks a single
    // compacted hop's *direct* previous hash even when previous_known is
    // false (see its own comment), so a one-hop-removed compacted root is
    // not actually the unresolvable case. Two hops (an intermediate this
    // node never received, itself pruned away too) is.
    Hash256 pruned_intermediate{};
    pruned_intermediate.bytes[0] = 0x77;
    auto beyond_snapshot = floor_snapshot;
    beyond_snapshot.entries["/beyond"] = directory;
    MetadataRecord beyond;
    beyond.generation = floor.generation + 5;
    beyond.previous = pruned_intermediate;
    beyond.payload = encode_snapshot(beyond_snapshot);
    beyond.hash = metadata_hash(beyond.generation, beyond.previous, beyond.payload);
    const MetadataAcceptance beyond_accept{beyond.generation, beyond.hash, 2, {a, b}};

    MetadataHistoryEntry beyond_compacted_root;
    beyond_compacted_root.generation = beyond.generation;
    beyond_compacted_root.previous = beyond.previous;
    beyond_compacted_root.hash = beyond.hash;
    beyond_compacted_root.previous_known = false;
    beyond_compacted_root.body = MetadataHistoryEntry::Body::full;
    beyond_compacted_root.payload.assign(beyond.payload.begin(), beyond.payload.end());

    // Negative case: a replica with no checkpoint proof for `floor` must
    // never silently adopt `beyond` on generation alone -- that is exactly
    // the defect a prior incident was caused by. It stays genuinely
    // divergent, awaiting real reconciliation.
    {
        MetadataReplica replica(t.path() / "no-proof", keys.storage);
        REQUIRE(replica.store_commit(floor));
        REQUIRE(replica.accept_commit(floor_accept));
        REQUIRE(replica.import_history(beyond_compacted_root));
        REQUIRE(replica.accept_commit(beyond_accept));
        auto heads = replica.accepted_heads();
        CHECK(heads.size() == 2);
        CHECK(!replica.history_common_ancestor(floor.hash, beyond.hash).has_value());
    }

    // Positive case: a replica that itself durably committed a checkpoint
    // proof for exactly `floor` trusts it as a universal ancestor of
    // anything that now materialises at a later generation, exactly like
    // genesis -- because reaching that proof already required every then-
    // known participant, including this replica, to agree floor was the
    // cluster's sole accepted head.
    {
        MetadataReplica replica(t.path() / "with-proof", keys.storage);
        REQUIRE(replica.store_commit(floor));
        REQUIRE(replica.accept_commit(floor_accept));

        HistoryCheckpointProof proposal;
        proposal.floor_hash = floor.hash;
        proposal.floor_generation = floor.generation;
        proposal.epoch.bytes[0] = 0x5A;
        proposal.participants = {a, b};
        replica.record_checkpoint_ack(proposal);
        REQUIRE(replica.record_checkpoint_commit(proposal.floor_hash, proposal.epoch));

        REQUIRE(replica.import_history(beyond_compacted_root));
        REQUIRE(replica.accept_commit(beyond_accept));
        auto heads = replica.accepted_heads();
        REQUIRE(heads.size() == 1);
        CHECK(heads.front().hash == beyond.hash);
    }
}

MACHA_FAST_TEST("storage_metadata", test_manual_causal_metadata_repair_requires_strict_dominance) {
    NodeId a{}, b{};
    a.bytes[15] = 1;
    b.bytes[15] = 2;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;

    auto left_snapshot = decode_snapshot(genesis_metadata().payload);
    left_snapshot.metadata_write_replicas_required = 2;
    left_snapshot.mutation_sequences[a] = 7;
    left_snapshot.mutation_sequences[b] = 3;
    left_snapshot.entries["/preserved"] = directory;

    auto right_snapshot = left_snapshot;
    right_snapshot.mutation_sequences[a] = 6;
    right_snapshot.entries.erase("/preserved");

    auto record_for = [](uint64_t generation, const MetadataSnapshot& snapshot,
                         uint8_t previous_seed) {
        MetadataRecord record;
        record.generation = generation;
        record.previous.bytes[0] = previous_seed;
        record.payload = encode_snapshot(snapshot);
        record.hash = metadata_hash(record.generation, record.previous, record.payload);
        return record;
    };
    const auto left = record_for(50, left_snapshot, 10);
    const auto right = record_for(40, right_snapshot, 20);
    auto plan = plan_causally_dominant_metadata_repair(left, left_snapshot,
                                                        right, right_snapshot);
    REQUIRE(plan.has_value());
    CHECK(plan->dominant_head == left.hash);
    CHECK(plan->subsumed_head == right.hash);
    CHECK(plan->record.generation == 51);
    CHECK(plan->record.previous == std::min(left.hash, right.hash));
    const auto repaired = decode_snapshot(plan->record.payload);
    CHECK(repaired.entries == left_snapshot.entries);
    CHECK(repaired.mutation_sequences == left_snapshot.mutation_sequences);
    REQUIRE(repaired.merge_parents.size() == 1);
    CHECK(repaired.merge_parents.front() == std::max(left.hash, right.hash));
    CHECK(valid_metadata_record(plan->record));

    auto concurrent = right_snapshot;
    concurrent.mutation_sequences[a] = 6;
    concurrent.mutation_sequences[b] = 4;
    const auto concurrent_record = record_for(41, concurrent, 30);
    CHECK(!plan_causally_dominant_metadata_repair(left, left_snapshot,
                                                   concurrent_record, concurrent));

    auto equal_clock = left_snapshot;
    equal_clock.entries.erase("/preserved");
    const auto equal_record = record_for(42, equal_clock, 40);
    CHECK(!plan_causally_dominant_metadata_repair(left, left_snapshot,
                                                   equal_record, equal_clock));
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

        // Legacy pre-0.19 PREPARE journal state remains readable deterministically.
        // This is storage/restart compatibility only; protocol 20 never uses
        // MetadataReplica::cas() for live distributed publication.
        REQUIRE(replica.cas(genesis.generation, genesis.hash, low.payload, &observed));
        CHECK(observed.hash == low.hash);
        CHECK(!replica.cas(genesis.generation, genesis.hash, high.payload, &observed));
        CHECK(observed.hash == low.hash);
        CHECK(replica.committed().hash == genesis.hash);
    }

    // The old PREPARE replacement sequence must still replay after upgrade so an
    // interrupted pre-0.19 journal can be recovered/migrated without data loss.
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

    // Simulate loss of the independent acceptance-proof file while retaining a
    // perfectly readable SM12 checkpoint/history. The checkpoint is recovery
    // material only; protocol 20 must never manufacture authority from it.
    REQUIRE(std::filesystem::remove(path / "metadata" / "heads.meta"));
    MetadataReplica reopened(path, keys.storage);
    CHECK(reopened.committed().hash == record.hash);
    CHECK(reopened.recovery_required());
    CHECK(reopened.accepted_heads().empty());
    CHECK(!reopened.acceptance(record.hash).has_value());
}

MACHA_FAST_TEST("storage_metadata",
                test_metadata_local_mutation_sequence_never_regresses_with_branch_state) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto path = t.path() / "mutation-sequence";
    {
        MetadataReplica replica(path, keys.storage);
        CHECK(replica.reserve_mutation_sequence(5) == 6);
        CHECK(replica.reserve_mutation_sequence(2) == 7);
    }
    MetadataReplica reopened(path, keys.storage);
    CHECK(reopened.reserve_mutation_sequence(1) == 8);
    CHECK(reopened.reserve_mutation_sequence(100) == 101);
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
    // Replaying a long delta chain must not retain every full intermediate
    // decoded namespace. Only the requested immutable result may enter the
    // cache; this is the regression for multi-gigabyte restart RSS.
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

MACHA_FAST_TEST("storage_metadata", test_metadata_conflict_resolution_is_not_resurrected_by_merge) {
    const auto genesis = genesis_metadata();
    auto base = decode_snapshot(genesis.payload);
    MetadataConflict conflict;
    conflict.kind = MetadataConflictKind::namespace_entry;
    conflict.key = "/old-conflict";
    conflict.left_head = sha256(pattern(31));
    conflict.right_head = sha256(pattern(32));
    const auto conflict_id = metadata_conflict_id(conflict);
    base.conflicts.emplace(conflict_id, conflict);

    auto left = base;
    left.conflicts.erase(conflict_id); // explicit resolution
    auto right = base;
    FsEntry directory;
    directory.type = EntryType::directory;
    directory.mode = 0755;
    right.entries["/unrelated"] = directory;

    const auto merged =
        merge_metadata_snapshots(base, left, right, sha256(pattern(41)), sha256(pattern(42)));
    CHECK(!merged.snapshot.conflicts.contains(conflict_id));
    CHECK(merged.snapshot.entries.contains("/unrelated"));
}

MACHA_FAST_TEST("storage_metadata", test_metadata_divergent_renames_become_conflicts) {
    const auto genesis = genesis_metadata();
    auto base = decode_snapshot(genesis.payload);
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.size = 123;
    file.version = 7;
    base.entries["/a"] = file;

    auto left = base;
    left.entries.erase("/a");
    left.entries["/b"] = file;
    auto right = base;
    right.entries.erase("/a");
    right.entries["/c"] = file;

    auto merged =
        merge_metadata_snapshots(base, left, right, sha256(pattern(51)), sha256(pattern(52)));
    REQUIRE(merged.snapshot.entries.contains("/a"));
    CHECK(merged.snapshot.entries.at("/a") == file);
    CHECK(!merged.snapshot.entries.contains("/b"));
    CHECK(!merged.snapshot.entries.contains("/c"));
    std::set<std::string> conflict_paths;
    for (const auto& [_, value] : merged.snapshot.conflicts)
        if (value.kind == MetadataConflictKind::namespace_entry)
            conflict_paths.insert(value.key);
    CHECK(conflict_paths.contains("/a"));
    CHECK(conflict_paths.contains("/b"));
    CHECK(conflict_paths.contains("/c"));

    // The same rename on both branches is not a conflict.
    right = base;
    right.entries.erase("/a");
    right.entries["/b"] = file;
    merged = merge_metadata_snapshots(base, left, right, sha256(pattern(61)), sha256(pattern(62)));
    CHECK(!merged.snapshot.entries.contains("/a"));
    REQUIRE(merged.snapshot.entries.contains("/b"));
    CHECK(merged.snapshot.entries.at("/b") == file);
    CHECK(merged.snapshot.conflicts.empty());

    // Move-vs-modify also preserves the ancestor and both alternatives.
    right = base;
    right.entries["/a"].size = 456;
    right.entries["/a"].version = 8;
    merged = merge_metadata_snapshots(base, left, right, sha256(pattern(71)), sha256(pattern(72)));
    REQUIRE(merged.snapshot.entries.contains("/a"));
    CHECK(merged.snapshot.entries.at("/a") == file);
    CHECK(!merged.snapshot.entries.contains("/b"));
    conflict_paths.clear();
    for (const auto& [_, value] : merged.snapshot.conflicts)
        if (value.kind == MetadataConflictKind::namespace_entry)
            conflict_paths.insert(value.key);
    CHECK(conflict_paths.contains("/a"));
    CHECK(conflict_paths.contains("/b"));
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

    // Corrupt/missing state is a migration/recovery case: fall back to one full
    // reconciliation, then recreate a trusted checkpoint for later O(1) boots.
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

    // Re-putting an identical hash reaffirms its physical age. This closes the
    // race where a new uncommitted write reuses an ancient orphan already on an owner.
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

    // Legacy SM7 snapshots remain valid on disk. Their tombstones intentionally
    // decode as legacy (no retirement time/id) and are stamped by 0.10.x GC.
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

    // DLT1 is accepted only as a persisted-journal format compatibility path.
    // New encoders emit DLT5; old journal records still replay byte-for-byte.
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

    // Legacy metadata snapshots had no catalogue-root field. Current code must read
    // them directly so an existing namespace upgrades to an empty catalogue
    // rather than requiring destructive state migration.
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
    // The journal contains two authenticated frames (prepare + commit), so for
    // deliberately tiny snapshots the fixed nonce/tag/framing overhead can be
    // larger than the snapshot itself. What matters is that growth tracks the
    // compact delta plus bounded framing, rather than embedding the full
    // successor snapshot in the ordinary mutation path.
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
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
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
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.maintenance.idle_bandwidth_fraction = 0.0;
    c2.maintenance.idle_bandwidth_fraction = 0.0;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Cluster reachability deliberately precedes DATA readiness now. Repair is
    // a storage-plane operation, so this test must wait for that plane rather
    // than treating membership activity as an implicit backend-ready signal.
    REQUIRE(s1.node().wait_local_state_ready(std::chrono::seconds{10}));
    REQUIRE(s2.node().wait_local_state_ready(std::chrono::seconds{10}));

    auto bytes = pattern(512 * 1024);
    auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    REQUIRE(!s2.node().local_store().has(id));
    std::vector<ObjectId> live{id};
    std::vector<ObjectId> universal{id};
    DistributedStore repair(s1.node());
    const auto full_lists_before = s1.node().local_store().full_list_scans();

    auto yielded =
        repair.repair_step(8ULL * 1024 * 1024, 8, &live, &universal, [] { return true; });
    CHECK(yielded.yielded);
    CHECK(!yielded.complete);
    CHECK(yielded.bytes_transferred == 0);
    CHECK(!s2.node().local_store().has(id));

    // One remote operation is enough to probe but not both probe and upload.
    // The pass must report itself incomplete rather than being mistaken for a
    // quiescent namespace simply because it transferred zero bytes.
    auto bounded = repair.repair_step(8ULL * 1024 * 1024, 1, &live, &universal);
    CHECK(!bounded.complete);
    CHECK(bounded.bytes_transferred == 0);
    CHECK(bounded.remote_operations == 1);
    CHECK(!s2.node().local_store().has(id));

    auto completed = repair.repair_step(8ULL * 1024 * 1024, 8, &live, &universal);
    CHECK(completed.bytes_transferred == bytes.size());
    CHECK(completed.complete);
    CHECK(completed.remote_operations <= 8);
    CHECK(s2.node().local_store().has(id));
    CHECK(s1.node().local_store().full_list_scans() == full_lists_before);
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
    config.metadata_min_write_replicas = 1;
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

    // DLT6 can erase, replace and append tombstones, but cannot reorder retained
    // entries. Claiming this transition is compact would reconstruct a
    // semantically equal but byte-different immutable record.
    CHECK(!metadata_delta(before, reordered).has_value());
}

MACHA_TEST("storage_metadata", test_local_metadata_store_falls_back_from_invalid_delta) {
    TestService fixture("local-delta-fallback");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;

    auto& service = fixture.start();
    MetadataManager metadata(service.node());
    auto committed = metadata.mutate_delta([](MetadataSnapshot& snapshot, MetadataDelta&) {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        snapshot.entries["/full-fallback"] = entry;
        // Deliberately omit the entry from the supplied exact delta. The local
        // replica must reject that compact body and retry the immutable full
        // record, matching the existing remote-replica safety path.
    });

    CHECK(service.filesystem().getattr("/full-fallback").type == EntryType::directory);
    auto history = service.node().metadata_replica().history_entry(committed.hash);
    REQUIRE(history.has_value());
    CHECK(history->body == MetadataHistoryEntry::Body::full);
}

} // namespace
