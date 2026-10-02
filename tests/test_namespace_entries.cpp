// SPDX-License-Identifier: GPL-3.0-or-later
//
// The resumable, budgeted namespace walk `entries(view, cursor, budget)` visits
// exactly the set and order of the callback walk, across budget boundaries, on
// every shape of tree and on a map-backed snapshot.
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace macha;

namespace {

FsEntry file_entry(uint64_t n) {
    FsEntry entry;
    entry.type = EntryType::file;
    entry.size = n;
    ExtentRef extent;
    extent.length = n + 1;
    extent.id.bytes[0] = static_cast<uint8_t>(n);
    extent.id.bytes[1] = static_cast<uint8_t>(n >> 8);
    entry.extents.push_back(extent);
    return entry;
}

// `count` paths in a few directories, so keys share prefixes as real ones do.
std::map<std::string, FsEntry> namespace_of(size_t count) {
    std::map<std::string, FsEntry> out;
    for (size_t i = 0; i < count; ++i)
        out["/d" + std::to_string(i % 7) + "/f" + std::to_string(i)] = file_entry(i);
    return out;
}

// Small fanouts, so a few hundred entries make a tree several levels deep.
NamespaceTreeLimits small_limits() {
    NamespaceTreeLimits limits;
    limits.entry_target_fanout = 3;
    limits.entry_max_fanout = 6;
    limits.branch_target_fanout = 3;
    limits.branch_max_fanout = 6;
    return limits;
}

std::vector<NamespaceItem> by_callback(const MetadataSnapshot& snapshot,
                                       const NamespaceNodeStore* store) {
    std::vector<NamespaceItem> out;
    for_each_namespace_entry(snapshot, store, [&](const std::string& path, const FsEntry& entry) {
        out.emplace_back(path, entry);
    });
    return out;
}

std::vector<NamespaceItem> by_pages(const MetadataSnapshot& snapshot,
                                    const NamespaceNodeStore* store, size_t bound,
                                    size_t& pages) {
    std::vector<NamespaceItem> out;
    Cursor<std::string> cursor;
    pages = 0;
    for (;;) {
        Budget budget;
        budget.operations(bound);
        auto page = namespace_entries(snapshot, store, cursor, budget);
        ++pages;
        CHECK(page.items.size() <= bound);
        for (auto& item : page.items)
            out.push_back(std::move(item));
        if (page.complete()) {
            CHECK(!page.next.after.has_value());
            return out;
        }
        CHECK(page.stopped == Stop::budget);
        CHECK(page.next.after.has_value());
        cursor = page.next;
        if (pages > 2000) {
            CHECK(!"the walk never completed");
            return out;
        }
    }
}

MACHA_FAST_TEST("namespace_entries", test_pages_visit_what_the_callback_walk_visits_on_every_shape) {
    for (size_t count : {0U, 1U, 2U, 3U, 7U, 31U, 100U, 257U}) {
        MemoryNamespaceNodeStore store;
        MetadataSnapshot tree;
        tree.namespace_root = build_namespace_tree(namespace_of(count), store, small_limits());
        MetadataSnapshot map;
        map.entries = namespace_of(count);
        const auto expected = by_callback(tree, &store);
        CHECK(expected.size() == count);
        CHECK(by_callback(map, nullptr) == expected);
        for (size_t bound : {1U, 2U, 3U, 5U, 8U, 64U, 1000U}) {
            size_t pages = 0;
            CHECK(by_pages(tree, &store, bound, pages) == expected);
            // The page that takes the last entry is complete; there is no empty final page.
            CHECK(pages == std::max<size_t>(1, (count + bound - 1) / bound));
            CHECK(by_pages(map, nullptr, bound, pages) == expected);
        }
    }
}

MACHA_FAST_TEST("namespace_entries", test_a_resumed_page_reads_only_its_own_path) {
    MemoryNamespaceNodeStore store;
    MetadataSnapshot tree;
    tree.namespace_root = build_namespace_tree(namespace_of(1000), store, small_limits());
    const auto all = by_callback(tree, &store);
    const size_t total_nodes = store.nodes();

    // Resuming before the last entry reads one root-to-leaf path (and its
    // extent nodes), far fewer than the tree's nodes.
    store.forget_reads();
    Budget budget;
    budget.operations(10);
    const auto page =
        namespace_entries(tree, &store, Cursor<std::string>{all[all.size() - 2].first}, budget);
    CHECK(page.complete());
    CHECK(page.items.size() == 1);
    CHECK(page.items.front().first == all.back().first);
    CHECK(store.reads() < total_nodes / 10);
    CHECK(store.reads() >= 2);
}

MACHA_FAST_TEST("namespace_entries", test_a_cursor_between_paths_resumes_after_it) {
    MetadataSnapshot map;
    map.entries = namespace_of(20);
    MemoryNamespaceNodeStore store;
    MetadataSnapshot tree;
    tree.namespace_root = build_namespace_tree(map.entries, store, small_limits());
    const auto all = by_callback(map, nullptr);
    // From a path not in the namespace, the walk resumes at the next one, in either form.
    const std::string between = all[4].first + "~";
    for (const auto* snapshot : {&map, &tree}) {
        Budget budget;
        budget.operations(100);
        const auto page = namespace_entries(*snapshot, &store, Cursor<std::string>{between}, budget);
        CHECK(!page.items.empty());
        CHECK(page.items.front().first > between);
        CHECK(page.items.front().first <= all[5].first);
    }
}

MACHA_FAST_TEST("namespace_entries", test_cancellation_stops_before_an_entry) {
    MetadataSnapshot map;
    map.entries = namespace_of(5);
    std::atomic_bool cancelled{true};
    Budget budget(WorkContext(FrameType::control, {}, &cancelled));
    budget.operations(100);
    const auto page = namespace_entries(map, nullptr, {}, budget);
    CHECK(page.items.empty());
    CHECK(page.stopped == Stop::cancelled);
}

MACHA_FAST_TEST("namespace_entries", test_a_missing_node_or_store_throws) {
    MemoryNamespaceNodeStore store;
    MetadataSnapshot tree;
    tree.namespace_root = build_namespace_tree(namespace_of(50), store, small_limits());
    MemoryNamespaceNodeStore empty;
    Budget budget;
    bool threw = false;
    try {
        (void)namespace_entries(tree, &empty, {}, budget);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)namespace_entries(tree, nullptr, {}, budget);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

// On a running node the manager pages its own (map-backed) namespace, equal to
// the callback walk over the same view.
namespace {

MACHA_TEST("namespace_entries", test_the_metadata_view_pages_a_live_namespace) {
    macha::test_support::TestService fixture("entries-live");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;
    auto& service = fixture.start();
    for (int i = 0; i < 12; ++i)
        macha::test_support::write_file(service.filesystem(), "/f" + std::to_string(i),
                                        macha::test_support::pattern(1024, i));
    MetadataView& metadata = service.metadata_manager();
    const auto view = metadata.converged();

    auto nodes = ControlNamespaceNodeStore::for_reading(service.node(), service.filesystem().store());
    const auto expected = by_callback(*view.snapshot, &nodes);
    CHECK(expected.size() >= 12);
    std::vector<NamespaceItem> paged;
    Cursor<std::string> cursor;
    for (int pages = 0; pages < 100; ++pages) {
        Budget budget;
        budget.operations(5);
        auto page = metadata.entries(view, cursor, budget);
        for (auto& item : page.items)
            paged.push_back(std::move(item));
        if (page.complete())
            break;
        cursor = page.next;
    }
    CHECK(paged == expected);
}

} // namespace

namespace {

MACHA_FAST_TEST("namespace_entries", test_a_node_that_is_not_a_tree_node_throws) {
    MemoryNamespaceNodeStore store;
    MetadataSnapshot tree;
    tree.namespace_root = build_namespace_tree(namespace_of(40), store, small_limits());
    // The root's address now holds bytes that are no tree node at all.
    store.put_at(*tree.namespace_root, Bytes{'n', 'o', 'p', 'e', 0, 0, 0, 0});
    Budget budget;
    bool threw = false;
    try {
        (void)namespace_entries(tree, &store, {}, budget);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// A manager without a namespace store pages a map-backed view from the map.
MACHA_TEST("namespace_entries", test_a_manager_without_a_store_pages_a_map_backed_view) {
    macha::test_support::TestNode fixture("entries-no-store");
    fixture.prepare();
    auto& node = fixture.start();
    MetadataManager metadata(node);
    MetadataSnapshot snapshot;
    snapshot.entries = namespace_of(9);
    const MetadataSnapshotView view{1, 0, Hash256{},
                                    std::make_shared<const MetadataSnapshot>(snapshot)};
    Budget budget;
    budget.operations(100);
    const auto page = metadata.entries(view, {}, budget);
    CHECK(page.complete());
    CHECK(page.items == by_callback(snapshot, nullptr));
}

} // namespace
