// SPDX-License-Identifier: GPL-3.0-or-later
#include "interposed_local_store_files.hpp"
#include "test_backend_support.hpp"
#include <limits>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

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
        files.before(
            [this, op, path](InterposedLocalStoreFiles::Op seen, const std::filesystem::path& at) {
                if (seen != op || (!path.empty() && at != path))
                    return;
                if (calls_.fetch_add(1) == 0) {
                    entered_.set_value();
                    released_.wait();
                }
            });
    }
    bool entered() {
        return entered_future_.wait_for(scaled(2s)) == std::future_status::ready;
    }
    void release() {
        if (!released_once_.exchange(true))
            release_.set_value();
    }
    uint64_t calls() const {
        return calls_.load();
    }
};

template <class T> bool ready(std::future<T>& future, std::chrono::milliseconds within = 2s) {
    return future.wait_for(within) == std::future_status::ready;
}

} // namespace

// What a store throws says which it was: ObjectCorrupt for a record that read
// and did not open, anything else for a read that did not finish.
MACHA_TEST("storage_v18", test_a_store_tells_a_bad_record_from_a_failed_read) {
    TempDir t;
    const auto key = storage_key(t);
    const auto throws_corrupt = [](LocalStore& store, const ObjectId& id) {
        try {
            (void)store.get(id);
        } catch (const ObjectCorrupt&) {
            return true;
        } catch (const std::exception&) {
        }
        return false;
    };
    const auto fails_plainly = [](LocalStore& store, const ObjectId& id) {
        try {
            (void)store.get(id);
        } catch (const ObjectCorrupt&) {
            return false;
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    {
        LocalStore packed(t.path() / "packed",
                          packed_options(64ULL * 1024 * 1024, 16ULL * 1024 * 1024), key);
        const auto first = pattern(16 * 1024, 1);
        const auto last = pattern(16 * 1024, 2);
        REQUIRE(packed.put(object_id(first), first));
        REQUIRE(packed.put(object_id(last), last));
        REQUIRE(packed.is_packed(object_id(first)));
        REQUIRE(packed.is_packed(object_id(last)));
        const auto pack = only_pack(t.path() / "packed");
        // The first record's payload starts past its header; flip a byte in
        // the middle of it. The last record's tail is cut off.
        {
            std::fstream file(pack, std::ios::in | std::ios::out | std::ios::binary);
            file.seekg(8 * 1024);
            char byte{};
            file.read(&byte, 1);
            byte ^= 0x5a;
            file.seekp(8 * 1024);
            file.write(&byte, 1);
            REQUIRE(file.good());
        }
        std::filesystem::resize_file(pack, std::filesystem::file_size(pack) - 1024);
        CHECK(throws_corrupt(packed, object_id(first)));
        CHECK(fails_plainly(packed, object_id(last)));
    }
    {
        LocalStoreOptions options;
        options.limit = 64ULL * 1024 * 1024;
        LocalStore loose(t.path() / "loose", options, key);
        const auto corrupt = pattern(16 * 1024, 3);
        const auto torn = pattern(16 * 1024, 4);
        const auto unreadable = pattern(16 * 1024, 5);
        for (const auto* bytes : {&corrupt, &torn, &unreadable})
            REQUIRE(loose.put(object_id(*bytes), *bytes));
        corrupt_object(t.path() / "loose", object_id(corrupt));
        std::filesystem::resize_file(object_path(t.path() / "loose", object_id(torn)), 8);
        const auto path = object_path(t.path() / "loose", object_id(unreadable));
        REQUIRE(std::filesystem::remove(path));
        REQUIRE(std::filesystem::create_directory(path));
        CHECK(throws_corrupt(loose, object_id(corrupt)));
        CHECK(throws_corrupt(loose, object_id(torn)));
        CHECK(fails_plainly(loose, object_id(unreadable)));
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
            try {
                node_->stop();
            } catch (...) {
            }
        }
    }
    Config& config() {
        return config_;
    }
    void prepare() {
        REQUIRE(!node_);
        for (const auto& backend : config_.storage_backends)
            std::filesystem::create_directories(backend.path);
        if (!config_.metadata_store.path.empty())
            std::filesystem::create_directories(config_.metadata_store.path);
        node_ = std::make_unique<BareNode>(config_, keys_);
    }
    void start() {
        if (!node_)
            prepare();
        node_->start();
        started_ = true;
        REQUIRE(node_->wait_local_state_ready(10s));
        store_ = std::make_unique<DistributedStore>(
            *node_, node_->local_state(), node_->resources.activity, node_->resources.data,
            node_->resources.memory, node_->resources.events);
        metadata_ = std::make_unique<MetadataManager>(
            *node_, node_->local_state(), node_->metadata_server(), node_->store_context());
        catalogue_ = std::make_unique<CatalogueManager>(*node_, node_->metadata_server(),
                                                        node_->store_context(), *metadata_);
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
    BareNode& node() {
        REQUIRE(node_);
        return *node_;
    }
    LocalState& local_state() {
        REQUIRE(node_);
        return node_->local_state();
    }
    DistributedStore& store() {
        REQUIRE(store_);
        return *store_;
    }
    MetadataManager& metadata() {
        REQUIRE(metadata_);
        return *metadata_;
    }
    CatalogueManager& catalogue() {
        REQUIRE(catalogue_);
        return *catalogue_;
    }
};

Config storage_node_config(const TestCluster& cluster, std::string_view name, uint16_t port,
                           uint64_t data_limit, size_t replicas, size_t metadata_replicas,
                           std::vector<Endpoint> bootstrap = {}, size_t write_copies = 1) {
    auto config = cluster.node_config(name, port, std::move(bootstrap));
    config.replication = replicas;
    config.write_copies = write_copies;
    config.metadata_write_copies = metadata_replicas;
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

Bytes preferred_for(NodeRuntime& observer, const NodeId& preferred, size_t bytes,
                    uint8_t salt_start = 0) {
    // Placement uses the observer's active membership, so the preferred node
    // must be active there before any candidate can prefer it.
    REQUIRE(wait_until(
        [&] {
            const auto active = observer.membership().active();
            return std::any_of(active.begin(), active.end(),
                               [&](const NodeInfo& node) { return node.id == preferred; });
        },
        10s));
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
    StorageClusterNode large(storage_node_config(cluster, "large", large_port, 16ULL * 1024 * 1024,
                                                 1, 2, {{"127.0.0.1", small_port}}),
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
    CHECK(large.node().control_store().has(*metadata.catalogue_root));
    // Delivered to the other node by the catalogue's convergence pass.
    large.catalogue().repair_once();
    CHECK(small.node().control_store().has(*metadata.catalogue_root));
    CHECK(!small.node().local_store().has(*metadata.catalogue_root));
    CHECK(!large.node().local_store().has(*metadata.catalogue_root));

    // The reader's view is its own head: maintenance's merge (no maintenance
    // runs in this fixture) brings the writer's commit into it.
    REQUIRE(wait_until(
        [&] {
            try {
                small.metadata().repair_once();
                small.catalogue().follow_head();
                return small.catalogue().get(item.id).has_value();
            } catch (...) {
                return false;
            }
        },
        10s));
    auto fetched = small.catalogue().artwork(art.id);
    REQUIRE(fetched.has_value());
    CHECK(fetched->bytes == art_bytes);
}

MACHA_TEST("storage_v18", test_catalogue_control_objects_recover_on_metadata_replica) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto a_port = free_port();
    const auto b_port = free_port();
    auto a_config = storage_node_config(cluster, "control-a", a_port, 8ULL * 1024 * 1024, 1, 2);
    auto b_config = storage_node_config(cluster, "control-b", b_port, 8ULL * 1024 * 1024, 1, 2,
                                        {{"127.0.0.1", a_port}});
    StorageClusterNode a(std::move(a_config), cluster.keys());
    StorageClusterNode b(std::move(b_config), cluster.keys());
    a.start();
    b.start();
    REQUIRE(wait_until(
        [&] {
            return a.node().membership().active().size() == 2 &&
                   b.node().membership().active().size() == 2;
        },
        5s));
    REQUIRE(wait_until(
        [&] {
            try {
                return b.metadata().snapshot().metadata_voters.empty();
            } catch (...) {
                return false;
            }
        },
        5s));

    CatalogueItem item;
    item.id = "tmdb:movie:1800";
    item.kind = CatalogueKind::movie;
    item.title = "Control Recovery";
    (void)b.catalogue().upsert(item);

    const auto metadata = b.metadata().snapshot();
    REQUIRE(metadata.catalogue_root.has_value());
    const auto root = *metadata.catalogue_root;
    // The write is answered from the node that took it. The other holds its
    // control objects once they are delivered: here by the catalogue's own
    // convergence pass, which maintenance runs (this fixture has no
    // replicator claims).
    b.catalogue().repair_once();
    REQUIRE(a.node().control_store().get(root).has_value());
    // And a's head carries the commit once maintenance's merge has run (no
    // maintenance runs in this fixture).
    REQUIRE(wait_until(
        [&] {
            try {
                a.metadata().repair_once();
                return a.metadata().local().snapshot->catalogue_root == root;
            } catch (...) {
                return false;
            }
        },
        10s));
    // The namespace tree's nodes are CONTROL objects too, but namespace repair
    // restores them, not the catalogue's.
    std::vector<ObjectId> tree;
    {
        const auto reader = ControlNamespaceNodeStore::for_reading(a.node().store_context());
        for_each_namespace_entry(
            a.metadata().snapshot(), &reader, [](const std::string&, const FsEntry&) {}, tree);
    }
    auto referenced = a.node().control_store().list();
    std::erase_if(referenced, [&](const ObjectId& id) {
        return std::find(tree.begin(), tree.end(), id) != tree.end();
    });
    REQUIRE(referenced.size() >= 2);
    REQUIRE(std::find(referenced.begin(), referenced.end(), root) != referenced.end());

    for (const auto& id : referenced)
        REQUIRE(a.node().control_store().remove(id));
    for (const auto& id : referenced)
        CHECK(!a.node().control_store().has(id));

    // A fresh catalogue manager has no in-memory snapshot to hide the missing
    // physical control objects. Repair must fetch the manifest/shards from the
    // other metadata replica and leave them durable locally again.
    CatalogueManager fresh(a.node(), a.node().metadata_server(), a.node().store_context(),
                           a.metadata());
    fresh.repair_once();
    REQUIRE(fresh.get(item.id).has_value());
    for (const auto& id : referenced)
        CHECK(a.node().control_store().valid(id));
}
