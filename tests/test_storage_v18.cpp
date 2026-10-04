// SPDX-License-Identifier: GPL-3.0-or-later
#include "interposed_local_store_files.hpp"
#include "test_backend_support.hpp"
#include <limits>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

size_t regular_files_below(const std::filesystem::path& root) {
    std::error_code error;
    size_t count = 0;
    if (!std::filesystem::exists(root, error))
        return 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        std::error_code type_error;
        if (it->is_regular_file(type_error) && !type_error)
            ++count;
    }
    return count;
}

std::array<uint8_t, 32> storage_key(const TempDir& t) {
    const auto keyfile = t.path() / "key";
    write_key(keyfile);
    return load_cluster_keys(keyfile).storage;
}

LocalStoreOptions packed_options(uint64_t limit, uint64_t target_size) {
    LocalStoreOptions options;
    options.limit = limit;
    options.pack_threshold = 256 * 1024;
    options.pack_target_size = target_size;
    return options;
}

std::filesystem::path only_pack(const std::filesystem::path& store) {
    std::filesystem::path pack;
    for (const auto& entry : std::filesystem::directory_iterator(store / "packs")) {
        if (entry.is_regular_file()) {
            REQUIRE(pack.empty());
            pack = entry.path();
        }
    }
    REQUIRE(!pack.empty());
    return pack;
}

// One device operation held at its start until released, so a test can act
// while it is provably in progress.
class HeldOperation {
    std::promise<void> entered_;
    std::future<void> entered_future_ = entered_.get_future();
    std::promise<void> release_;
    std::shared_future<void> released_ = release_.get_future().share();
    std::atomic_uint64_t calls_{};
    std::atomic_bool released_once_{};
    InterposedLocalStoreFiles* files_{};

  public:
    HeldOperation() = default;
    HeldOperation(const HeldOperation&) = delete;
    HeldOperation& operator=(const HeldOperation&) = delete;
    // Releases a holder a failed check left waiting, and detaches from the device.
    ~HeldOperation() {
        release();
        if (files_)
            files_->before({});
    }

    // Holds the first call matching `op` (and `path`, if given); counts every
    // matching call.
    void hold(InterposedLocalStoreFiles& files, InterposedLocalStoreFiles::Op op,
              std::filesystem::path path = {}) {
        files_ = &files;
        files.before([this, op, path](InterposedLocalStoreFiles::Op seen,
                                      const std::filesystem::path& at) {
            if (seen != op || (!path.empty() && at != path))
                return;
            if (calls_.fetch_add(1) == 0) {
                entered_.set_value();
                released_.wait();
            }
        });
    }
    bool entered() { return entered_future_.wait_for(scaled(2s)) == std::future_status::ready; }
    void release() {
        if (!released_once_.exchange(true))
            release_.set_value();
    }
    uint64_t calls() const { return calls_.load(); }
};

template <class T>
bool ready(std::future<T>& future, std::chrono::milliseconds within = 2s) {
    return future.wait_for(within) == std::future_status::ready;
}

} // namespace

// The pack lifecycle on one store: small objects are packed, deletions are
// tombstones, compaction reclaims dead space a bounded slice at a time (and
// not at all once shutdown is requested), and a restart rebuilds the pack
// index from the pack records alone.
MACHA_FAST_TEST("storage_v18", test_packs_store_compact_and_survive_restart) {
    TempDir t;
    const auto key = storage_key(t);
    const auto options = packed_options(128ULL * 1024 * 1024, 512 * 1024);

    std::vector<std::pair<ObjectId, Bytes>> live;
    std::vector<ObjectId> removed;
    {
        LocalStore store(t.path() / "store", options, key);
        std::stop_source shutdown;
        shutdown.request_stop();
        CHECK(!store.compact_packs(shutdown.get_token()));

        for (size_t i = 0; i < 40; ++i) {
            auto data = pattern(96 * 1024 + i, static_cast<uint8_t>(17 + i));
            const auto id = object_id(data);
            REQUIRE(store.put(id, data));
            CHECK(store.is_packed(id));
            if (i % 2)
                live.emplace_back(id, std::move(data));
            else
                removed.push_back(id);
        }
        CHECK(regular_files_below(t.path() / "store" / "objects") == 0);
        CHECK(regular_files_below(t.path() / "store" / "packs") < live.size() + removed.size());

        for (const auto& id : removed)
            REQUIRE(store.remove(id));
        // Dead packs remain after one bounded victim rewrite, so a second slice
        // makes further progress; no call needs space for the whole corpus.
        const auto before = store.used();
        REQUIRE(store.compact_packs());
        const auto after_one = store.used();
        CHECK(after_one < before);
        REQUIRE(store.compact_packs());
        CHECK(store.used() < after_one);
        for (const auto& id : removed)
            CHECK(!store.has(id));
        for (const auto& [id, data] : live)
            CHECK(store.get(id) == std::optional<Bytes>{data});
    }

    LocalStore reopened(t.path() / "store", options, key);
    for (const auto& id : removed)
        CHECK(!reopened.has(id));
    for (const auto& [id, data] : live) {
        CHECK(reopened.get(id) == std::optional<Bytes>{data});
        CHECK(reopened.is_packed(id));
    }
}

// Admission: the free-space reserve refuses a put before the filesystem is
// exhausted; a pool falls through to a backend with room; r=1 logical
// capacity is the sum of the nodes, not the smallest.
MACHA_FAST_TEST("storage_v18", test_admission_respects_reserve_and_capacity) {
    TempDir t;
    const auto key = storage_key(t);
    {
        LocalStoreOptions options;
        options.limit = 64ULL * 1024 * 1024;
        options.reserve_free = std::numeric_limits<uint64_t>::max();
        options.pack_threshold = 0;
        options.pack_target_size = 0;
        LocalStore store(t.path() / "reserved", options, key);
        auto data = pattern(128 * 1024, 31);
        const auto id = object_id(data);
        CHECK(!store.put(id, data));
        CHECK(!store.has(id));
        CHECK(store.used() == 0);
    }
    {
        const auto state = t.path() / "state";
        const auto small = t.path() / "small";
        const auto large = t.path() / "large";
        std::filesystem::create_directories(small);
        std::filesystem::create_directories(large);
        StoragePackingConfig no_packing;
        no_packing.threshold = 0;
        no_packing.target_size = 0;
        StoragePool pool(state, load_or_create_node_id(state),
                         {{small, 512ULL * 1024, 0}, {large, 16ULL * 1024 * 1024, 0}}, key, 0ms,
                         no_packing);
        std::vector<ObjectId> ids;
        for (size_t i = 0; i < 40; ++i) {
            auto data = pattern(192 * 1024 + i);
            data[0] ^= static_cast<uint8_t>(i);
            const auto id = object_id(data);
            REQUIRE(pool.put(id, data));
            ids.push_back(id);
        }
        CHECK(pool.used() > 512ULL * 1024);
        CHECK(pool.limit() == 512ULL * 1024 + 16ULL * 1024 * 1024);
        for (const auto& id : ids)
            CHECK(pool.valid(id));
    }
    NodeInfo small;
    small.id.bytes[0] = 1;
    small.capacity = 1ULL * 1024 * 1024 * 1024;
    NodeInfo large;
    large.id.bytes[0] = 2;
    large.capacity = 2ULL * 1024 * 1024 * 1024 * 1024;
    CHECK(placement_logical_capacity({small, large}, 1) == small.capacity + large.capacity);
}

// Damage a power loss or a bad sector leaves in a pack. A torn or
// undecodable record at the tail is truncated and the pack stays the active
// one; an unreadable span with live records after it is skipped, reported,
// and reclaimed by compaction, and the lost object can be written again.
MACHA_FAST_TEST("storage_v18", test_pack_recovery_settles_each_kind_of_damage) {
    TempDir t;
    const auto key = storage_key(t);
    const auto options = packed_options(32ULL * 1024 * 1024, 1024 * 1024);
    // A record is this header plus ciphertext as long as the plaintext.
    constexpr uint64_t pack_header_size = 93 + 32;

    struct Tail {
        const char* what;
        Bytes garbage;
        // Shorter than a header: discarded before any header is decoded, so
        // not counted among undecodable tails.
        bool short_of_a_header;
    };
    std::vector<Tail> tails;
    tails.push_back({"incomplete record",
                     Bytes{'M', 'A', 'C', 'H', 'P', 'K', '0', '1', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                           11, 12, 13, 14, 15},
                     true});
    tails.push_back({"zero-filled header", Bytes(pack_header_size, 0), false});
    tails.push_back({"garbage header", pattern(pack_header_size, 0x5a), false});
    tails.push_back(
        {"garbage header and partial payload", pattern(pack_header_size + 40000, 0x5a), false});

    size_t index = 0;
    for (auto& tail : tails) {
        std::cerr << "row: " << tail.what << "\n";
        const auto root = t.path() / ("store-" + std::to_string(index++));
        const auto first = pattern(96 * 1024, 11);
        const auto second = pattern(96 * 1024, 12);
        {
            LocalStore store(root, options, key);
            REQUIRE(store.put(object_id(first), first));
            REQUIRE(store.put(object_id(second), second));
        }
        const auto pack = only_pack(root);
        const auto intact_size = std::filesystem::file_size(pack);
        {
            std::ofstream out(pack, std::ios::binary | std::ios::app);
            out.write(reinterpret_cast<const char*>(tail.garbage.data()),
                      static_cast<std::streamsize>(tail.garbage.size()));
            REQUIRE(out.good());
        }
        REQUIRE(std::filesystem::file_size(pack) == intact_size + tail.garbage.size());

        LocalStore reopened(root, options, key);
        CHECK(reopened.get(object_id(first)) == std::optional<Bytes>{first});
        CHECK(reopened.get(object_id(second)) == std::optional<Bytes>{second});
        CHECK(std::filesystem::file_size(pack) == intact_size);
        const auto diagnostics = reopened.diagnostics();
        CHECK(diagnostics.pack_recovery_truncated_tails == (tail.short_of_a_header ? 0 : 1));
        CHECK(diagnostics.pack_recovery_skipped_regions == 0);
        CHECK(diagnostics.pack_recovery_skipped_bytes == 0);
        // Appends continue behind the restored boundary.
        const auto third = pattern(64 * 1024, 13);
        REQUIRE(reopened.put(object_id(third), third));
        CHECK(reopened.is_packed(object_id(third)));
    }

    // A header failing its checksum with intact records after it.
    const auto root = t.path() / "store-damaged-middle";
    const std::vector<Bytes> objects{pattern(96 * 1024, 21), pattern(96 * 1024, 22),
                                     pattern(96 * 1024, 23)};
    {
        LocalStore store(root, options, key);
        for (const auto& object : objects)
            REQUIRE(store.put(object_id(object), object));
    }
    const auto pack = only_pack(root);
    const auto intact_size = std::filesystem::file_size(pack);
    const uint64_t second_offset = pack_header_size + objects[0].size();
    {
        // Flip one byte inside the second record's header checksum.
        std::fstream io(pack, std::ios::binary | std::ios::in | std::ios::out);
        io.seekg(static_cast<std::streamoff>(second_offset + 100));
        char byte = 0;
        io.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x01);
        io.seekp(static_cast<std::streamoff>(second_offset + 100));
        io.write(&byte, 1);
        REQUIRE(io.good());
    }
    LocalStore reopened(root, options, key);
    CHECK(std::filesystem::file_size(pack) == intact_size);
    CHECK(reopened.get(object_id(objects[0])) == std::optional<Bytes>{objects[0]});
    CHECK(!reopened.has(object_id(objects[1])));
    CHECK(reopened.get(object_id(objects[2])) == std::optional<Bytes>{objects[2]});
    const auto diagnostics = reopened.diagnostics();
    CHECK(diagnostics.pack_recovery_truncated_tails == 0);
    CHECK(diagnostics.pack_recovery_skipped_regions == 1);
    CHECK(diagnostics.pack_recovery_skipped_bytes == pack_header_size + objects[1].size());
    // What replica repair does: write the lost object again. Compaction then
    // reclaims the unreadable span without touching live data.
    REQUIRE(reopened.put(object_id(objects[1]), objects[1]));
    const auto before_compaction = reopened.used();
    REQUIRE(reopened.compact_packs());
    CHECK(reopened.used() < before_compaction);
    for (const auto& object : objects)
        CHECK(reopened.get(object_id(object)) == std::optional<Bytes>{object});
}

// Device I/O on one object never blocks the index lock an unrelated read
// needs; operations on the same object stay single-flight behind it; and
// compaction cannot unlink a pack a reader has leased but not yet opened.
MACHA_FAST_TEST("storage_v18", test_held_device_io_blocks_only_its_own_object) {
    TempDir t;
    const auto key = storage_key(t);
    LocalStoreOptions loose;
    loose.limit = 64ULL * 1024 * 1024;
    loose.pack_threshold = 0;
    loose.pack_target_size = 0;
    const auto packed = packed_options(64ULL * 1024 * 1024, 4 * 1024 * 1024);
    // Over the pack threshold: stored loose even in a packing store.
    const auto viewer = pattern(512 * 1024, 0x41);

    {
        // A loose loader write held at its disk write, after admission.
        InterposedLocalStoreFiles files;
        LocalStore store(t.path() / "loose-write", loose, key, LocalStoreMode::authoritative, {},
                         files);
        REQUIRE(store.put(object_id(viewer), viewer));
        const auto loader = pattern(4 * 1024 * 1024, 0x82);
        HeldOperation held;
        held.hold(files, InterposedLocalStoreFiles::Op::install,
                  store.object_path(object_id(loader)));
        auto write = std::async(std::launch::async, [&] { return store.put(object_id(loader), loader); });
        REQUIRE(held.entered());
        auto read = std::async(std::launch::async, [&] { return store.get(object_id(viewer)); });
        REQUIRE(ready(read));
        CHECK(read.get() == std::optional<Bytes>{viewer});
        auto same = std::async(std::launch::async, [&] { return store.put(object_id(loader), loader); });
        CHECK(!ready(same, 20ms));
        held.release();
        REQUIRE(ready(write));
        CHECK(write.get());
        REQUIRE(ready(same));
        CHECK(same.get());
        // The second put found the object installed and wrote nothing.
        CHECK(held.calls() == 1);
        CHECK(store.get(object_id(loader)) == std::optional<Bytes>{loader});
    }
    {
        // A packed read held at its device read.
        InterposedLocalStoreFiles files;
        LocalStore store(t.path() / "packed-read", packed, key, LocalStoreMode::authoritative, {},
                         files);
        const auto blocked = pattern(128 * 1024, 0x37);
        REQUIRE(store.put(object_id(blocked), blocked));
        REQUIRE(store.is_packed(object_id(blocked)));
        REQUIRE(store.put(object_id(viewer), viewer));
        CHECK(!store.is_packed(object_id(viewer)));
        HeldOperation held;
        held.hold(files, InterposedLocalStoreFiles::Op::read_at);
        auto first = std::async(std::launch::async, [&] { return store.get(object_id(blocked)); });
        REQUIRE(held.entered());
        auto read = std::async(std::launch::async, [&] { return store.get(object_id(viewer)); });
        REQUIRE(ready(read));
        CHECK(read.get() == std::optional<Bytes>{viewer});
        auto same = std::async(std::launch::async, [&] { return store.get(object_id(blocked)); });
        CHECK(!ready(same, 20ms));
        held.release();
        REQUIRE(ready(first));
        CHECK(first.get() == std::optional<Bytes>{blocked});
        REQUIRE(ready(same));
        CHECK(same.get() == std::optional<Bytes>{blocked});
    }
    {
        // A packed write held at its append; its reservation and ownership
        // must not hold the index mutex.
        InterposedLocalStoreFiles files;
        LocalStore store(t.path() / "packed-write", packed, key, LocalStoreMode::authoritative, {},
                         files);
        REQUIRE(store.put(object_id(viewer), viewer));
        const auto loader = pattern(128 * 1024, 0x73);
        HeldOperation held;
        held.hold(files, InterposedLocalStoreFiles::Op::append);
        auto write = std::async(std::launch::async, [&] { return store.put(object_id(loader), loader); });
        REQUIRE(held.entered());
        auto read = std::async(std::launch::async, [&] { return store.get(object_id(viewer)); });
        REQUIRE(ready(read));
        CHECK(read.get() == std::optional<Bytes>{viewer});
        auto same = std::async(std::launch::async, [&] { return store.put(object_id(loader), loader); });
        CHECK(!ready(same, 20ms));
        CHECK(held.calls() == 1);
        held.release();
        REQUIRE(ready(write));
        CHECK(write.get());
        REQUIRE(ready(same));
        CHECK(same.get());
        // The second put appends its touch only after the first write completes.
        CHECK(held.calls() == 2);
        CHECK(store.get(object_id(loader)) == std::optional<Bytes>{loader});
    }
    {
        // Compaction held at its pack listing: packed and loose reads proceed.
        InterposedLocalStoreFiles files;
        LocalStore store(t.path() / "compaction", packed, key, LocalStoreMode::authoritative, {},
                         files);
        const auto dead = pattern(128 * 1024, 0x19);
        const auto packed_viewer = pattern(128 * 1024, 0x29);
        REQUIRE(store.put(object_id(dead), dead));
        REQUIRE(store.put(object_id(packed_viewer), packed_viewer));
        REQUIRE(store.put(object_id(viewer), viewer));
        REQUIRE(store.remove(object_id(dead)));
        HeldOperation held;
        held.hold(files, InterposedLocalStoreFiles::Op::list);
        auto compaction = std::async(std::launch::async, [&] { return store.compact_packs(); });
        REQUIRE(held.entered());
        auto packed_read =
            std::async(std::launch::async, [&] { return store.get(object_id(packed_viewer)); });
        auto loose_read = std::async(std::launch::async, [&] { return store.get(object_id(viewer)); });
        REQUIRE(ready(packed_read));
        REQUIRE(ready(loose_read));
        CHECK(packed_read.get() == std::optional<Bytes>{packed_viewer});
        CHECK(loose_read.get() == std::optional<Bytes>{viewer});
        held.release();
        REQUIRE(ready(compaction));
        CHECK(compaction.get());
        CHECK(store.get(object_id(packed_viewer)) == std::optional<Bytes>{packed_viewer});
        CHECK(store.get(object_id(viewer)) == std::optional<Bytes>{viewer});
    }
    {
        // A reader holds a logical lease on a pack and has not opened it:
        // compaction may switch to the replacement but cannot unlink the
        // victim until that reader has finished.
        InterposedLocalStoreFiles files;
        LocalStore store(t.path() / "lease", packed, key, LocalStoreMode::authoritative, {}, files);
        const auto live = pattern(128 * 1024, 0x51);
        const auto dead = pattern(128 * 1024, 0x61);
        REQUIRE(store.put(object_id(live), live));
        REQUIRE(store.put(object_id(dead), dead));
        REQUIRE(store.remove(object_id(dead)));
        HeldOperation held;
        held.hold(files, InterposedLocalStoreFiles::Op::read_at);
        auto read = std::async(std::launch::async, [&] { return store.get(object_id(live)); });
        REQUIRE(held.entered());
        auto compaction = std::async(std::launch::async, [&] { return store.compact_packs(); });
        CHECK(!ready(compaction, 20ms));
        held.release();
        REQUIRE(ready(read));
        CHECK(read.get() == std::optional<Bytes>{live});
        REQUIRE(ready(compaction));
        CHECK(compaction.get());
        CHECK(store.get(object_id(live)) == std::optional<Bytes>{live});
    }
}

// Presence is known without reading or decrypting an object: a restarted
// store learns what it holds from object names, has() touches no device, and
// an empty object file (a crash or external truncation) is pruned on read.
MACHA_FAST_TEST("storage_v18", test_presence_is_known_without_reading_objects) {
    TempDir t;
    const auto key = storage_key(t);
    const auto options = packed_options(64ULL * 1024 * 1024, 1024 * 1024);
    const auto root = t.path() / "store";
    const auto loose = pattern(512 * 1024, 0x71);
    const auto packed = pattern(64 * 1024, 0x72);
    const auto truncated = pattern(512 * 1024, 0x73);
    std::vector<ObjectId> more_loose;
    std::filesystem::path truncated_path;
    {
        InterposedLocalStoreFiles files;
        LocalStore store(root, options, key, LocalStoreMode::authoritative, nullptr, files);
        REQUIRE(store.put(object_id(loose), loose));
        REQUIRE(store.put(object_id(packed), packed));
        REQUIRE(!store.is_packed(object_id(loose)));
        REQUIRE(store.is_packed(object_id(packed)));
        for (int i = 0; i < 12; ++i) {
            auto data = pattern(300 * 1024, static_cast<uint8_t>(0x30 + i));
            REQUIRE(store.put(object_id(data), data));
            more_loose.push_back(object_id(data));
        }

        std::atomic_bool loose_read{false};
        std::atomic_bool packed_read{false};
        files.before([&](InterposedLocalStoreFiles::Op op, const std::filesystem::path&) {
            if (op == InterposedLocalStoreFiles::Op::read)
                loose_read = true;
            if (op == InterposedLocalStoreFiles::Op::read_at)
                packed_read = true;
        });
        // The recorded device reads prove has() reads nothing, not just its answer.
        CHECK(store.has(object_id(loose)));
        CHECK(store.has(object_id(packed)));
        CHECK(!store.has(ObjectId{}));
        CHECK(!loose_read.load());
        CHECK(!packed_read.load());
        CHECK(store.get(object_id(loose)).has_value());
        CHECK(store.get(object_id(packed)).has_value());
        CHECK(loose_read.load());
        CHECK(packed_read.load());
        files.before({});

        REQUIRE(store.put(object_id(truncated), truncated));
        truncated_path = store.object_path(object_id(truncated));
    }
    {
        std::ofstream truncate(truncated_path, std::ios::binary | std::ios::trunc);
        REQUIRE(truncate.good());
    }

    LocalStore reopened(root, options, key);
    // Every loose object name, including the truncated file's.
    REQUIRE(wait_until([&] {
        return reopened.diagnostics().presence_index_entries == more_loose.size() + 2;
    }));
    for (const auto& id : more_loose)
        CHECK(reopened.has(id));
    CHECK(reopened.has(object_id(loose)));
    CHECK(reopened.has(object_id(packed)));
    CHECK(!reopened.has(ObjectId{}));
    // Presence is not stat'ed, so the empty file is found on read, which
    // prunes it and reports the object absent.
    CHECK(!reopened.get(object_id(truncated)).has_value());
    CHECK(!std::filesystem::exists(truncated_path));
    CHECK(!reopened.has(object_id(truncated)));
    REQUIRE(reopened.put(object_id(truncated), truncated));
    CHECK(reopened.has(object_id(truncated)));
}

// Kept integrated: the separate budget is LocalServices' wiring of the
// control store beside the DATA pool, not a LocalStore property.
MACHA_TEST("storage_v18", test_metadata_control_store_is_independent_of_data_quota) {
    TestNode fixture("metadata-priority");
    auto& config = fixture.config();
    config.storage_backends = {{fixture.path() / "data", 512ULL * 1024, 0}};
    config.storage_packing.threshold = 0;
    config.storage_packing.target_size = 0;
    config.metadata_store.path = fixture.path() / "control";
    config.metadata_store.limit = 8ULL * 1024 * 1024;
    std::filesystem::create_directories(config.storage_backends.front().path);
    fixture.prepare();
    fixture.start();

    // Exhaust ordinary DATA admission.
    size_t stored = 0;
    for (size_t i = 0; i < 16; ++i) {
        auto data = pattern(128 * 1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        if (!fixture.node().local_store().put(object_id(data), data))
            break;
        ++stored;
    }
    CHECK(stored > 0);
    auto extra = pattern(256 * 1024);
    CHECK(!fixture.node().local_store().put(object_id(extra), extra));

    // Control-plane durability must still have its own admission budget.
    auto control = pattern(128 * 1024 + 17);
    auto control_id = object_id(control);
    REQUIRE(fixture.node().control_store().put(control_id, control));
    CHECK(fixture.node().control_store().get(control_id) == std::optional<Bytes>{control});
}

// State from before the versioned layout: a non-empty unversioned state
// namespace refuses to start; a non-empty unversioned DATA backend leaves the
// node up with that backend offline.
MACHA_TEST("storage_v18", test_unversioned_state_is_refused) {
    TestCluster cluster(ConfigProfile::isolated);
    {
        auto config = cluster.node_config("old-state", free_port());
        config.storage_backends = {{cluster.path() / "old-state.data", 8ULL * 1024 * 1024, 0}};
        config.storage_packing.threshold = 0;
        config.storage_packing.target_size = 0;
        config.metadata_store.path = cluster.path() / "old-state.control";
        std::filesystem::create_directories(config.state_path);
        std::ofstream(config.state_path / "metadata.bin") << "old namespace";
        bool refused = false;
        try {
            BareNode node(config, cluster.keys());
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find("fresh namespace") != std::string::npos;
        }
        CHECK(refused);
    }
    {
        auto config = cluster.node_config("fresh-boundary", free_port());
        config.storage_backends = {{cluster.path() / "old-data", 8ULL * 1024 * 1024, 0}};
        config.storage_packing.threshold = 0;
        config.storage_packing.target_size = 0;
        config.metadata_store.path = cluster.path() / "fresh-boundary.control";
        std::filesystem::create_directories(config.storage_backends.front().path / "objects");
        std::ofstream(config.storage_backends.front().path / "objects" / "legacy")
            << "old namespace";
        BareNode node(config, cluster.keys());
        node.start();
        REQUIRE(node.wait_local_state_ready(10s));
        CHECK(node.readiness().control_plane_online);
        CHECK(node.readiness().data_storage_ready);
        CHECK(node.local_store().online_backends() == 0);
        CHECK(node.local_store().limit() == 0);
        node.stop();
    }
}
namespace {
class StorageClusterNode {
    Config config_;
    const ClusterKeys& keys_;
    std::unique_ptr<BareNode> node_;
    std::unique_ptr<DistributedStore> store_;
    std::unique_ptr<MetadataManager> metadata_;
    std::unique_ptr<CatalogueManager> catalogue_;
    bool started_{};

  public:
    StorageClusterNode(Config config, const ClusterKeys& keys)
        : config_(std::move(config)), keys_(keys) {}
    ~StorageClusterNode() {
        if (started_ && node_) {
            try { node_->stop(); } catch (...) {}
        }
    }
    Config& config() { return config_; }
    void prepare() {
        REQUIRE(!node_);
        for (const auto& backend : config_.storage_backends)
            std::filesystem::create_directories(backend.path);
        if (!config_.metadata_store.path.empty())
            std::filesystem::create_directories(config_.metadata_store.path);
        node_ = std::make_unique<BareNode>(config_, keys_);
    }
    void start() {
        if (!node_) prepare();
        node_->start();
        started_ = true;
        REQUIRE(node_->wait_local_state_ready(10s));
        store_ = std::make_unique<DistributedStore>(*node_, node_->local_state(), node_->resources.activity,
                                                    node_->resources.data, node_->resources.memory, node_->resources.events);
        metadata_ = std::make_unique<MetadataManager>(*node_, node_->local_state(), node_->metadata_server());
        catalogue_ = std::make_unique<CatalogueManager>(*node_, node_->local_state(), node_->metadata_server(), *store_, *metadata_, node_->ledger());
    }
    // A process restart in miniature: a fresh NodeRuntime over the same
    // on-disk state gets a fresh durability epoch and fresh backend instance
    // ids, exactly what a peer sees after `systemctl restart`.
    void restart() {
        stop();
        start();
    }
    // Take the node down and leave it down: what a peer sees mid-restart.
    void stop() {
        REQUIRE(node_);
        catalogue_.reset();
        metadata_.reset();
        store_.reset();
        if (started_)
            node_->stop();
        node_.reset();
        started_ = false;
    }
    BareNode& node() { REQUIRE(node_); return *node_; }
    LocalState& local_state() { REQUIRE(node_); return node_->local_state(); }
    DistributedStore& store() { REQUIRE(store_); return *store_; }
    MetadataManager& metadata() { REQUIRE(metadata_); return *metadata_; }
    CatalogueManager& catalogue() { REQUIRE(catalogue_); return *catalogue_; }
};

Config storage_node_config(const TestCluster& cluster, std::string_view name, uint16_t port,
                           uint64_t data_limit, size_t replicas, size_t metadata_replicas,
                           std::vector<Endpoint> bootstrap = {}, size_t min_write_replicas = 1) {
    auto config = cluster.node_config(name, port, std::move(bootstrap));
    config.replication = replicas;
    config.min_write_replicas = min_write_replicas;
    config.metadata_min_write_replicas = metadata_replicas;
    // Loopback test nodes are independent storage failure domains even though
    // they share 127.0.0.1 as their transport host.
    config.failure_domain = std::string(name);
    config.storage_backends = {{cluster.path() / std::string(name), data_limit, 0}};
    config.storage_packing.threshold = 0;
    config.storage_packing.target_size = 0;
    config.metadata_store.path = cluster.path() / (std::string(name) + ".control");
    config.metadata_store.limit = 32ULL * 1024 * 1024;
    config.metadata_store.packing.threshold = 256 * 1024;
    config.metadata_store.packing.target_size = 1024 * 1024;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    return config;
}

Bytes preferred_for(NodeRuntime& observer, const NodeId& preferred, size_t bytes, uint8_t salt_start = 0) {
    // Placement uses the observer's active membership, so the preferred node
    // must be active there before any candidate can prefer it.
    REQUIRE(wait_until([&] {
        const auto active = observer.membership().active();
        return std::any_of(active.begin(), active.end(),
                           [&](const NodeInfo& node) { return node.id == preferred; });
    }, 10s));
    // The whole counter is written into the object so all 4096 candidates are
    // distinct, enough to find a node with a small capacity weight.
    auto data = pattern(bytes, salt_start);
    for (uint32_t i = 0; i < 4096; ++i) {
        for (size_t b = 0; b < 4 && b < data.size(); ++b)
            data[b] = static_cast<uint8_t>(i >> (8 * b));
        const auto id = object_id(data);
        auto ranked = capacity_placement_nodes(id.bytes, observer.membership().active(), 1);
        if (!ranked.empty() && ranked.front().id == preferred)
            return data;
    }
    throw std::runtime_error("could not find object preferring requested node");
}

void fill_data_store(StoragePool& store, size_t object_bytes = 96 * 1024) {
    for (unsigned i = 0; i < 4096; ++i) {
        auto data = pattern(object_bytes, static_cast<uint8_t>(i));
        data[0] ^= static_cast<uint8_t>(i * 17U);
        if (!store.put(object_id(data), data))
            return;
    }
    throw std::runtime_error("test data store did not reach configured admission limit");
}
} // namespace

// Kept integrated: placement across two real nodes' memberships and the
// catalogue's split between control and DATA storage on both.
//
// A full preferred node: an r=1 DATA put spills to the next candidate, and
// artwork follows it; the catalogue's control objects ignore the DATA quota
// and land on both metadata replicas; the small node reads the artwork back.
MACHA_TEST("storage_v18", test_a_full_preferred_node_spills_data_but_not_control) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto small_port = free_port();
    const auto large_port = free_port();
    StorageClusterNode small(storage_node_config(cluster, "small", small_port, 384ULL * 1024, 1, 2),
                             cluster.keys());
    StorageClusterNode large(storage_node_config(cluster, "large", large_port,
                                                 16ULL * 1024 * 1024, 1, 2,
                                                 {{"127.0.0.1", small_port}}),
                             cluster.keys());
    small.start();
    large.start();
    REQUIRE(wait_until([&] {
        return small.node().membership().active().size() == 2 &&
               large.node().membership().active().size() == 2;
    }));
    REQUIRE(wait_until([&] {
        try {
            return large.metadata().snapshot().metadata_voters.empty();
        } catch (...) {
            return false;
        }
    }));

    fill_data_store(small.local_state().data());
    const auto data = preferred_for(large.node(), small.node().node_id(), 128 * 1024, 77);
    const auto id = object_id(data);
    REQUIRE(large.store().put(id, data));
    CHECK(!small.local_state().data().has(id));
    CHECK(large.local_state().data().has(id));
    CHECK(large.store().get(id) == std::optional<Bytes>{data});

    const auto art_bytes = preferred_for(large.node(), small.node().node_id(), 128 * 1024, 123);
    auto art = large.catalogue().stage_artwork("poster", "image/jpeg", art_bytes);
    CHECK(!small.node().local_store().has(art.id));
    CHECK(large.node().local_store().has(art.id));

    CatalogueItem item;
    item.id = "tmdb:movie:1";
    item.kind = CatalogueKind::movie;
    item.title = "Storage Contract";
    item.artwork.push_back(art);
    CHECK(large.catalogue().upsert(item).id == item.id);

    const auto metadata = large.metadata().snapshot();
    REQUIRE(metadata.catalogue_root.has_value());
    CHECK(small.node().control_store().has(*metadata.catalogue_root));
    CHECK(large.node().control_store().has(*metadata.catalogue_root));
    CHECK(!small.node().local_store().has(*metadata.catalogue_root));
    CHECK(!large.node().local_store().has(*metadata.catalogue_root));

    REQUIRE(small.catalogue().get(item.id).has_value());
    auto fetched = small.catalogue().artwork(art.id);
    REQUIRE(fetched.has_value());
    CHECK(fetched->bytes == art_bytes);
}

// Repair does no work that cannot complete, and says what it cannot do: an
// object the survey marks unavailable is passed without a fetch, and a live
// object no peer can supply is counted and sampled once, never a held one.
MACHA_TEST("storage_v18", test_repair_passes_unavailable_and_counts_unsourceable_objects) {
    TestCluster cluster;
    StorageClusterNode node(storage_node_config(cluster, "lonely", free_port(), 8ULL * 1024 * 1024,
                                                2, 1),
                            cluster.keys());
    node.start();

    const auto present_bytes = pattern(64 * 1024, 7);
    const auto present = object_id(present_bytes);
    REQUIRE(node.store().put(present, present_bytes));
    const auto missing = object_id(pattern(64 * 1024, 9));
    REQUIRE(!node.local_state().data().has(missing));
    std::vector<ObjectId> live{missing};

    bool marked = true;
    std::vector<ObjectId> asked;
    const auto unavailable = [&](const ObjectId& id) {
        asked.push_back(id);
        return marked;
    };
    const auto step = [&] {
        return node.store().repair_step(4ULL * 1024 * 1024, 16, live, {}, 0, unavailable);
    };

    // A whole pass over the marked object examines it and attempts nothing.
    const auto passed = step();
    CHECK(asked == std::vector<ObjectId>{missing});
    CHECK(passed.pull_examined == 1);
    CHECK(passed.bytes_transferred == 0);
    CHECK(node.store().repair_diagnostics().pull_unsourceable == 0);

    // Unmarked, the pass tries to source it and, with no peer, cannot.
    marked = false;
    REQUIRE(wait_until([&] {
        (void)step();
        return node.store().repair_diagnostics().pull_unsourceable > 0;
    }));
    // With a held object beside it in the live set (sorted: repair
    // binary-searches it), only the missing one is unsourceable.
    live = {present, missing};
    std::sort(live.begin(), live.end());
    node.store().repair_once(4ULL * 1024 * 1024, live);
    const auto after = node.store().repair_diagnostics();
    REQUIRE(after.unsourceable_sample.size() == 1);
    CHECK(after.unsourceable_sample.front() == missing);
    CHECK(node.local_state().data().has(present));

    // Repeated passes deduplicate the sample while the counter keeps climbing.
    node.store().repair_once(4ULL * 1024 * 1024, live);
    const auto repeated = node.store().repair_diagnostics();
    CHECK(repeated.unsourceable_sample.size() == 1);
    CHECK(repeated.pull_unsourceable >= after.pull_unsourceable);
}

// Kept integrated: the barrier's answer across a real peer restart, a new
// durability epoch and backend incarnation read from the peer's own disk.
//
// After a peer restarts, the barrier re-derives its token from the peer's
// disk and re-stamps the batch without re-sending bytes; while the peer is
// down the failure is transient, naming nothing; once a restarted peer has
// lost the object, the barrier names it so the writer can re-put it.
MACHA_TEST("storage_v18", test_durability_barrier_across_peer_restarts) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    StorageClusterNode a(storage_node_config(cluster, "epoch-a", a_port, 64ULL * 1024 * 1024, 2, 1,
                                             {}, 2),
                         cluster.keys());
    StorageClusterNode b(storage_node_config(cluster, "epoch-b", b_port, 64ULL * 1024 * 1024, 2, 1,
                                             {{"127.0.0.1", a_port}}, 2),
                         cluster.keys());
    a.start();
    b.start();
    const auto both_active = [&] {
        return wait_until([&] {
            return a.node().membership().active().size() == 2 &&
                   b.node().membership().active().size() == 2;
        }, 10s);
    };
    REQUIRE(both_active());

    const auto data = pattern(64 * 1024, 91);
    DistributedStore::DurabilityBatch batch;
    const auto id = a.store().put_deferred(data, batch);
    REQUIRE(!batch.empty());
    REQUIRE(b.node().local_store().has(id));
    REQUIRE(a.store().durability_barrier(batch));

    // a's dial to b failed while b was down, so early barriers may be refused
    // as in retry backoff; that is transient, and the contract is to ask again.
    std::vector<ObjectId> unsatisfiable;
    bool durable = false;
    const auto definitive = [&] {
        return wait_until([&] {
            unsatisfiable.clear();
            durable = a.store().durability_barrier(batch, FrameType::loader, &unsatisfiable);
            return durable || !unsatisfiable.empty();
        }, 30s);
    };

    const auto old_epoch = b.node().durability_epoch();
    b.restart();
    REQUIRE(b.node().durability_epoch() != old_epoch);
    REQUIRE(both_active());
    REQUIRE(b.node().local_store().has(id));
    REQUIRE(definitive());
    CHECK(durable);
    CHECK(unsatisfiable.empty());
    bool restamped = false;
    for (const auto& requirement : batch.requirements)
        for (const auto& replica : requirement.replicas)
            if (replica.id == b.node().node_id())
                restamped = replica.epoch == b.node().durability_epoch();
    CHECK(restamped);
    CHECK(a.store().durability_barrier(batch));

    b.stop();
    unsatisfiable.clear();
    CHECK(!a.store().durability_barrier(batch, FrameType::loader, &unsatisfiable));
    CHECK(unsatisfiable.empty());

    b.start();
    REQUIRE(both_active());
    REQUIRE(b.node().local_store().remove(id));
    REQUIRE(definitive());
    CHECK(!durable);
    CHECK(unsatisfiable == std::vector<ObjectId>{id});
}
MACHA_TEST("storage_v18", test_min_write_two_uses_fallback_when_preferred_replica_is_full) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    const auto c_port = free_port();
    // Equal placement weights, then fill A: this tests fallback from a full
    // preferred replica, not capacity-weighted placement.
    constexpr uint64_t node_limit = 2ULL * 1024 * 1024;
    auto a_config = storage_node_config(cluster, "a", a_port, node_limit, 2, 1, {}, 2);
    auto b_config = storage_node_config(cluster, "b", b_port, node_limit, 2, 1,
                                        {{"127.0.0.1", a_port}}, 2);
    auto c_config = storage_node_config(cluster, "c", c_port, node_limit, 2, 1,
                                        {{"127.0.0.1", a_port}}, 2);
    StorageClusterNode a(std::move(a_config), cluster.keys());
    StorageClusterNode b(std::move(b_config), cluster.keys());
    StorageClusterNode c(std::move(c_config), cluster.keys());
    a.start(); b.start(); c.start();
    REQUIRE(wait_until([&] {
        return a.node().membership().active().size() == 3 &&
               b.node().membership().active().size() == 3 &&
               c.node().membership().active().size() == 3;
    }, 5s));

    fill_data_store(a.local_state().data());
    Bytes data;
    std::vector<NodeInfo> order;
    for (unsigned i = 0; i < 4096; ++i) {
        data = pattern(128 * 1024, static_cast<uint8_t>(i));
        data[0] ^= static_cast<uint8_t>(i * 13U);
        order = capacity_placement_nodes(object_id(data).bytes, b.node().membership().active(), 2);
        if (!order.empty() && order.front().id == a.node().node_id()) break;
    }
    REQUIRE(!order.empty());
    REQUIRE(order.front().id == a.node().node_id());
    const auto id = object_id(data);
    REQUIRE(b.store().put(id, data));
    CHECK(!a.local_state().data().has(id));
    const unsigned copies = static_cast<unsigned>(b.local_state().data().has(id)) +
                            static_cast<unsigned>(c.local_state().data().has(id));
    CHECK(copies == 2);
}

MACHA_TEST("storage_v18", test_min_write_floor_publishes_then_repair_converges_to_r2) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto first_port = free_port();
    const auto second_port = free_port();
    auto first_config = storage_node_config(cluster, "first", first_port, 8ULL * 1024 * 1024, 2, 1);
    StorageClusterNode first(std::move(first_config), cluster.keys());
    first.start();

    auto data = pattern(256 * 1024, 42);
    const auto id = object_id(data);
    REQUIRE(first.store().put(id, data));
    CHECK(first.local_state().data().has(id));

    auto second_config = storage_node_config(
        cluster, "second", second_port, 8ULL * 1024 * 1024, 2, 1,
        {{"127.0.0.1", first_port}});
    StorageClusterNode second(std::move(second_config), cluster.keys());
    second.start();
    REQUIRE(wait_until([&] {
        return first.node().membership().active().size() == 2 &&
               second.node().membership().active().size() == 2;
    }, 5s));
    CHECK(!second.local_state().data().has(id));

    const std::vector<ObjectId> live{id};
    REQUIRE(wait_until([&] {
        first.store().repair_once(4ULL * 1024 * 1024, live);
        second.store().repair_once(4ULL * 1024 * 1024, live);
        return first.local_state().data().has(id) && second.local_state().data().has(id);
    }, 5s));
    REQUIRE(second.local_state().data().get(id).has_value());
    CHECK(*second.local_state().data().get(id) == data);
}

MACHA_TEST("storage_v18", test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer) {
    // Each node has one background DATA slot and replicates its own objects
    // to the other. A sender that kept its slot while the peer admitted the
    // object would leave both nodes waiting on each other's slot.
    TestCluster cluster;
    const auto a_port = free_port();
    const auto b_port = free_port();
    auto a_config = storage_node_config(cluster, "cross-a", a_port, 64ULL * 1024 * 1024, 2, 1);
    auto b_config = storage_node_config(cluster, "cross-b", b_port, 64ULL * 1024 * 1024, 2, 1,
                                        {{"127.0.0.1", a_port}});
    a_config.maintenance.background_concurrency = 1;
    b_config.maintenance.background_concurrency = 1;
    StorageClusterNode a(std::move(a_config), cluster.keys());
    StorageClusterNode b(std::move(b_config), cluster.keys());
    a.start();
    b.start();
    REQUIRE(wait_until([&] {
        return a.node().membership().active().size() == 2 &&
               b.node().membership().active().size() == 2;
    }, 5s));

    constexpr size_t threads_per_node = 4;
    constexpr size_t objects_per_thread = 8;
    std::atomic_size_t done{};
    std::atomic_size_t on_both{};
    std::vector<std::jthread> writers;
    for (auto* node : {&a, &b})
        for (size_t t = 0; t < threads_per_node; ++t)
            writers.emplace_back([&, node, t] {
                for (size_t i = 0; i < objects_per_thread; ++i) {
                    const auto seed = static_cast<uint8_t>((node == &a ? 0 : 100) + t * 10 + i);
                    const auto data = pattern(64 * 1024, seed);
                    if (node->store().replicate_all(object_id(data), data) == 2)
                        on_both.fetch_add(1);
                    done.fetch_add(1);
                }
            });
    const size_t total = 2 * threads_per_node * objects_per_thread;
    REQUIRE(wait_until([&] { return done.load() == total; }, 20s));
    CHECK(on_both.load() == total);
}

MACHA_HEAVY_TEST("storage_v18", test_retain_data_batches_a_large_publication_within_bounded_time) {
    // One publication referencing thousands of extents: retain_data() must
    // batch presence checks rather than make a round trip per extent.
    // min_write_replicas == replication == 2, so both the local presence path
    // and the batched remote have_objects path are exercised.
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    auto a_config =
        storage_node_config(cluster, "retain-a", a_port, 64ULL * 1024 * 1024, 2, 1, {}, 2);
    auto b_config = storage_node_config(cluster, "retain-b", b_port, 64ULL * 1024 * 1024, 2, 1,
                                        {{"127.0.0.1", a_port}}, 2);
    StorageClusterNode a(std::move(a_config), cluster.keys());
    StorageClusterNode b(std::move(b_config), cluster.keys());
    a.start();
    b.start();
    REQUIRE(wait_until([&] {
        return a.node().membership().active().size() == 2 &&
               b.node().membership().active().size() == 2;
    }, 5s));

    constexpr size_t extents = 3000;
    std::vector<ObjectId> ids;
    ids.reserve(extents);
    for (size_t i = 0; i < extents; ++i) {
        auto data = pattern(256, static_cast<uint8_t>(i));
        data[0] ^= static_cast<uint8_t>(i >> 8);
        const auto id = object_id(data);
        // Write both replicas straight into each LocalStore, without network
        // replication or per-object fsync: only retain_data() is timed.
        REQUIRE(a.local_state().data().put_deferred(id, data));
        REQUIRE(b.local_state().data().put_deferred(id, data));
        ids.push_back(id);
    }
    CHECK(a.node().local_store().has(ids.front()));
    CHECK(b.node().local_store().has(ids.front()));

    const RetentionDot dot{a.node().node_id(), 1};
    const auto started = std::chrono::steady_clock::now();
    a.store().retain_data(ids, dot);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Batched checks need a handful of round trips; the bound is loose for
    // loaded hardware but still catches one round trip per extent.
    CHECK(elapsed < scaled(10s));
}

MACHA_TEST("storage_v18", test_a_control_graph_larger_than_the_connection_budget_still_publishes) {
    // retain_control() pipelines puts, but a connection holds at most
    // max_pending_rpc_requests outstanding; a graph larger than that must
    // still publish.
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    auto a_config = storage_node_config(cluster, "graph-a", a_port, 64ULL * 1024 * 1024, 2, 2);
    auto b_config = storage_node_config(cluster, "graph-b", b_port, 64ULL * 1024 * 1024, 2, 2,
                                        {{"127.0.0.1", a_port}});
    StorageClusterNode a(std::move(a_config), cluster.keys());
    StorageClusterNode b(std::move(b_config), cluster.keys());
    a.start();
    b.start();
    REQUIRE(wait_until([&] {
        return a.node().membership().active().size() == 2 &&
               b.node().membership().active().size() == 2;
    }, 10s));

    // Two windows past the budget.
    const size_t count = max_pending_rpc_requests * 2;
    std::vector<ObjectId> ids;
    ids.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto bytes = pattern(128, static_cast<uint8_t>(i % 251));
        bytes[0] = static_cast<uint8_t>(i & 0xff);
        bytes[1] = static_cast<uint8_t>((i >> 8) & 0xff);
        const auto id = object_id(bytes);
        REQUIRE(a.node().control_store().put(id, bytes));
        ids.push_back(id);
    }

    const RetentionDot dot{a.node().node_id(), 1};
    REQUIRE(a.store().retain_control(ids, dot));

    // The peer holds every object: a present peer is given the graph.
    size_t on_peer = 0;
    for (const auto& id : ids)
        if (b.node().control_store().has(id))
            ++on_peer;
    CHECK(on_peer == ids.size());

    // A second retain over the same graph sends nothing, so commit cost
    // scales with what changed rather than with library size.
    const auto puts_before =
        b.node().rpc_server_work_stats().message_timings[MessageType::put_control_object].requests;
    REQUIRE(puts_before > 0);
    const RetentionDot again{a.node().node_id(), 2};
    REQUIRE(a.store().retain_control(ids, again));
    const auto puts_after =
        b.node().rpc_server_work_stats().message_timings[MessageType::put_control_object].requests;
    CHECK(puts_after == puts_before);

    // Removing part of the graph from the peer resends exactly that part.
    for (size_t i = 0; i < 10; ++i)
        REQUIRE(b.node().control_store().remove(ids[i]));
    const RetentionDot third{a.node().node_id(), 3};
    REQUIRE(a.store().retain_control(ids, third));
    const auto puts_final =
        b.node().rpc_server_work_stats().message_timings[MessageType::put_control_object].requests;
    CHECK(puts_final == puts_after + 10);
    for (size_t i = 0; i < 10; ++i)
        CHECK(b.node().control_store().has(ids[i]));
}

MACHA_TEST("storage_v18", test_catalogue_control_objects_recover_on_metadata_replica) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    auto a_config = storage_node_config(cluster, "control-a", a_port, 8ULL * 1024 * 1024, 1, 2);
    auto b_config = storage_node_config(
        cluster, "control-b", b_port, 8ULL * 1024 * 1024, 1, 2,
        {{"127.0.0.1", a_port}});
    StorageClusterNode a(std::move(a_config), cluster.keys());
    StorageClusterNode b(std::move(b_config), cluster.keys());
    a.start();
    b.start();
    REQUIRE(wait_until([&] {
        return a.node().membership().active().size() == 2 &&
               b.node().membership().active().size() == 2;
    }, 5s));
    REQUIRE(wait_until([&] {
        try { return b.metadata().snapshot().metadata_voters.empty(); }
        catch (...) { return false; }
    }, 5s));

    CatalogueItem item;
    item.id = "tmdb:movie:1800";
    item.kind = CatalogueKind::movie;
    item.title = "Control Recovery";
    (void)b.catalogue().upsert(item);

    const auto metadata = b.metadata().snapshot();
    REQUIRE(metadata.catalogue_root.has_value());
    const auto root = *metadata.catalogue_root;
    REQUIRE(a.node().control_store().get(root).has_value());
    auto referenced = a.node().control_store().list();
    REQUIRE(referenced.size() >= 2);
    REQUIRE(std::find(referenced.begin(), referenced.end(), root) != referenced.end());

    for (const auto& id : referenced)
        REQUIRE(a.node().control_store().remove(id));
    for (const auto& id : referenced)
        CHECK(!a.node().control_store().has(id));

    // A fresh catalogue manager has no in-memory snapshot to hide the missing
    // physical control objects. Repair must fetch the manifest/shards from the
    // other metadata replica and leave them durable locally again.
    CatalogueManager fresh(a.node(), a.node().local_state(), a.node().metadata_server(), a.store(), a.metadata(), a.node().ledger());
    fresh.repair_once();
    REQUIRE(fresh.get(item.id).has_value());
    for (const auto& id : referenced)
        CHECK(a.node().control_store().valid(id));
}

// An edge node (no storage.data) is never an owner or fallback for any key,
// and its writes land on nodes that host extents.
MACHA_TEST("storage_v18", test_edge_node_never_owns_and_its_writes_land_on_owners) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();
    const std::vector<Endpoint> seeds{{"127.0.0.1", p1}};

    StorageClusterNode s1(storage_node_config(cluster, "s1", p1, 64ULL * 1024 * 1024, 1, 1), keys);
    StorageClusterNode s2(storage_node_config(cluster, "s2", p2, 64ULL * 1024 * 1024, 1, 1, seeds),
                          keys);
    auto edge_config = storage_node_config(cluster, "edge", p3, 64ULL * 1024 * 1024, 1, 1, seeds);
    edge_config.storage_backends.clear(); // hosts_extents auto -> false
    StorageClusterNode edge(edge_config, keys);
    s1.start();
    s2.start();
    edge.start();

    CHECK(!edge.node().hosts_extents());
    CHECK(edge.node().inbound_capable());
    CHECK(edge.node().local_store().limit() == 0);
    const auto edge_id = edge.node().node_id();
    // Placement is capacity-weighted per observer, and a handshake can carry a
    // peer at capacity 0 until gossip refreshes it. Wait until both observers
    // hold the same hosting set with the same capacities.
    const auto hosting_view = [](NodeRuntime& node) {
        std::vector<std::tuple<std::string, uint64_t, uint8_t>> out;
        for (const auto& member : node.membership().active())
            if (node_hosts_extents(member))
                out.emplace_back(to_string(member.id), member.capacity, member.flags);
        std::sort(out.begin(), out.end());
        return out;
    };
    REQUIRE(wait_until([&] {
        const auto view1 = hosting_view(s1.node());
        const auto view2 = hosting_view(s2.node());
        return edge.node().membership().active().size() == 3 &&
               s1.node().membership().active().size() == 3 &&
               s2.node().membership().active().size() == 3 &&
               !s1.node().membership().hosts_extents(edge_id) &&
               !s2.node().membership().hosts_extents(edge_id) &&
               view1.size() == 2 && view1 == view2 &&
               std::all_of(view1.begin(), view1.end(),
                           [](const auto& member) { return std::get<1>(member) > 0; });
    }, 10s));

    // Never an owner, from any node's point of view, for any key.
    size_t owned_by_s1 = 0, owned_by_s2 = 0;
    for (unsigned i = 0; i < 512; ++i) {
        const auto id = object_id(pattern(64, static_cast<uint8_t>(i)));
        CHECK(!edge.store().should_own(id));
        owned_by_s1 += s1.store().should_own(id) ? 1 : 0;
        owned_by_s2 += s2.store().should_own(id) ? 1 : 0;
        // The observers agree: the edge node is in nobody's placement input.
        const auto ranked = capacity_placement_nodes(id.bytes, s1.node().membership().active(), 1);
        (void)ranked;
    }
    CHECK(owned_by_s1 > 0);
    CHECK(owned_by_s2 > 0);
    CHECK(owned_by_s1 + owned_by_s2 == 512);
    if (owned_by_s1 + owned_by_s2 != 512)
        Log::warn("edge-node placement views disagree owned_by_s1=" +
                  std::to_string(owned_by_s1) + " owned_by_s2=" + std::to_string(owned_by_s2));

    // A write from the edge node reaches an owner and stays off the edge.
    const auto data = pattern(256 * 1024, 7);
    const auto id = edge.store().put(data, FrameType::loader);
    CHECK(!edge.node().local_store().has(id));
    CHECK(s1.node().local_store().has(id) || s2.node().local_store().has(id));
    // ...and it can read it back, through its cache, like any non-owner.
    const auto read = edge.store().get(id, 0, FrameType::foreground);
    REQUIRE(read.has_value());
    CHECK(*read == data);
    CHECK(!edge.node().local_store().has(id));
}

// A node that stops hosting extents drains through ordinary repair: the
// copies it holds are pushed to the owners and then removed locally.
MACHA_TEST("storage_v18", test_node_that_stops_hosting_drains_through_repair) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const std::vector<Endpoint> seeds{{"127.0.0.1", p1}};

    StorageClusterNode s1(storage_node_config(cluster, "s1", p1, 64ULL * 1024 * 1024, 1, 1), keys);
    StorageClusterNode s2(storage_node_config(cluster, "s2", p2, 64ULL * 1024 * 1024, 1, 1, seeds),
                          keys);
    s1.start();
    s2.start();
    const auto s2_id = s2.node().node_id();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }, 10s));

    // Objects s2 owns while it still hosts.
    std::vector<ObjectId> owned;
    for (uint8_t salt = 1; owned.size() < 4 && salt < 250; ++salt) {
        const auto data = preferred_for(s2.node(), s2_id, 96 * 1024, salt);
        const auto id = s2.store().put(data, FrameType::loader);
        REQUIRE(s2.node().local_store().has(id));
        owned.push_back(id);
    }
    REQUIRE(owned.size() == 4);

    // s2 restarts declaring it hosts nothing; its backends stay configured, so
    // the objects remain to be drained.
    s2.stop();
    s2.config().hosts_extents = Tristate::no;
    s2.start();
    CHECK(!s2.node().hosts_extents());
    REQUIRE(wait_until([&] { return !s1.node().membership().hosts_extents(s2_id); }, 10s));
    for (const auto& id : owned) {
        CHECK(!s2.store().should_own(id));
        CHECK(s1.store().should_own(id));
    }

    // Repair on the draining node pushes each copy to its owner and removes
    // the local one once the owner holds it.
    REQUIRE(wait_until([&] {
        (void)s2.store().repair_once();
        return std::all_of(owned.begin(), owned.end(), [&](const ObjectId& id) {
            return s1.node().local_store().has(id) && !s2.node().local_store().has(id);
        });
    }, 20s, 100ms));
    for (const auto& id : owned) {
        const auto read = s2.store().get(id, 0, FrameType::speculative);
        CHECK(read.has_value());
    }
}

