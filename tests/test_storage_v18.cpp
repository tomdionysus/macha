// SPDX-License-Identifier: GPL-3.0-or-later
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

MACHA_TEST("storage_v18", test_small_objects_are_packed_and_recover_after_restart) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    LocalStoreOptions options;
    options.limit = 64ULL * 1024 * 1024;
    options.pack_threshold = 256 * 1024;
    options.pack_target_size = 1024 * 1024;

    std::vector<std::pair<ObjectId, Bytes>> objects;
    {
        LocalStore store(t.path() / "store", options, keys.storage);
        for (size_t i = 0; i < 40; ++i) {
            auto data = pattern(48 * 1024 + i);
            data[0] ^= static_cast<uint8_t>(i);
            auto id = object_id(data);
            REQUIRE(store.put(id, data));
            objects.push_back({id, std::move(data)});
        }

        for (const auto& [id, data] : objects) {
            REQUIRE(store.get(id).has_value());
            CHECK(*store.get(id) == data);
            CHECK(store.is_packed(id));
        }
        CHECK(regular_files_below(t.path() / "store" / "objects") == 0);
        CHECK(regular_files_below(t.path() / "store" / "packs") < objects.size());
    }

    // The pack index is derived state. A clean restart must reconstruct it from
    // durable pack records without any cluster metadata or external index.
    {
        LocalStore reopened(t.path() / "store", options, keys.storage);
        for (const auto& [id, data] : objects) {
            REQUIRE(reopened.get(id).has_value());
            CHECK(*reopened.get(id) == data);
            CHECK(reopened.is_packed(id));
        }
    }
}

MACHA_TEST("storage_v18", test_pack_tombstones_and_compaction_preserve_live_objects) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    LocalStoreOptions options;
    options.limit = 96ULL * 1024 * 1024;
    options.pack_threshold = 256 * 1024;
    options.pack_target_size = 1024 * 1024;

    std::vector<std::pair<ObjectId, Bytes>> live;
    std::vector<ObjectId> removed;
    uint64_t before_compaction = 0;
    {
        LocalStore store(t.path() / "store", options, keys.storage);
        for (size_t i = 0; i < 48; ++i) {
            auto data = pattern(64 * 1024 + i);
            data[0] ^= static_cast<uint8_t>(i * 3);
            auto id = object_id(data);
            REQUIRE(store.put(id, data));
            if (i % 2) {
                live.push_back({id, std::move(data)});
            } else {
                removed.push_back(id);
            }
        }
        for (const auto& id : removed)
            REQUIRE(store.remove(id));
        before_compaction = store.used();
        REQUIRE(store.compact_packs());
        CHECK(store.used() < before_compaction);
        for (const auto& id : removed)
            CHECK(!store.has(id));
        for (const auto& [id, data] : live) {
            REQUIRE(store.get(id).has_value());
            CHECK(*store.get(id) == data);
        }
    }

    {
        LocalStore reopened(t.path() / "store", options, keys.storage);
        for (const auto& id : removed)
            CHECK(!reopened.has(id));
        for (const auto& [id, data] : live) {
            REQUIRE(reopened.get(id).has_value());
            CHECK(*reopened.get(id) == data);
        }
    }
}

MACHA_TEST("storage_v18", test_pack_compaction_reclaims_dead_space_incrementally) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    LocalStoreOptions options;
    options.limit = 128ULL * 1024 * 1024;
    options.pack_threshold = 256 * 1024;
    options.pack_target_size = 512 * 1024;

    LocalStore store(t.path() / "store", options, keys.storage);
    std::vector<ObjectId> ids;
    for (size_t i = 0; i < 40; ++i) {
        auto data = pattern(96 * 1024 + i, static_cast<uint8_t>(17 + i));
        auto id = object_id(data);
        REQUIRE(store.put(id, data));
        ids.push_back(id);
    }
    for (size_t i = 0; i < ids.size(); i += 2)
        REQUIRE(store.remove(ids[i]));

    const auto before = store.used();
    REQUIRE(store.compact_packs());
    const auto after_one = store.used();
    CHECK(after_one < before);

    // More dead packs remain after one bounded victim rewrite. A second
    // maintenance slice therefore makes additional progress instead of the
    // first call requiring temporary space for the complete packed corpus.
    REQUIRE(store.compact_packs());
    const auto after_two = store.used();
    CHECK(after_two < after_one);

    for (size_t i = 0; i < ids.size(); ++i) {
        if (i % 2 == 0)
            CHECK(!store.has(ids[i]));
        else
            CHECK(store.valid(ids[i]));
    }
}

MACHA_TEST("storage_v18", test_local_backend_capacity_falls_through_to_larger_backend) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto state = t.path() / "state";
    auto small = t.path() / "small";
    auto large = t.path() / "large";
    std::filesystem::create_directories(small);
    std::filesystem::create_directories(large);

    auto node = load_or_create_node_id(state);
    StoragePackingConfig no_packing;
    no_packing.threshold = 0;
    no_packing.target_size = 0;
    StoragePool pool(state, node,
                     {{small, 512ULL * 1024, 0}, {large, 16ULL * 1024 * 1024, 0}},
                     keys.storage, 0ms, no_packing);

    std::vector<ObjectId> ids;
    for (size_t i = 0; i < 40; ++i) {
        auto data = pattern(192 * 1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        ids.push_back(id);
    }

    CHECK(pool.used() > 512ULL * 1024);
    CHECK(pool.limit() == 512ULL * 1024 + 16ULL * 1024 * 1024);
    for (const auto& id : ids)
        CHECK(pool.valid(id));
}

MACHA_TEST("storage_v18", test_data_reserve_free_blocks_admission_before_filesystem_exhaustion) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    LocalStoreOptions options;
    options.limit = 64ULL * 1024 * 1024;
    options.reserve_free = std::numeric_limits<uint64_t>::max();
    options.pack_threshold = 0;
    options.pack_target_size = 0;

    LocalStore store(t.path() / "store", options, keys.storage);
    auto data = pattern(128 * 1024, 31);
    const auto id = object_id(data);
    CHECK(!store.put(id, data));
    CHECK(!store.has(id));
    CHECK(store.used() == 0);
}

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
    REQUIRE(fixture.node().control_store().get(control_id).has_value());
    CHECK(*fixture.node().control_store().get(control_id) == control);
}

MACHA_TEST("storage_v18", test_pack_recovery_discards_incomplete_tail_record) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    LocalStoreOptions options;
    options.limit = 32ULL * 1024 * 1024;
    options.pack_threshold = 256 * 1024;
    options.pack_target_size = 1024 * 1024;

    auto first = pattern(96 * 1024, 11);
    auto second = pattern(96 * 1024, 12);
    const auto first_id = object_id(first);
    const auto second_id = object_id(second);
    {
        LocalStore store(t.path() / "store", options, keys.storage);
        REQUIRE(store.put(first_id, first));
        REQUIRE(store.put(second_id, second));
    }

    std::filesystem::path pack;
    for (const auto& entry : std::filesystem::directory_iterator(t.path() / "store" / "packs")) {
        if (entry.is_regular_file()) {
            pack = entry.path();
            break;
        }
    }
    REQUIRE(!pack.empty());
    const auto intact_size = std::filesystem::file_size(pack);
    {
        std::ofstream out(pack, std::ios::binary | std::ios::app);
        REQUIRE(out.good());
        const std::array<uint8_t, 23> torn{'M','A','C','H','P','K','0','1',1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
        out.write(reinterpret_cast<const char*>(torn.data()), static_cast<std::streamsize>(torn.size()));
        REQUIRE(out.good());
    }
    CHECK(std::filesystem::file_size(pack) > intact_size);

    LocalStore reopened(t.path() / "store", options, keys.storage);
    REQUIRE(reopened.get(first_id).has_value());
    REQUIRE(reopened.get(second_id).has_value());
    CHECK(*reopened.get(first_id) == first);
    CHECK(*reopened.get(second_id) == second);
    CHECK(std::filesystem::file_size(pack) == intact_size);
}

namespace {
class StorageClusterNode {
    Config config_;
    const ClusterKeys& keys_;
    std::unique_ptr<NodeRuntime> node_;
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
        node_ = std::make_unique<NodeRuntime>(config_, keys_);
        store_ = std::make_unique<DistributedStore>(*node_);
        metadata_ = std::make_unique<MetadataManager>(*node_);
        catalogue_ = std::make_unique<CatalogueManager>(*node_, *store_, *metadata_);
    }
    void start() {
        if (!node_) prepare();
        node_->start();
        started_ = true;
    }
    NodeRuntime& node() { REQUIRE(node_); return *node_; }
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
    for (unsigned i = 0; i < 4096; ++i) {
        auto data = pattern(bytes, static_cast<uint8_t>(salt_start + i));
        data[0] ^= static_cast<uint8_t>(i);
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


MACHA_TEST("storage_v18", test_unversioned_nonempty_state_namespace_is_refused) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("old-state", free_port());
    config.storage_backends = {{cluster.path() / "old-state.data", 8ULL * 1024 * 1024, 0}};
    config.storage_packing.threshold = 0;
    config.storage_packing.target_size = 0;
    config.metadata_store.path = cluster.path() / "old-state.control";
    std::filesystem::create_directories(config.state_path);
    {
        std::ofstream legacy(config.state_path / "metadata.bin");
        legacy << "old namespace";
    }

    bool refused = false;
    try {
        NodeRuntime node(config, cluster.keys());
    } catch (const std::exception& e) {
        refused = std::string(e.what()).find("fresh namespace") != std::string::npos;
    }
    CHECK(refused);
}

MACHA_TEST("storage_v18", test_unversioned_nonempty_data_backend_is_refused) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("fresh-boundary", free_port());
    config.storage_backends = {{cluster.path() / "old-data", 8ULL * 1024 * 1024, 0}};
    config.storage_packing.threshold = 0;
    config.storage_packing.target_size = 0;
    config.metadata_store.path = cluster.path() / "fresh-boundary.control";
    std::filesystem::create_directories(config.storage_backends.front().path / "objects");
    {
        std::ofstream old(config.storage_backends.front().path / "objects" / "legacy");
        old << "old namespace";
    }

    NodeRuntime node(config, cluster.keys());
    CHECK(node.local_store().online_backends() == 0);
}

MACHA_TEST("storage_v18", test_r1_logical_capacity_is_aggregate_not_smallest_node) {
    NodeInfo small;
    small.id.bytes[0] = 1;
    small.capacity = 1ULL * 1024 * 1024 * 1024;
    NodeInfo large;
    large.id.bytes[0] = 2;
    large.capacity = 2ULL * 1024 * 1024 * 1024 * 1024;
    CHECK(placement_logical_capacity({small, large}, 1) == small.capacity + large.capacity);
}

MACHA_TEST("storage_v18", test_distributed_r1_spills_preferred_full_node_to_next_candidate) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto small_port = free_port();
    const auto large_port = free_port();
    auto small_config = storage_node_config(cluster, "small", small_port, 384ULL * 1024, 1, 1);
    auto large_config = storage_node_config(
        cluster, "large", large_port, 16ULL * 1024 * 1024, 1, 1,
        {{"127.0.0.1", small_port}});

    StorageClusterNode small(std::move(small_config), cluster.keys());
    StorageClusterNode large(std::move(large_config), cluster.keys());
    small.start();
    large.start();
    REQUIRE(wait_until([&] {
        return small.node().membership().active().size() == 2 &&
               large.node().membership().active().size() == 2;
    }, 5s));

    fill_data_store(small.node().local_store());
    auto data = preferred_for(large.node(), small.node().node_id(), 128 * 1024, 77);
    const auto id = object_id(data);
    REQUIRE(large.store().put(id, data));
    CHECK(!small.node().local_store().has(id));
    CHECK(large.node().local_store().has(id));
    REQUIRE(large.store().get(id).has_value());
    CHECK(*large.store().get(id) == data);
}

MACHA_TEST("storage_v18", test_min_write_two_uses_fallback_when_preferred_replica_is_full) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    const auto c_port = free_port();
    // Keep configured placement weights equal, then physically fill A.  The
    // test is about fallback from a full preferred replica, not about making a
    // deliberately tiny node win capacity-weighted placement.
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

    fill_data_store(a.node().local_store());
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
    CHECK(!a.node().local_store().has(id));
    const unsigned copies = static_cast<unsigned>(b.node().local_store().has(id)) +
                            static_cast<unsigned>(c.node().local_store().has(id));
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
    CHECK(first.node().local_store().has(id));

    auto second_config = storage_node_config(
        cluster, "second", second_port, 8ULL * 1024 * 1024, 2, 1,
        {{"127.0.0.1", first_port}});
    StorageClusterNode second(std::move(second_config), cluster.keys());
    second.start();
    REQUIRE(wait_until([&] {
        return first.node().membership().active().size() == 2 &&
               second.node().membership().active().size() == 2;
    }, 5s));
    CHECK(!second.node().local_store().has(id));

    const std::vector<ObjectId> live{id};
    REQUIRE(wait_until([&] {
        first.store().repair_once(4ULL * 1024 * 1024, &live);
        second.store().repair_once(4ULL * 1024 * 1024, &live);
        return first.node().local_store().has(id) && second.node().local_store().has(id);
    }, 5s));
    REQUIRE(second.node().local_store().get(id).has_value());
    CHECK(*second.node().local_store().get(id) == data);
}

MACHA_TEST("storage_v18", test_catalogue_metadata_ignores_full_data_quota_and_artwork_uses_data_fallback) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto small_port = free_port();
    const auto large_port = free_port();
    auto small_config = storage_node_config(cluster, "small", small_port, 384ULL * 1024, 1, 2);
    auto large_config = storage_node_config(
        cluster, "large", large_port, 16ULL * 1024 * 1024, 1, 2,
        {{"127.0.0.1", small_port}});
    StorageClusterNode small(std::move(small_config), cluster.keys());
    StorageClusterNode large(std::move(large_config), cluster.keys());
    small.start();
    large.start();
    REQUIRE(wait_until([&] {
        return small.node().membership().active().size() == 2 &&
               large.node().membership().active().size() == 2;
    }, 5s));

    REQUIRE(wait_until([&] {
        try {
            return large.metadata().snapshot().metadata_voters.empty();
        } catch (...) {
            return false;
        }
    }, 5s));

    fill_data_store(small.node().local_store());
    auto art_bytes = preferred_for(large.node(), small.node().node_id(), 128 * 1024, 123);
    auto art = large.catalogue().stage_artwork("poster", "image/jpeg", art_bytes);
    CHECK(!small.node().local_store().has(art.id));
    CHECK(large.node().local_store().has(art.id));

    CatalogueItem item;
    item.id = "tmdb:movie:1";
    item.kind = CatalogueKind::movie;
    item.title = "Storage Contract";
    item.artwork.push_back(art);
    auto committed = large.catalogue().upsert(item);
    CHECK(committed.id == item.id);

    auto metadata = large.metadata().snapshot();
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
    CatalogueManager fresh(a.node(), a.store(), a.metadata());
    fresh.repair_once();
    REQUIRE(fresh.get(item.id).has_value());
    for (const auto& id : referenced)
        CHECK(a.node().control_store().valid(id));
}

} // namespace
