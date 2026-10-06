// SPDX-License-Identifier: GPL-3.0-or-later
//
// The catalogue's maintenance inventory (head, repair, read) against a fake
// metadata view, so no metadata manager is needed behind it.
#include "catalogue/catalogue.hpp"
#include "cluster/distributed_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "test_support.hpp"

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
        return {};
    }
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

MACHA_TEST("catalogue_maintenance", test_a_repair_that_cannot_read_metadata_reports_failure) {
    Node fixture;
    FakeMetadataView metadata;
    CatalogueManager catalogue(*fixture.node, fixture.node->local_state(), fixture.node->metadata_server(), *fixture.store, metadata, fixture.node->ledger());
    CHECK(!catalogue.maintenance_repair());
    // The head taken against no metadata is not current.
    const auto head = catalogue.maintenance_head();
    CHECK(!head.current);
    CHECK(!head.root.has_value());
    CHECK(!catalogue.maintenance_objects(head, false).complete);
}

MACHA_TEST("catalogue_maintenance", test_the_read_is_complete_only_when_the_repair_succeeded) {
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
    CHECK(catalogue.maintenance_repair());
    CHECK(catalogue.maintenance_objects(head, true).complete);
    CHECK(!catalogue.maintenance_objects(head, false).complete);
    // A head that is not current never reads complete, repaired or not.
    auto stale = head;
    stale.current = false;
    CHECK(!catalogue.maintenance_objects(stale, true).complete);
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

} // namespace
