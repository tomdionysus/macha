// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_support.hpp"

#include "api/item_availability.hpp"
#include "filesystem/filesystem.hpp"
#include "ledger/availability.hpp"
#include "metadata/namespace_tree.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

ObjectId extent_id(uint64_t seed) {
    ObjectId id{};
    for (size_t i = 0; i < id.bytes.size(); ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        id.bytes[i] = static_cast<uint8_t>(seed >> 33);
    }
    return id;
}

// Small fanouts, so a few dozen files make branches, leaves and extent nodes
// of more than one level.
NamespaceTreeLimits small_limits() {
    NamespaceTreeLimits limits;
    limits.entry_target_fanout = 3;
    limits.entry_max_fanout = 6;
    limits.branch_target_fanout = 3;
    limits.branch_max_fanout = 6;
    limits.extent_inline_max = 2;
    limits.extent_target_fanout = 3;
    limits.extent_max_fanout = 6;
    return limits;
}

FsEntry file_of(uint64_t first_extent, size_t extents, bool with_hole = false) {
    FsEntry entry;
    entry.type = EntryType::file;
    entry.size = extents * 4096;
    for (size_t i = 0; i < extents; ++i) {
        ExtentRef extent;
        extent.offset = i * 4096;
        extent.length = 4096;
        if (with_hole && i == 1)
            extent.hole = true;
        else
            extent.id = extent_id(first_extent + i);
        entry.extents.push_back(extent);
    }
    return entry;
}

// `files` files of 0..12 extents; file f's extents are seeds f*100...
std::map<std::string, FsEntry> library(size_t files) {
    std::map<std::string, FsEntry> entries;
    FsEntry root;
    root.type = EntryType::directory;
    entries["/"] = root;
    for (size_t f = 0; f < files; ++f)
        entries["/f" + std::to_string(1000 + f)] = file_of(f * 100, f % 13, f % 5 == 4);
    return entries;
}

std::vector<ObjectId> references(const std::map<std::string, FsEntry>& entries) {
    std::vector<ObjectId> ids;
    for (const auto& [_, entry] : entries)
        for (const auto& extent : entry.extents)
            if (!extent.hole)
                ids.push_back(extent.id);
    return ids;
}

HeldFn holds(const std::set<ObjectId>& ids) {
    return [&ids](const ObjectId& id) { return ids.contains(id); };
}

// A peer with its own tree and holdings, answering as a node would.
class FakePeer final : public PeerHoldings {
  public:
    FakePeer(const ObjectId& root, const NamespaceNodeStore& store, std::set<ObjectId> held)
        : store_(store), held_(std::move(held)),
          rollup_(HoldingsRollup::build(root, store, holds(held_))) {}

    std::vector<NodeHoldings> ask(std::span<const ObjectId> nodes) override {
        ++asks;
        asked += nodes.size();
        if (failing)
            throw std::runtime_error("peer unreachable");
        std::vector<NodeHoldings> answers;
        for (const auto& node : nodes)
            answers.push_back(describe_holdings(rollup_, store_, holds(held_), node));
        return answers;
    }

    size_t asks{};
    size_t asked{};
    bool failing{};

  private:
    const NamespaceNodeStore& store_;
    std::set<ObjectId> held_;
    HoldingsRollup rollup_;
};

// Every reference reached by descending children from the root.
void collect(const ObjectId& node, const NamespaceNodeStore& store, std::vector<ObjectId>& extents,
             size_t& nodes) {
    ++nodes;
    for (const auto& child : namespace_tree_children(*store.get(node))) {
        if (child.extent)
            extents.push_back(child.id);
        else
            collect(child.id, store, extents, nodes);
    }
}

// A deterministic subset: about `percent` of the ids, varied by `salt`.
std::set<ObjectId> subset(const std::vector<ObjectId>& ids, unsigned percent, unsigned salt) {
    std::set<ObjectId> out;
    for (const auto& id : ids)
        if ((id.bytes[salt % 32] * 100U) / 256U < percent)
            out.insert(id);
    return out;
}

} // namespace

MACHA_FAST_TEST("availability", test_a_trees_children_are_exactly_its_extent_references) {
    for (const size_t files : {0U, 1U, 7U, 40U}) {
        MemoryNamespaceNodeStore store;
        const auto entries = library(files);
        const auto root = build_namespace_tree(entries, store, small_limits());
        std::vector<ObjectId> reached;
        size_t nodes = 0;
        collect(root, store, reached, nodes);
        auto expected = references(entries);
        std::sort(reached.begin(), reached.end());
        std::sort(expected.begin(), expected.end());
        CHECK(reached == expected);
    }
    // Anything that is not a tree node is refused.
    bool refused = false;
    try {
        (void)namespace_tree_children(Bytes{'n', 'o', 'p', 'e', 0, 0, 0, 0});
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_FAST_TEST("availability", test_the_rollup_counts_references_and_what_is_held) {
    MemoryNamespaceNodeStore store;
    const auto entries = library(40);
    const auto root = build_namespace_tree(entries, store, small_limits());
    const auto all = references(entries);
    const auto held = subset(all, 40, 3);

    size_t pauses = 0;
    const auto rollup = HoldingsRollup::build(root, store, holds(held), [&] { ++pauses; });
    CHECK(rollup.root() == root);
    CHECK(rollup.total().extents == all.size());
    CHECK(rollup.total().held ==
          static_cast<uint64_t>(std::count_if(all.begin(), all.end(), holds(held))));
    CHECK(!rollup.total().complete());
    // One pause per tree node read, and every node read once.
    CHECK(pauses == rollup.nodes());
    CHECK(!rollup.find(extent_id(999999)).has_value());

    // Each node's count is the sum over its children.
    std::vector<ObjectId> pending{root};
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        Holding sum;
        for (const auto& child : namespace_tree_children(*store.get(node))) {
            if (child.extent) {
                ++sum.extents;
                sum.held += held.contains(child.id) ? 1 : 0;
            } else {
                const auto beneath = rollup.find(child.id);
                REQUIRE(beneath.has_value());
                sum.extents += beneath->extents;
                sum.held += beneath->held;
                pending.push_back(child.id);
            }
        }
        CHECK(rollup.find(node) == std::optional<Holding>(sum));
    }

    // Holding everything, or nothing referenced, is complete.
    CHECK(HoldingsRollup::build(root, store, [](const ObjectId&) { return true; })
              .total()
              .complete());
    MemoryNamespaceNodeStore empty_store;
    const auto empty_root = build_namespace_tree(library(0), empty_store, small_limits());
    CHECK(HoldingsRollup::build(empty_root, empty_store, [](const ObjectId&) { return false; })
              .total()
              .complete());
}

MACHA_FAST_TEST("availability", test_the_rollup_reads_a_shared_subtree_once_and_counts_it_twice) {
    MemoryNamespaceNodeStore store;
    std::map<std::string, FsEntry> entries;
    entries["/a"] = file_of(500, 12);
    entries["/b"] = file_of(500, 12); // the same content: one extent sequence
    const auto root = build_namespace_tree(entries, store, small_limits());
    store.forget_reads();
    size_t asked = 0;
    const auto rollup = HoldingsRollup::build(root, store, [&](const ObjectId&) {
        ++asked;
        return false;
    });
    CHECK(rollup.total().extents == 24);
    CHECK(asked == 12);
    CHECK(store.reads() == rollup.nodes());
}

MACHA_FAST_TEST("availability", test_a_rollup_refuses_a_tree_with_a_missing_node) {
    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(library(20), store, small_limits());
    MemoryNamespaceNodeStore damaged;
    bool first = true;
    for (const auto& id : store.written()) {
        if (id != root && first) {
            first = false;
            continue;
        }
        damaged.put_at(id, *store.get(id));
    }
    bool refused = false;
    try {
        (void)HoldingsRollup::build(root, damaged, [](const ObjectId&) { return true; });
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_FAST_TEST("availability", test_a_node_describes_each_child_it_holds_whole) {
    MemoryNamespaceNodeStore store;
    const auto entries = library(40);
    const auto root = build_namespace_tree(entries, store, small_limits());
    const auto held = subset(references(entries), 70, 5);
    const auto rollup = HoldingsRollup::build(root, store, holds(held));

    for (const auto& node : store.written()) {
        const auto answer = describe_holdings(rollup, store, holds(held), node);
        const auto children = namespace_tree_children(*store.get(node));
        REQUIRE(answer.children.size() == children.size());
        CHECK(answer.known == rollup.find(node).has_value());
        for (size_t c = 0; c < children.size(); ++c) {
            const bool expected = children[c].extent
                                      ? held.contains(children[c].id)
                                      : rollup.find(children[c].id)->complete();
            CHECK(answer.children[c] == expected);
        }
    }

    // A tree node this node has never seen: no count, no flags.
    const auto stranger = describe_holdings(rollup, store, holds(held), extent_id(424242));
    CHECK(!stranger.known);
    CHECK(stranger.children.empty());
}

MACHA_FAST_TEST("availability", test_the_survey_finds_exactly_what_no_node_holds) {
    MemoryNamespaceNodeStore store;
    const auto entries = library(60);
    const auto root = build_namespace_tree(entries, store, small_limits());
    const auto all = references(entries);

    // Every mix of sparse and dense holdings across this node and two peers.
    for (const unsigned here_percent : {0U, 15U, 60U, 100U})
        for (const unsigned a_percent : {0U, 30U, 95U, 100U})
            for (const unsigned b_percent : {0U, 50U, 100U}) {
                const auto here = subset(all, here_percent, 1);
                const auto local = HoldingsRollup::build(root, store, holds(here));
                FakePeer a(root, store, subset(all, a_percent, 7));
                FakePeer b(root, store, subset(all, b_percent, 19));
                std::vector<PeerHoldings*> peers{&a, &b};
                const auto survey = survey_availability(local, store, holds(here), peers);

                const auto a_held = subset(all, a_percent, 7);
                const auto b_held = subset(all, b_percent, 19);
                std::set<ObjectId> nobody;
                for (const auto& id : all)
                    if (!here.contains(id) && !a_held.contains(id) && !b_held.contains(id))
                        nobody.insert(id);
                CHECK(survey.unavailable ==
                      std::vector<ObjectId>(nobody.begin(), nobody.end()));
                CHECK(survey.unknown.empty());
                CHECK(survey.peers_asked == 2);
                CHECK(survey.peers_failed == 0);
                for (const auto& id : all)
                    CHECK(survey.is_unavailable(id) == nobody.contains(id));
            }
}

MACHA_FAST_TEST("availability", test_the_survey_costs_what_differs_not_the_library) {
    MemoryNamespaceNodeStore store;
    const auto entries = library(60);
    const auto root = build_namespace_tree(entries, store, small_limits());
    const auto all = references(entries);
    const std::set<ObjectId> none;
    const std::set<ObjectId> everything(all.begin(), all.end());

    // This node holds everything: no question is asked.
    {
        const auto local = HoldingsRollup::build(root, store, holds(everything));
        FakePeer peer(root, store, none);
        std::vector<PeerHoldings*> peers{&peer};
        const auto survey = survey_availability(local, store, holds(everything), peers);
        CHECK(survey.rounds == 0);
        CHECK(peer.asks == 0);
        CHECK(survey.unavailable.empty());
    }
    // This node holds nothing, a peer everything: one id settles the library.
    {
        const auto local = HoldingsRollup::build(root, store, holds(none));
        FakePeer peer(root, store, everything);
        std::vector<PeerHoldings*> peers{&peer};
        const auto survey = survey_availability(local, store, holds(none), peers);
        CHECK(survey.rounds == 1);
        CHECK(survey.nodes_asked == 1);
        CHECK(survey.unavailable.empty());
    }
    // The peer lacks one extent of a large file: the descent follows one path.
    {
        const auto missing = entries.at("/f1012").extents.at(7).id;
        auto most = everything;
        most.erase(missing);
        const auto local = HoldingsRollup::build(root, store, holds(none));
        FakePeer peer(root, store, most);
        std::vector<PeerHoldings*> peers{&peer};
        const auto survey = survey_availability(local, store, holds(none), peers);
        CHECK(survey.unavailable == std::vector<ObjectId>{missing});
        const auto stats = namespace_tree_stats(root, store);
        // One tree node per level on the way down: the namespace spine, then
        // the file's extent sequence.
        CHECK(survey.nodes_asked == survey.rounds);
        CHECK(survey.rounds <= stats.depth + 4);
        CHECK(survey.nodes_asked < local.nodes() / 4);
    }
    // No peer at all: what this node lacks, nobody reachable holds.
    {
        const auto here = subset(all, 50, 2);
        const auto local = HoldingsRollup::build(root, store, holds(here));
        const auto survey = survey_availability(local, store, holds(here), {});
        std::set<ObjectId> lacking;
        for (const auto& id : all)
            if (!here.contains(id))
                lacking.insert(id);
        CHECK(survey.unavailable == std::vector<ObjectId>(lacking.begin(), lacking.end()));
    }
}

MACHA_FAST_TEST("availability", test_a_peer_at_another_generation_answers_for_shared_subtrees) {
    MemoryNamespaceNodeStore store;
    auto entries = library(60);
    const auto root = build_namespace_tree(entries, store, small_limits());
    // The peer is one commit behind: it lacks a file this node has, and has
    // one this node has removed.
    auto peer_entries = entries;
    peer_entries.erase("/f1030");
    peer_entries["/old"] = file_of(90000, 9);
    const auto peer_root = build_namespace_tree(peer_entries, store, small_limits());
    REQUIRE(peer_root != root);

    const auto all = references(entries);
    const auto peer_all = references(peer_entries);
    const std::set<ObjectId> none;
    const std::set<ObjectId> peer_held(peer_all.begin(), peer_all.end());
    const auto local = HoldingsRollup::build(root, store, holds(none));
    FakePeer peer(peer_root, store, peer_held);
    std::vector<PeerHoldings*> peers{&peer};
    const auto survey = survey_availability(local, store, holds(none), peers);

    // Exactly the new file's extents are held by nobody; nothing is undecided,
    // because the peer has the tree nodes' bytes and answers by child.
    std::set<ObjectId> expected;
    for (const auto& extent : entries.at("/f1030").extents)
        if (!extent.hole)
            expected.insert(extent.id);
    CHECK(survey.unavailable == std::vector<ObjectId>(expected.begin(), expected.end()));
    CHECK(survey.unknown.empty());
    CHECK(survey.nodes_asked < local.nodes());
}

MACHA_FAST_TEST("availability", test_what_no_peer_could_describe_is_unknown_not_unavailable) {
    MemoryNamespaceNodeStore store;
    const auto entries = library(30);
    const auto root = build_namespace_tree(entries, store, small_limits());
    const auto all = references(entries);
    const std::set<ObjectId> none;
    const auto local = HoldingsRollup::build(root, store, holds(none));

    // A peer that cannot be reached: everything this node lacks is undecided.
    {
        FakePeer peer(root, store, std::set<ObjectId>(all.begin(), all.end()));
        peer.failing = true;
        std::vector<PeerHoldings*> peers{&peer};
        const auto survey = survey_availability(local, store, holds(none), peers);
        CHECK(survey.peers_failed == 1);
        CHECK(peer.asks == 1); // not asked again after failing
        CHECK(survey.unavailable.empty());
        const std::set<ObjectId> everything(all.begin(), all.end());
        CHECK(survey.unknown == std::vector<ObjectId>(everything.begin(), everything.end()));
    }
    // A peer with a different namespace and none of this tree's nodes: it
    // cannot describe them, so it decides nothing.
    {
        MemoryNamespaceNodeStore other_store;
        std::map<std::string, FsEntry> other;
        other["/elsewhere"] = file_of(70000, 5);
        const auto other_root = build_namespace_tree(other, other_store, small_limits());
        FakePeer peer(other_root, other_store, none);
        std::vector<PeerHoldings*> peers{&peer};
        const auto survey = survey_availability(local, store, holds(none), peers);
        CHECK(survey.peers_failed == 0);
        CHECK(survey.unavailable.empty());
        CHECK(survey.unknown.size() == std::set<ObjectId>(all.begin(), all.end()).size());
        CHECK(survey.is_unknown(all.front()));
    }
    // One peer answers and holds nothing, one fails: still undecided, since
    // the failed peer might hold it.
    {
        FakePeer answering(root, store, none);
        FakePeer failing(root, store, none);
        failing.failing = true;
        std::vector<PeerHoldings*> peers{&answering, &failing};
        const auto survey = survey_availability(local, store, holds(none), peers);
        CHECK(survey.unavailable.empty());
        CHECK(!survey.unknown.empty());
    }
}

MACHA_FAST_TEST("availability", test_questions_and_answers_survive_the_wire) {
    std::vector<ObjectId> nodes;
    for (uint64_t i = 0; i < 5; ++i)
        nodes.push_back(extent_id(i));
    CHECK(decode_tree_holdings_request(encode_tree_holdings_request(nodes)) == nodes);
    CHECK(decode_tree_holdings_request(encode_tree_holdings_request({})).empty());

    std::vector<NodeHoldings> answers;
    answers.push_back({});                                // a stranger
    answers.push_back({true, {12, 12}, {}});              // whole, bytes not held
    for (const size_t children : {1U, 7U, 8U, 9U, 17U}) { // every byte boundary
        NodeHoldings answer{true, {children, 0}, {}};
        for (size_t c = 0; c < children; ++c) {
            answer.children.push_back(c % 3 == 0);
            answer.holding.held += c % 3 == 0 ? 1 : 0;
        }
        answers.push_back(answer);
    }
    CHECK(decode_tree_holdings_reply(encode_tree_holdings_reply(answers)) == answers);

    const auto refused = [](auto decode, Bytes bytes) {
        try {
            (void)decode(bytes);
        } catch (const DecodeError&) {
            return true;
        }
        return false;
    };
    auto truncated = encode_tree_holdings_reply(answers);
    truncated.pop_back();
    CHECK(refused(decode_tree_holdings_reply, truncated));
    auto trailing = encode_tree_holdings_request(nodes);
    trailing.push_back(0);
    CHECK(refused(decode_tree_holdings_request, trailing));
    // More held than referenced is not a holding.
    CHECK(refused(decode_tree_holdings_reply,
                  encode_tree_holdings_reply(std::vector<NodeHoldings>{{true, {1, 2}, {}}})));
    bool too_many = false;
    try {
        (void)encode_tree_holdings_request(std::vector<ObjectId>(tree_holdings_max + 1));
    } catch (const std::invalid_argument&) {
        too_many = true;
    }
    CHECK(too_many);
}

MACHA_FAST_TEST("availability", test_a_files_code_follows_its_extent_counts) {
    CHECK(availability_of(nullptr) == Availability::unknown);
    PathAvailability facts;
    CHECK(availability_of(&facts) == Availability::complete); // no extents: nothing needed
    facts.extents = 4;
    facts.extents_local = 4;
    CHECK(availability_of(&facts) == Availability::complete);
    facts.extents_local = 1; // the rest on a peer
    CHECK(availability_of(&facts) == Availability::complete);
    facts.extents_unknown = 1;
    CHECK(availability_of(&facts) == Availability::unknown);
    facts.extents_unavailable = 1; // definitely short, whatever is undecided
    CHECK(availability_of(&facts) == Availability::partial);
    facts.extents_unknown = 0;
    facts.extents_unavailable = 4;
    CHECK(availability_of(&facts) == Availability::unavailable);
    CHECK(std::string(availability_name(Availability::unavailable)) == "unavailable");
}

MACHA_FAST_TEST("availability", test_items_take_their_best_file_and_sets_count_their_members) {
    AvailabilitySnapshot survey;
    const auto file = [&](const std::string& hash, uint64_t extents, uint64_t unavailable,
                          uint64_t unknown = 0) {
        PathAvailability facts;
        facts.extents = extents;
        facts.extents_unavailable = unavailable;
        facts.extents_unknown = unknown;
        facts.hash = hash;
        survey.paths["/" + hash] = facts;
        survey.by_hash.emplace(hash, "/" + hash);
    };
    file("macha:whole", 4, 0);
    file("macha:holed", 4, 1);
    file("macha:gone", 4, 4);
    file("macha:undecided", 4, 0, 2);

    CatalogueSnapshot catalogue;
    const auto item = [&](const std::string& id, CatalogueKind kind,
                          std::vector<std::string> media, std::optional<std::string> parent = {}) {
        CatalogueItem value;
        value.id = id;
        value.kind = kind;
        value.media_ids = std::move(media);
        value.parent_id = std::move(parent);
        catalogue.items[id] = value;
    };
    item("m-whole", CatalogueKind::movie, {"macha:whole"});
    item("m-holed", CatalogueKind::movie, {"macha:holed"});
    item("m-gone", CatalogueKind::movie, {"macha:gone"});
    item("m-undecided", CatalogueKind::movie, {"macha:undecided"});
    item("m-unsurveyed", CatalogueKind::movie, {"macha:never-seen"});
    item("m-two-files", CatalogueKind::movie, {"macha:gone", "macha:whole"}); // any one plays
    item("m-holed-or-gone", CatalogueKind::movie, {"macha:gone", "macha:holed"});
    item("m-no-files", CatalogueKind::movie, {});

    item("show", CatalogueKind::show, {});
    item("s1", CatalogueKind::season, {}, "show");
    item("s1e1", CatalogueKind::episode, {"macha:whole"}, "s1");
    item("s1e2", CatalogueKind::episode, {"macha:gone"}, "s1");
    item("s2", CatalogueKind::season, {}, "show");
    item("s2e1", CatalogueKind::episode, {"macha:whole"}, "s2");
    item("s3", CatalogueKind::season, {}, "show"); // nothing in it
    item("lost-show", CatalogueKind::show, {});
    item("lost-e1", CatalogueKind::episode, {"macha:gone"}, "lost-show");
    item("new-show", CatalogueKind::show, {});
    item("new-e1", CatalogueKind::episode, {"macha:whole"}, "new-show");
    item("new-e2", CatalogueKind::episode, {"macha:undecided"}, "new-show");

    const auto table = item_availability(catalogue, &survey);
    REQUIRE(table.size() == catalogue.items.size());
    const auto status = [&](const char* id) { return table.at(id).status; };
    CHECK(status("m-whole") == Availability::complete);
    CHECK(status("m-holed") == Availability::partial);
    CHECK(status("m-gone") == Availability::unavailable);
    CHECK(status("m-undecided") == Availability::unknown);
    CHECK(status("m-unsurveyed") == Availability::unknown);
    CHECK(status("m-two-files") == Availability::complete);
    CHECK(status("m-holed-or-gone") == Availability::partial);
    CHECK(status("m-no-files") == Availability::unknown);
    CHECK(table.at("m-whole").members == 0);

    // A season of one whole and one lost episode; the show adds a whole season.
    CHECK((table.at("s1") == ItemAvailability{Availability::partial, 2, 1, 0, 1, 0}));
    CHECK((table.at("s2") == ItemAvailability{Availability::complete, 1, 1, 0, 0, 0}));
    CHECK((table.at("s3") == ItemAvailability{Availability::unknown, 0, 0, 0, 0, 0}));
    CHECK((table.at("show") == ItemAvailability{Availability::partial, 3, 2, 0, 1, 0}));
    CHECK((table.at("lost-show") == ItemAvailability{Availability::unavailable, 1, 0, 0, 1, 0}));
    // Complete as far as is known, with one member undecided.
    CHECK((table.at("new-show") == ItemAvailability{Availability::unknown, 2, 1, 0, 0, 1}));

    // Nothing surveyed: every item is unknown, and sets still count members.
    const auto blind = item_availability(catalogue, nullptr);
    for (const auto& [id, entry] : blind)
        CHECK(entry.status == Availability::unknown);
    CHECK(blind.at("show").members == 3);

    // A parent cycle ends instead of looping.
    catalogue.items["show"].parent_id = "s1";
    CHECK(item_availability(catalogue, &survey).size() == catalogue.items.size());
}

MACHA_FAST_TEST("availability", test_the_item_table_is_rebuilt_only_when_a_snapshot_changes) {
    auto catalogue = std::make_shared<CatalogueSnapshot>();
    CatalogueItem movie;
    movie.id = "m";
    movie.media_ids = {"macha:x"};
    catalogue->items["m"] = movie;
    auto survey = std::make_shared<AvailabilitySnapshot>();

    ItemAvailabilityCache cache;
    const auto first = cache.table(catalogue, survey);
    CHECK(cache.table(catalogue, survey) == first);
    CHECK(first->at("m").status == Availability::unknown);

    auto surveyed = std::make_shared<AvailabilitySnapshot>();
    PathAvailability facts;
    facts.extents = 2;
    facts.hash = "macha:x";
    surveyed->paths["/x"] = facts;
    surveyed->by_hash.emplace("macha:x", "/x");
    const auto second = cache.table(catalogue, surveyed);
    CHECK(second != first);
    CHECK(second->at("m").status == Availability::complete);
    CHECK(cache.table(catalogue, nullptr)->at("m").status == Availability::unknown);
}

MACHA_FAST_TEST("availability", test_a_file_the_survey_cannot_decide_takes_its_last_known_count) {
    MetadataSnapshot head;
    head.entries["/"].type = EntryType::directory;
    head.entries["/undecided"] = file_of(100, 4);
    head.entries["/short-now"] = file_of(200, 4);
    head.entries["/never-decided"] = file_of(300, 4);
    head.entries["/new"] = file_of(400, 4);
    const auto hash = [&](const char* path) { return file_media_id(head.entries.at(path)); };

    // The last survey, under other names: what a file is, is its content.
    AvailabilitySnapshot last;
    const auto known = [&](const std::string& path, const std::string& file_hash,
                           uint64_t unavailable, uint64_t unknown) {
        PathAvailability facts;
        facts.extents = 4;
        facts.extents_unavailable = unavailable;
        facts.extents_unknown = unknown;
        facts.hash = file_hash;
        last.paths[path] = facts;
        last.by_hash.emplace(file_hash, path);
    };
    known("/was-undecided", hash("/undecided"), 1, 0);
    known("/was-short-now", hash("/short-now"), 1, 0);
    known("/was-never-decided", hash("/never-decided"), 0, 2);

    // Nothing held here; the one peer could not be asked, except that two
    // extents of /short-now are now known to be held nowhere.
    AvailabilitySnapshot next;
    for (const auto& [_, entry] : head.entries)
        for (const auto& extent : entry.extents)
            next.survey.unknown.push_back(extent.id);
    next.survey.unavailable = {head.entries.at("/short-now").extents.at(0).id,
                               head.entries.at("/short-now").extents.at(1).id};
    std::erase_if(next.survey.unknown,
                  [&](const ObjectId& id) { return next.survey.is_unavailable(id); });
    std::sort(next.survey.unknown.begin(), next.survey.unknown.end());
    std::sort(next.survey.unavailable.begin(), next.survey.unavailable.end());

    MemoryNamespaceNodeStore store;
    const std::set<ObjectId> none;
    fill_path_table(next, head, store, holds(none), &last);
    const auto facts = [&](const char* path) { return next.paths.at(path); };
    CHECK(facts("/undecided").extents_unavailable == 1);
    CHECK(facts("/undecided").extents_unknown == 0);
    CHECK(availability_of(&next.paths.at("/undecided")) == Availability::partial);
    // A count found now is not lowered by an older one.
    CHECK(facts("/short-now").extents_unavailable == 2);
    CHECK(facts("/short-now").extents_unknown == 0);
    // Undecided before as well: still unknown.
    CHECK(facts("/never-decided").extents_unknown == 4);
    CHECK(facts("/new").extents_unknown == 4);
    CHECK(facts("/").extents_unavailable == 3);
    CHECK(facts("/").extents_unknown == 8);

    // What this node holds itself is never counted short.
    const auto& undecided = head.entries.at("/undecided").extents;
    const std::set<ObjectId> three{undecided.at(0).id, undecided.at(1).id, undecided.at(2).id};
    AvailabilitySnapshot held_here;
    held_here.survey = next.survey;
    fill_path_table(held_here, head, store, holds(three), &last);
    CHECK(held_here.paths.at("/undecided").extents_local == 3);
    CHECK(held_here.paths.at("/undecided").extents_unavailable == 1);

    // Kept on disk and read back, extent lists aside.
    next.generation = 7;
    next.surveyed_unix_ms = 1234;
    const auto bytes = encode_availability_paths(next);
    const auto back = decode_availability_paths(bytes);
    CHECK(back.generation == 7);
    CHECK(back.surveyed_unix_ms == 1234);
    CHECK(back.paths == next.paths);
    CHECK(back.by_hash == next.by_hash);
    CHECK(back.survey.unknown.empty());
    const auto refused = [](Bytes damaged) {
        try {
            (void)decode_availability_paths(damaged);
        } catch (const DecodeError&) {
            return true;
        }
        return false;
    };
    auto truncated = bytes;
    truncated.pop_back();
    CHECK(refused(truncated));
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK(refused(trailing));
    AvailabilitySnapshot impossible;
    impossible.paths["/x"] = PathAvailability{false, 1, 2, 2, 1, 0, "macha:x"};
    CHECK(refused(encode_availability_paths(impossible)));
}

// Two real nodes: the pass rolls up and surveys, a peer answers over RPC, and
// the path table says what a reader would be told.
MACHA_TEST("availability", test_two_nodes_survey_what_neither_holds) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto first_port = free_port();
    const auto second_port = free_port();
    auto first_config =
        cluster.node_config("availability-first", first_port, {{"127.0.0.1", second_port}});
    auto second_config =
        cluster.node_config("availability-second", second_port, {{"127.0.0.1", first_port}});
    for (auto* config : {&first_config, &second_config}) {
        config->replication = 2;
        config->min_write_replicas = 2;
        config->catalogue.scanner.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
    }

    Service first(first_config, cluster.keys());
    Service second(second_config, cluster.keys());
    first.start();
    second.start();
    (void)first.filesystem();
    (void)second.filesystem();
    REQUIRE(wait_until(
        [&] {
            return first.node().membership().active().size() == 2 &&
                   second.node().membership().active().size() == 2;
        },
        10s));

    const auto extent = first_config.extent_size;
    REQUIRE(retry_while_not_ready(
        [&] { write_file(first.filesystem(), "/whole.bin", pattern(extent * 3, 1)); }));
    first.filesystem().mkdir("/dir", 0755, getuid(), getgid());
    write_file(first.filesystem(), "/dir/holed.bin", pattern(extent * 3, 2));

    const auto path_of = [](Service& service, std::string_view path)
        -> std::optional<PathAvailability> {
        const auto snapshot = service.availability().snapshot();
        if (!snapshot)
            return {};
        const auto found = snapshot->paths.find(path);
        if (found == snapshot->paths.end())
            return {};
        return found->second;
    };
    // Both nodes hold both files: each surveys them complete.
    for (auto* service : {&first, &second})
        REQUIRE(wait_until(
            [&] {
                const auto holed = path_of(*service, "/dir/holed.bin");
                return holed && holed->extents == 3 && holed->extents_local == 3 &&
                       holed->extents_unavailable == 0 && holed->extents_unknown == 0;
            },
            20s));

    // One extent of one file leaves both nodes; the next commit moves the
    // namespace, and both nodes survey again.
    const auto lost = first.filesystem().getattr("/dir/holed.bin").extents.at(1).id;
    REQUIRE(first.local_state().data().remove(lost));
    REQUIRE(second.local_state().data().remove(lost));
    write_file(first.filesystem(), "/later.bin", pattern(extent, 3));

    for (auto* service : {&first, &second}) {
        REQUIRE(wait_until(
            [&] {
                // The later file with its data: it is named before its
                // extent is committed.
                const auto holed = path_of(*service, "/dir/holed.bin");
                const auto later = path_of(*service, "/later.bin");
                return holed && holed->extents_unavailable == 1 && later && later->extents == 1;
            },
            20s));
        const auto snapshot = service->availability().snapshot();
        CHECK(snapshot->survey.unavailable == std::vector<ObjectId>{lost});
        CHECK(snapshot->survey.unknown.empty());
        CHECK(snapshot->survey.peers_asked == 1);
        CHECK(snapshot->survey.peers_failed == 0);
        CHECK(service->availability().unavailable(lost));

        const auto holed = *path_of(*service, "/dir/holed.bin");
        CHECK(holed.extents == 3);
        CHECK(holed.extents_local == 2);
        CHECK(!holed.directory);
        CHECK(holed.hash.starts_with("macha:"));
        const auto whole = *path_of(*service, "/whole.bin");
        CHECK(whole.extents == 3);
        CHECK(whole.extents_unavailable == 0);
        // Directories sum what is beneath them.
        const auto dir = *path_of(*service, "/dir");
        CHECK(dir.directory);
        CHECK(dir.extents == 3);
        CHECK(dir.extents_unavailable == 1);
        const auto root = *path_of(*service, "/");
        CHECK(root.extents == 7);
        CHECK(root.extents_unavailable == 1);
        CHECK(root.size == extent * 7);
        // By content identity.
        const auto paths = snapshot->by_hash.equal_range(holed.hash);
        REQUIRE(paths.first != paths.second);
        CHECK(paths.first->second == "/dir/holed.bin");
    }

    // Each node keeps its last survey, to answer with until its next.
    for (auto* service : {&first, &second}) {
        const auto kept = service->node().config().state_path / "availability" / "last-survey.bin";
        CHECK(wait_until(
            [&] {
                std::ifstream in(kept, std::ios::binary);
                const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
                return !bytes.empty() &&
                       decode_availability_paths(bytes).paths ==
                           service->availability().snapshot()->paths;
            },
            10s));
    }

    // What a client is told.
    const auto get = [&](std::string path, std::string hash = {}) {
        HttpRequest request;
        request.method = "GET";
        request.path = std::move(path);
        if (!hash.empty())
            request.query["hash"] = std::move(hash);
        return first.files_api().handle(request);
    };
    const auto json = [](const HttpResponse& response) {
        return Json::parse(std::string(reinterpret_cast<const char*>(response.body.data()),
                                       response.body.size()));
    };
    {
        const auto response = get("/api/v1/files/dir/holed.bin");
        REQUIRE(response.status == 200);
        const auto file = json(response);
        CHECK(file.find("status")->asString() == "ok");
        CHECK(file.find("path")->asString() == "/dir/holed.bin");
        CHECK(file.find("name")->asString() == "holed.bin");
        CHECK(file.find("type")->asString() == "file");
        CHECK(file.find("size")->asUInt64() == extent * 3);
        CHECK(file.find("media_id")->asString().starts_with("macha:"));
        CHECK(file.find("extents")->asUInt64() == 3);
        CHECK(file.find("extents_local")->asUInt64() == 2);
        CHECK(file.find("extents_unavailable")->asUInt64() == 1);
        CHECK(file.find("extents_unknown")->asUInt64() == 0);
        CHECK(file.find("availability")->asString() == "partial");
        CHECK(file.find("surveyed_generation")->asUInt64() > 0);
        CHECK(file.find("entries") == nullptr);

        // The same file by content identity.
        const auto by_hash = get("/api/v1/files", file.find("media_id")->asString());
        REQUIRE(by_hash.status == 200);
        const auto listing = json(by_hash);
        const auto& files = listing.find("files")->asArray();
        REQUIRE(files.size() == 1);
        CHECK(files.front().find("path")->asString() == "/dir/holed.bin");
        CHECK(files.front().find("availability")->asString() == "partial");
    }
    {
        // A directory carries what is beneath it, and lists its entries.
        const auto root = json(get("/api/v1/files/"));
        CHECK(root.find("type")->asString() == "directory");
        CHECK(root.find("extents")->asUInt64() == 7);
        CHECK(root.find("availability")->asString() == "partial");
        std::map<std::string, std::string> listed;
        for (const auto& entry : root.find("entries")->asArray())
            listed[entry.find("path")->asString()] = entry.find("availability")->asString();
        CHECK(listed == (std::map<std::string, std::string>{
                            {"/dir", "partial"}, {"/later.bin", "complete"}, {"/whole.bin", "complete"}}));
        CHECK(json(get("/api/v1/files/dir")).find("entries")->asArray().size() == 1);
        CHECK(json(get("/api/v1/files")).find("path")->asString() == "/");
    }
    {
        // The same answer on catalogue items: a title with one holed file is
        // partial; with a whole file as well it is complete; a set counts.
        const auto media_id = json(get("/api/v1/files/dir/holed.bin")).find("media_id")->asString();
        const auto whole_id = json(get("/api/v1/files/whole.bin")).find("media_id")->asString();
        CatalogueItem show;
        show.id = "test:show:availability";
        show.kind = CatalogueKind::show;
        show.title = "Availability";
        first.catalogue().upsert(show);
        CatalogueItem holed;
        holed.id = "test:movie:holed";
        holed.kind = CatalogueKind::movie;
        holed.title = "Holed";
        holed.media_ids = {media_id};
        first.catalogue().upsert(holed);
        CatalogueItem both = holed;
        both.id = "test:movie:both";
        both.title = "Both";
        both.media_ids = {media_id, whole_id};
        first.catalogue().upsert(both);

        const auto item = [&](const std::string& id) {
            HttpRequest request;
            request.method = "GET";
            request.path = "/api/v1/catalogue/items/" + id;
            const auto response = first.catalogue_api().handle(request);
            REQUIRE(response.status == 200);
            return json(response);
        };
        // The survey is of the namespace, which these catalogue writes do
        // not change, so the answers are already there.
        CHECK(item("test:movie:holed").find("availability")->asString() == "partial");
        CHECK(item("test:movie:holed").find("availability_members")->isNull());
        CHECK(item("test:movie:both").find("availability")->asString() == "complete");
        CHECK(item("test:show:availability").find("availability")->asString() == "unknown");

        // A client that PUTs the whole item back echoes both fields: they
        // are not part of the item, and are answered afresh.
        {
            auto echoed = item("test:movie:holed");
            echoed.asObject()["availability"] = "complete";
            echoed.asObject()["availability_members"] = Json::Object{{"total", 9}};
            echoed.asObject()["title"] = "Holed again";
            const auto body = echoed.dump();
            HttpRequest put;
            put.method = "PUT";
            put.path = "/api/v1/catalogue/items/test:movie:holed";
            put.body.assign(body.begin(), body.end());
            const auto response = first.catalogue_api().handle(put);
            REQUIRE(response.status == 200);
            const auto saved = json(response);
            CHECK(saved.find("title")->asString() == "Holed again");
            CHECK(saved.find("availability")->asString() == "partial");
            CHECK(saved.find("availability_members")->isNull());
        }

        HttpRequest list;
        list.method = "GET";
        list.path = "/api/v1/catalogue/items";
        const auto listed = json(first.catalogue_api().handle(list));
        size_t seen = 0;
        for (const auto& entry : listed.find("items")->asArray()) {
            CHECK(entry.find("availability") != nullptr);
            ++seen;
        }
        CHECK(seen == 3);
    }
    CHECK(get("/api/v1/files/nowhere").status == 404);
    CHECK(get("/api/v1/files/dir", "macha:00").status == 400);
    CHECK(get("/api/v1/files", "not-a-hash").status == 400);
    CHECK(json(get("/api/v1/files", "macha:" + std::string(64, '0'))).find("files")->asArray().empty());
    {
        HttpRequest post;
        post.method = "POST";
        post.path = "/api/v1/files/dir";
        CHECK(first.files_api().handle(post).status == 405);
    }

    second.stop();
    first.stop();
}
