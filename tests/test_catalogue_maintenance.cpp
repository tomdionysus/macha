// SPDX-License-Identifier: GPL-3.0-or-later
//
// The catalogue's maintenance inventory (head, repair, read) against a fake
// metadata view, so no metadata manager is needed behind it.
#include "catalogue/catalogue.hpp"
#include "cluster/distributed_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "test_support.hpp"

#include <set>
#include <stdexcept>

using namespace macha;
using namespace std::chrono_literals;

namespace {

// A metadata view that holds one snapshot, or none; with none, every read
// that would go to the replicas throws, as an unreachable replica set does.
struct FakeMetadataView final : MetadataView {
    std::optional<MetadataSnapshotView> view;
    std::optional<MetadataSnapshotView> current() const override { return view; }
    MetadataSnapshotView converged() override {
        if (!view)
            throw std::runtime_error("metadata unavailable");
        return *view;
    }
    MetadataSnapshotView converged(const WorkContext&) override { return converged(); }
    MetadataSnapshotView local() override { return converged(); }
    uint64_t current_generation() const noexcept override { return view ? view->generation : 0; }
    uint64_t current_namespace_revision() const noexcept override { return 0; }
    // When set, a committed read brings the view up to date; otherwise it
    // fails as an unreachable replica set does.
    std::optional<MetadataSnapshotView> after_record;
    MetadataRecord record() override {
        if (!after_record)
            throw std::runtime_error("metadata unavailable");
        view = after_record;
        return {};
    }
    std::optional<MetadataSnapshotView> release_head() const override { return view; }
    MetadataClusterStatus status() const noexcept override { return {}; }
    MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&, size_t) override {
        throw std::runtime_error("read-only fake");
    }
    // A commit applies to the head and makes the next generation.
    MetadataRecord mutate_delta(const std::function<void(MetadataSnapshot&, MetadataDelta&)>& change,
                                size_t, std::optional<MetadataMutationIdentity>) override {
        if (!view)
            throw std::runtime_error("metadata unavailable");
        auto next = std::make_shared<MetadataSnapshot>(*view->snapshot);
        MetadataDelta delta;
        change(*next, delta);
        view = MetadataSnapshotView{view->generation + 1, 0, Hash256{}, std::move(next)};
        if (after_commit)
            after_commit();
        return {};
    }
    // Runs once the head has moved, before the committer returns: where the
    // installer sees a head change.
    std::function<void()> after_commit;
    // The head moves on with nothing changed.
    void advance() {
        view = MetadataSnapshotView{view->generation + 1, 0, Hash256{}, view->snapshot};
    }
    void set_catalogue_root(std::optional<ObjectId> root) {
        auto next = std::make_shared<MetadataSnapshot>(*view->snapshot);
        next->catalogue_root = root;
        view = MetadataSnapshotView{view->generation + 1, 0, Hash256{}, std::move(next)};
    }
    bool resolve_conflict(const std::string&, std::string_view) override { return false; }
    uint64_t conflicts_superseded() const noexcept override { return 0; }
    uint64_t conflicts_resolved() const noexcept override { return 0; }
    MetadataHeadStanding head_standing() const noexcept override { return {}; }
    std::optional<std::optional<ObjectId>>
    common_ancestor_catalogue_root(const Hash256&, const Hash256&) const override {
        return {};
    }
    MetadataMutationTiming mutation_timing() const noexcept override { return {}; }
    Page<std::pair<std::string, FsEntry>, std::string>
    entries(const MetadataSnapshotView& v, Cursor<std::string> from, Budget& budget) override {
        return namespace_entries(*v.snapshot, nullptr, std::move(from), budget);
    }
};

struct Node {
    macha::test_support::TestCluster cluster;
    Config config;
    std::optional<test_support::BareNode> node;
    std::optional<DistributedStore> store;
    Node() {
        config = macha::test_support::config_for(cluster.path() / "node", cluster.keyfile(),
                                                 macha::test_support::free_port());
        config.replication = 1;
        config.metadata_write_copies = 1;
        node.emplace(config, cluster.keys());
        node->start();
        REQUIRE(node->wait_local_state_ready(10s));
        store.emplace(*node, node->local_state(), node->resources.activity, node->resources.data, node->resources.memory, node->resources.events);
    }
};

MACHA_TEST("catalogue_maintenance", test_without_metadata_nothing_installs_and_the_read_is_incomplete) {
    Node fixture;
    FakeMetadataView metadata;
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    bool failed = false;
    try {
        catalogue.follow_head();
    } catch (const std::exception&) {
        failed = true;
    }
    CHECK(failed);
    CHECK(catalogue.installs() == 0);
    // The head taken against no metadata is not current.
    const auto head = catalogue.maintenance_head();
    CHECK(!head.current);
    CHECK(!head.root.has_value());
    CHECK(!catalogue.maintenance_objects(head).complete);
}

MACHA_TEST("catalogue_maintenance", test_the_read_is_complete_only_when_the_view_is_at_the_head) {
    Node fixture;
    FakeMetadataView metadata;
    // An empty catalogue at the newest generation this node knows of:
    // current, no root, nothing to protect.
    const auto known = fixture.node->known_metadata_generation();
    metadata.view =
        MetadataSnapshotView{known, 0, Hash256{}, std::make_shared<const MetadataSnapshot>()};
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    const auto head = catalogue.maintenance_head();
    CHECK(head.current);
    // Nothing installed yet.
    CHECK(!catalogue.maintenance_objects(head).complete);
    catalogue.follow_head();
    CHECK(catalogue.maintenance_objects(head).complete);
    // A head that is not current never reads complete.
    auto stale = head;
    stale.current = false;
    CHECK(!catalogue.maintenance_objects(stale).complete);
    // Nor does a head whose root the view has not installed.
    auto moved = head;
    moved.root = object_id(test_support::pattern(64, 9));
    CHECK(!catalogue.maintenance_objects(moved).complete);
}

MACHA_TEST("catalogue_maintenance", test_a_head_behind_the_known_generation_reads_the_record_first) {
    Node fixture;
    FakeMetadataView metadata;
    const auto known = fixture.node->known_metadata_generation();
    REQUIRE(known > 0);
    // The cached view is a generation behind; the committed read brings it
    // to the known one, and the head is taken from that.
    metadata.view = MetadataSnapshotView{known - 1, 0, Hash256{},
                                         std::make_shared<const MetadataSnapshot>()};
    metadata.after_record = MetadataSnapshotView{known, 0, Hash256{},
                                                 std::make_shared<const MetadataSnapshot>()};
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    const auto head = catalogue.maintenance_head();
    CHECK(head.current);
    CHECK(head.generation == known);
}

CatalogueItem movie(std::string id, std::string title) {
    CatalogueItem item;
    item.id = std::move(id);
    item.kind = CatalogueKind::movie;
    item.title = std::move(title);
    return item;
}

MetadataSnapshotView empty_head(uint64_t generation) {
    return MetadataSnapshotView{generation, 0, Hash256{},
                                std::make_shared<const MetadataSnapshot>()};
}

MACHA_TEST("catalogue_maintenance", test_a_commit_installs_what_it_wrote_once) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    catalogue.follow_head();
    const auto cold = catalogue.installs();
    CHECK(cold == 1);
    for (int i = 0; i < 3; ++i) {
        const auto written = catalogue.upsert(movie("movie:one", "Title " + std::to_string(i)));
        CHECK(catalogue.installs() == cold + 1 + static_cast<uint64_t>(i));
        const auto read = catalogue.get("movie:one");
        REQUIRE(read.has_value());
        CHECK(read->revision == written.revision);
        CHECK(catalogue.status().root == metadata.view->snapshot->catalogue_root);
    }
}

MACHA_TEST("catalogue_maintenance", test_an_unchanged_root_is_never_installed_again) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    (void)catalogue.upsert(movie("movie:one", "One"));
    const auto installs = catalogue.installs();
    // New heads that leave the catalogue alone: the view stands.
    for (int i = 0; i < 5; ++i) {
        metadata.advance();
        catalogue.follow_head();
        catalogue.repair_once();
    }
    CHECK(catalogue.installs() == installs);
    CHECK(catalogue.status().metadata_generation == metadata.view->generation);
}

MACHA_TEST("catalogue_maintenance", test_another_writers_commit_is_installed_on_the_next_head) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager reader(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    CatalogueManager writer(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    auto item = reader.upsert(movie("movie:one", "One"));
    item.title = "Edited elsewhere";
    item = writer.upsert(item, item.revision);
    // A warm read is memory-only: the reader still serves its own view.
    CHECK(reader.get("movie:one")->title == "One");
    reader.follow_head();
    const auto seen = reader.get("movie:one");
    REQUIRE(seen.has_value());
    CHECK(seen->title == "Edited elsewhere");
    // Writing on from the revision just seen succeeds: the view is the head.
    item.title = "Edited here";
    const auto written = reader.upsert(item, seen->revision);
    CHECK(written.revision == seen->revision + 1);
    CHECK(reader.get("movie:one")->revision == written.revision);
}

MACHA_TEST("catalogue_maintenance", test_a_root_that_cannot_be_read_keeps_the_view_serving) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    (void)catalogue.upsert(movie("movie:one", "One"));
    const auto installs = catalogue.installs();
    const auto held_root = catalogue.status().root;
    // A head naming a catalogue no node present holds.
    metadata.set_catalogue_root(object_id(test_support::pattern(64, 7)));
    bool failed = false;
    try {
        catalogue.follow_head();
    } catch (const CatalogueUnavailable&) {
        failed = true;
    }
    CHECK(failed);
    CHECK(catalogue.installs() == installs);
    CHECK(catalogue.status().root == held_root);
    CHECK(catalogue.status().error_code == "unavailable");
    REQUIRE(catalogue.get("movie:one").has_value());
}

MACHA_TEST("catalogue_maintenance", test_an_install_racing_a_commit_takes_what_it_wrote) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    catalogue.follow_head();
    const auto loads = catalogue.loads();
    // The installer runs between the head moving and the commit installing.
    metadata.after_commit = [&] { catalogue.follow_head(); };
    for (int i = 0; i < 3; ++i)
        (void)catalogue.upsert(movie("movie:" + std::to_string(i), "Title"));
    CHECK(catalogue.loads() == loads);
    CHECK(catalogue.get("movie:2").has_value());
    // A root nobody staged is read from the control store.
    metadata.after_commit = {};
    CatalogueManager other(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    other.follow_head();
    CHECK(other.loads() == 1);
    CHECK(other.get("movie:2").has_value());
}

MACHA_TEST("catalogue_maintenance", test_resident_bytes_count_what_the_catalogue_holds) {
    CatalogueSnapshot snapshot;
    const auto empty = catalogue_resident_bytes(snapshot);
    CHECK(empty == sizeof(CatalogueSnapshot));
    auto item = movie("movie:one", "One");
    snapshot.items.emplace(item.id, item);
    const auto one = catalogue_resident_bytes(snapshot);
    CHECK(one > empty);
    // The baseline already counted the empty synopsis's inline buffer.
    snapshot.items.at("movie:one").synopsis = std::string(10000, 's');
    const auto long_synopsis = catalogue_resident_bytes(snapshot);
    CHECK(long_synopsis >= one + 10000 - sizeof(std::string));
    CatalogueSnapshot::MediaProfile profile;
    profile.probe.streams.resize(4);
    snapshot.media_profiles.emplace("media:one", profile);
    CHECK(catalogue_resident_bytes(snapshot) >= long_synopsis + 4 * sizeof(MediaStreamInfo));
}

MACHA_TEST("catalogue_maintenance", test_a_view_holds_each_entry_once_in_its_shard) {
    CatalogueSnapshot merged;
    for (int i = 0; i < 500; ++i) {
        auto item = movie("movie:" + std::to_string(i), "Title " + std::to_string(i));
        merged.items.emplace(item.id, item);
        if (i % 3 == 0) {
            CatalogueSnapshot::MediaProfile profile;
            profile.probe.format = "mp4";
            merged.media_profiles.emplace("macha:" + std::to_string(i), profile);
        }
        if (i % 5 == 0)
            merged.media_indexes.emplace("macha:" + std::to_string(i),
                                         object_id(test_support::pattern(16, static_cast<uint8_t>(i))));
    }
    const auto view = CatalogueView::of(merged);
    CHECK(view.item_count() == merged.items.size());
    std::set<std::string> seen;
    for (const auto& [id, item] : view.items()) {
        CHECK(seen.insert(id).second);
        CHECK(item.id == id);
    }
    CHECK(seen.size() == merged.items.size());
    size_t profiles = 0, indexes = 0;
    for (const auto& entry : view.media_profiles()) {
        (void)entry;
        ++profiles;
    }
    for (const auto& entry : view.media_indexes()) {
        (void)entry;
        ++indexes;
    }
    CHECK(profiles == merged.media_profiles.size());
    CHECK(indexes == merged.media_indexes.size());
    for (size_t slot = 0; slot < catalogue_shard_count; ++slot)
        if (const auto& shard = view.shards()[slot])
            for (const auto& [id, _] : shard->items)
                CHECK(catalogue_shard(id) == slot);
    REQUIRE(view.item("movie:7") != nullptr);
    CHECK(view.item("movie:7")->title == "Title 7");
    CHECK(view.item("movie:none") == nullptr);
    CHECK(view.media_profile("macha:3") != nullptr);
    CHECK(view.media_profile("macha:4") == nullptr);
    CHECK(view.media_index("macha:5") != nullptr);
    CHECK(view.merged().items == merged.items);
    CHECK(view.merged().media_profiles == merged.media_profiles);
    CHECK(view.merged().media_indexes == merged.media_indexes);
    // An empty catalogue iterates nothing.
    const CatalogueView empty;
    CHECK(empty.items().begin() == empty.items().end());
}

MACHA_TEST("catalogue_maintenance", test_what_a_commit_installs_is_what_a_load_of_its_root_reads) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager writer(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    // What the writes should leave, kept apart from the catalogue's own code.
    std::map<std::string, std::string> titles;
    std::set<std::string> profiled;
    std::vector<CatalogueItem> batch;
    for (int i = 0; i < 200; ++i) {
        batch.push_back(movie("movie:" + std::to_string(i), "Title " + std::to_string(i)));
        titles[batch.back().id] = batch.back().title;
    }
    (void)writer.upsert_many(batch);
    MediaProbeResult probe;
    probe.format = "mp4";
    probe.duration_seconds = 60;
    MediaStreamInfo stream;
    stream.index = 0;
    stream.type = MediaStreamType::audio;
    stream.codec = "aac";
    probe.streams.push_back(stream);
    uint32_t state = 7;
    const auto next_random = [&] {
        state = state * 1103515245u + 12345u;
        return (state >> 8) % 200;
    };
    const auto matches = [&](CatalogueManager& catalogue) {
        const auto view = catalogue.snapshot_view();
        if (view->item_count() != titles.size())
            return false;
        for (const auto& [id, title] : titles) {
            const auto* item = view->item(id);
            if (!item || item->title != title)
                return false;
        }
        size_t profiles = 0;
        for (const auto& [media_id, _] : view->media_profiles()) {
            if (!profiled.contains(media_id))
                return false;
            ++profiles;
        }
        return profiles == profiled.size();
    };
    for (int round = 0; round < 40; ++round) {
        const auto id = "movie:" + std::to_string(next_random());
        switch (round % 4) {
        case 0:
            if (auto item = writer.get(id)) {
                item->title = "Edited " + std::to_string(round);
                (void)writer.upsert(*item, item->revision);
                titles[id] = item->title;
            }
            break;
        case 1:
            if (writer.erase(id))
                titles.erase(id);
            break;
        case 2: {
            const auto added = movie("movie:new-" + std::to_string(round), "New");
            (void)writer.upsert(added);
            titles[added.id] = added.title;
            break;
        }
        case 3: {
            const auto media_id = "macha:" + std::to_string(round);
            writer.put_media_profile(media_id, probe);
            profiled.insert(media_id);
            break;
        }
        }
        CatalogueManager reader(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
        reader.follow_head();
        REQUIRE(reader.loads() == 1);
        CHECK(matches(writer));
        CHECK(matches(reader));
        CHECK(writer.status().root == reader.status().root);
    }
}

MACHA_TEST("catalogue_maintenance", test_a_load_reuses_every_shard_the_new_root_still_names) {
    Node fixture;
    FakeMetadataView metadata;
    metadata.view = empty_head(fixture.node->known_metadata_generation());
    CatalogueManager writer(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    CatalogueManager reader(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    std::vector<CatalogueItem> batch;
    for (int i = 0; i < 300; ++i)
        batch.push_back(movie("movie:" + std::to_string(i), "Title"));
    (void)writer.upsert_many(batch);
    reader.follow_head();
    const auto before = reader.snapshot_view();
    auto item = *writer.get("movie:42");
    item.title = "Edited";
    (void)writer.upsert(item, item.revision);
    reader.follow_head();
    const auto after = reader.snapshot_view();
    const auto edited = catalogue_shard("movie:42");
    size_t shared = 0;
    for (size_t slot = 0; slot < catalogue_shard_count; ++slot) {
        if (slot == edited) {
            CHECK(after->shards()[slot] != before->shards()[slot]);
            continue;
        }
        CHECK(after->shards()[slot] == before->shards()[slot]);
        shared += after->shards()[slot] ? 1 : 0;
    }
    CHECK(shared > 0);
    CHECK(after->item("movie:42")->title == "Edited");
}

} // namespace
