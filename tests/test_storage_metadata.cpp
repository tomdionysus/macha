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

        // A successful PUT acknowledgement means the named immutable replica
        // contains the requested bytes, not merely that its pathname exists.
        corrupt_object(root, id);
        REQUIRE(store.put(id, plain));
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
    StoragePool pool(state, node,
                     {{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}},
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
    pool.reconfigure({{disk1, 64ULL * 1024 * 1024},
                      {disk2, 64ULL * 1024 * 1024},
                      {disk3, 64ULL * 1024 * 1024}});
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
    pool.reconfigure({{disk1, 64ULL * 1024 * 1024},
                      {disk2, 64ULL * 1024 * 1024},
                      {disk3, 64ULL * 1024 * 1024}});
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
                         {{small_disk, 8ULL * 1024 * 1024},
                          {large_disk, 64ULL * 1024 * 1024}},
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
    const std::string unicode_file =
        unicode_dir + "/01.Clannad - Na Buachaill\xc3\xad lainn.mp3";
    FsEntry unicode_directory_entry;
    unicode_directory_entry.type = EntryType::directory;
    snap.entries[unicode_dir] = unicode_directory_entry;
    snap.entries[unicode_file] = file;
    auto garbage_id = object_id(pattern(4096));
    auto retirement_id = random_node_id();
    snap.garbage.push_back({garbage_id, 123456789, retirement_id});
    auto encoded = encode_snapshot(snap);
    auto decoded = decode_snapshot(encoded);
    CHECK(decoded.metadata_voters == snap.metadata_voters);
    CHECK(decoded.mutation_sequences == snap.mutation_sequences);
    CHECK(decoded.entries.at("/movie.mkv").size == 123);
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
    auto compact = metadata_delta(decoded, delta_target);
    REQUIRE(compact.has_value());
    auto encoded_delta = encode_metadata_delta(*compact);
    auto decoded_delta = decode_metadata_delta(encoded_delta);
    auto reconstructed = apply_metadata_delta(decoded, decoded_delta);
    CHECK(encode_snapshot(reconstructed) == encode_snapshot(delta_target));
    CHECK(encoded_delta.size() < encode_snapshot(delta_target).size());

    // DLT2 represents tombstone replacement and pruning directly. This is the
    // ordinary 0.10.x path used to stamp legacy records and bound garbage metadata.
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
    // New encoders always emit DLT2; old 0.9.x journal records still replay.
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
    legacy_seed.hash = metadata_hash(legacy_seed.generation, legacy_seed.previous,
                                     legacy_seed.payload);
    REQUIRE(legacy_journal.seed(legacy_seed));
    REQUIRE(legacy_journal.remember_current_committed(legacy_seed.generation,
                                                       legacy_seed.hash));
    MetadataRecord legacy_successor;
    REQUIRE(legacy_journal.cas_delta(legacy_seed.generation, legacy_seed.hash,
                                     old_delta.data(), &legacy_successor));
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
    // A successful vote is not yet a committed cluster checkpoint. Recovery
    // witnesses advance only after MetadataManager has observed quorum.
    CHECK(replica.committed().hash == current.hash);
    REQUIRE(replica.remember_current_committed(next.generation, next.hash));
    CHECK(replica.committed().hash == next.hash);
    CHECK(!replica.remember_current_committed(next.generation, current.hash));

    MetadataReplica reopened(replica_path, keys.storage);
    CHECK(reopened.current().hash == next.hash);
    CHECK(reopened.committed().hash == next.hash);
    CHECK(std::filesystem::exists(replica_path / "metadata" / "checkpoint.meta"));
    CHECK(std::filesystem::exists(replica_path / "metadata" / "journal.log"));

    // Ordinary 0.9 mutation is a compact delta proposal. It survives restart
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
    REQUIRE(reopened.cas_delta(reopened.current().generation, reopened.current().hash,
                               delta_bytes, &delta_next));
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

    // The persistent metadata cache is an independently encrypted committed
    // snapshot. If the primary checkpoint is damaged, it may seed startup but
    // is explicitly marked non-authoritative until quorum checkpointing clears
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
    c1.metadata_replication = c2.metadata_replication = 1;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] { return n1.membership().active().size() >= 2; }, 5s));

    auto peers = n1.membership().active();
    auto found = std::find_if(peers.begin(), peers.end(),
                              [&](const NodeInfo& peer) { return peer.id == n2.node_id(); });
    REQUIRE(found != peers.end());
    auto reply = n1.call(*found, MessageType::get_metadata_identity, {},
                         FrameType::speculative);
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
    c1.metadata_replication = c2.metadata_replication = 1;
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

    auto bytes = pattern(512 * 1024);
    auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    REQUIRE(!s2.node().local_store().has(id));
    std::vector<ObjectId> live{id};
    std::vector<ObjectId> universal{id};
    DistributedStore repair(s1.node());
    const auto full_lists_before = s1.node().local_store().full_list_scans();

    auto yielded = repair.repair_step(8ULL * 1024 * 1024, 8, &live, &universal,
                                      [] { return true; });
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
    config.metadata_replication = 1;
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

} // namespace
