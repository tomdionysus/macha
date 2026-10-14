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

MACHA_FAST_TEST("availability", test_the_path_table_follows_a_change_to_the_tree) {
    MemoryNamespaceNodeStore store;
    uint64_t seed = 99;
    const auto random = [&](uint64_t below) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed >> 33) % below;
    };
    const auto directory = [] {
        FsEntry entry;
        entry.type = EntryType::directory;
        return entry;
    };
    const auto place = [&] {
        return "/d" + std::to_string(random(4)) + "/s" + std::to_string(random(3));
    };
    std::map<std::string, FsEntry> entries;
    uint64_t content = 0;
    // A file, and sometimes the directories above it as entries of their own.
    const auto add_file = [&] {
        const auto in = place();
        if (random(3)) {
            entries["/"] = directory();
            entries[in.substr(0, 3)] = directory();
            entries[in] = directory();
        }
        // A file is never above another.
        for (const auto& above : {in.substr(0, 3), in})
            if (const auto found = entries.find(above);
                found != entries.end() && found->second.type == EntryType::file)
                found->second = directory();
        // One in five shares another file's content.
        const auto first = random(5) ? ++content * 100 : (1 + random(content + 1)) * 100;
        entries[in + "/f" + std::to_string(random(40))] = file_of(first, 1 + random(8));
    };
    for (int i = 0; i < 40; ++i)
        add_file();

    const auto table_of = [&](const ObjectId& root, const AvailabilitySurvey& survey,
                              const std::set<ObjectId>& held) {
        MetadataSnapshot head;
        head.namespace_root = root;
        AvailabilitySnapshot table;
        table.survey = survey;
        fill_path_table(table, head, store, holds(held));
        return table;
    };
    const auto pairs = [](const AvailabilitySnapshot& table) {
        return std::set<std::pair<std::string, std::string>>(table.by_hash.begin(),
                                                             table.by_hash.end());
    };

    auto root = build_namespace_tree(entries, store, small_limits());
    for (int round = 0; round < 60; ++round) {
        auto next = entries;
        for (uint64_t changes = 1 + random(4); changes; --changes) {
            const auto kind = random(5);
            auto victim = next.begin();
            if (!next.empty())
                std::advance(victim, random(next.size()));
            if (kind < 2 || next.empty()) {
                std::swap(entries, next);
                add_file();
                std::swap(entries, next);
            } else if (victim->second.type == EntryType::file) {
                if (kind == 2)
                    victim->second = file_of(++content * 100, 1 + random(8));
                else
                    next.erase(victim);
            } else {
                // A directory and everything beneath it; once in a while a
                // file takes its place.
                const auto path = victim->first;
                const auto prefix = path == "/" ? path : path + "/";
                std::erase_if(next, [&](const auto& item) {
                    return item.first == path || item.first.starts_with(prefix);
                });
                if (kind == 4 && path != "/")
                    next[path] = file_of(++content * 100, 2);
            }
        }
        const auto next_root = build_namespace_tree(next, store, small_limits());

        // Every node holds what it held: the survey lists stand for both.
        auto all = references(entries);
        const auto more = references(next);
        all.insert(all.end(), more.begin(), more.end());
        const auto held = subset(all, 40, 3);
        AvailabilitySurvey survey;
        for (const auto& id : subset(all, 30, 11))
            if (!held.contains(id))
                survey.unavailable.push_back(id);

        const auto before = table_of(root, survey, held);
        const auto expected = table_of(next_root, survey, held);
        MetadataSnapshot head;
        head.namespace_root = next_root;
        AvailabilitySnapshot updated;
        updated.survey = survey;
        update_path_table(updated, before, diff_namespace_trees(root, next_root, store), head,
                          store, holds(held));
        CHECK(updated.paths == expected.paths);
        CHECK(pairs(updated) == pairs(expected));

        entries = std::move(next);
        root = next_root;
    }
}

// --- The survey by trie diff -------------------------------------------------

std::unique_ptr<ObjectTrie> held_trie(const std::filesystem::path& dir,
                                      const std::set<ObjectId>& ids) {
    auto trie = std::make_unique<ObjectTrie>(dir, std::array<uint8_t, 32>{}, ObjectTrie::Options{});
    std::vector<ObjectTrie::Change> changes;
    for (const auto& id : ids)
        changes.push_back({id, Bytes{0, 0, 0, 0, 0, 0, 0, 1}});
    trie->apply(changes);
    return trie;
}

ObjectId numbered_id(uint32_t n) {
    ObjectId id;
    for (size_t i = 0; i < 4; ++i)
        id.bytes[i] = static_cast<uint8_t>(n >> (8 * (3 - i)));
    id.bytes[31] = 1;
    return id;
}

// A question and every kind of answer survive the wire, and a malformed one
// is refused.
MACHA_FAST_TEST("availability", test_trie_questions_and_answers_survive_the_wire) {
    TrieQuestion question;
    question.kind = TrieQuestionKind::children;
    question.at.bytes.fill(7);
    for (uint8_t length = 0; length < 5; ++length) {
        ObjectTrie::Prefix prefix;
        prefix.length = length;
        for (uint8_t i = 0; i < length; ++i)
            prefix.bytes[i] = static_cast<uint8_t>(i * 31 + 1);
        question.prefixes.push_back(prefix);
    }
    CHECK(decode_trie_question(encode_trie_question(question)) == question);

    const ObjectTrie::Summary root{12345, question.at};
    CHECK(decode_trie_root(encode_trie_root(root)) == root);

    std::vector<ObjectTrie::Children> children(3);
    children[1][0] = {5, question.at};
    children[1][255] = {9, Hash256{}};
    children[2][17] = {1, question.at};
    CHECK(decode_trie_children(encode_trie_children(children)) == children);

    std::vector<std::vector<ObjectTrie::Record>> records(2);
    records[1] = {{numbered_id(1), Bytes{1}}, {numbered_id(2), Bytes{}}};
    CHECK(decode_trie_records(encode_trie_records(records)) == records);

    const auto refused = [](auto decode, Bytes bytes) {
        try {
            (void)decode(bytes);
        } catch (const DecodeError&) {
            return true;
        }
        return false;
    };
    auto too_long = encode_trie_question(question);
    too_long[2 + 32 + 4] = 33; // the first prefix's length
    CHECK(refused(decode_trie_question, too_long));
    std::reverse(records[1].begin(), records[1].end());
    CHECK(refused(decode_trie_records, encode_trie_records(records)));
    auto whole = question;
    whole.prefixes = {ObjectTrie::Prefix{{}, 32}};
    CHECK(refused(decode_trie_question, encode_trie_question(whole)));

    // A records question carries more prefixes than a children question.
    TrieQuestion many;
    many.kind = TrieQuestionKind::records;
    many.prefixes.assign(trie_records_max, ObjectTrie::Prefix{{}, 2});
    CHECK(decode_trie_question(encode_trie_question(many)) == many);
    many.kind = TrieQuestionKind::children;
    bool too_many = false;
    try {
        (void)encode_trie_question(many);
    } catch (const std::invalid_argument&) {
        too_many = true;
    }
    CHECK(too_many);
}

MACHA_TEST("availability", test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable) {
    AvailabilitySurvey last;
    last.unavailable = {extent_id(1), extent_id(2), extent_id(3)};
    std::sort(last.unavailable.begin(), last.unavailable.end());
    AvailabilitySurvey next;
    next.unavailable = {extent_id(4)};
    next.unknown = {extent_id(1), extent_id(5)};
    std::sort(next.unknown.begin(), next.unknown.end());
    // This node has since gained the second.
    keep_unavailable(next, last, holds({extent_id(2)}));
    std::vector<ObjectId> unavailable{extent_id(1), extent_id(3), extent_id(4)};
    std::sort(unavailable.begin(), unavailable.end());
    CHECK(next.unavailable == unavailable);
    CHECK(next.unknown == std::vector<ObjectId>{extent_id(5)});
}

// One peer's trie as a source that fails after a few questions.
class FailingSource final : public TrieSource {
  public:
    explicit FailingSource(ObjectTrie::Snapshot trie) : inner_(std::move(trie)) {}
    ObjectTrie::Summary root() override { return inner_.root(); }
    std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override {
        throw std::runtime_error("peer went away");
        return inner_.children(p);
    }
    std::vector<std::vector<ObjectTrie::Record>>
    records(std::span<const ObjectTrie::Prefix> p) override {
        throw std::runtime_error("peer went away");
        return inner_.records(p);
    }

  private:
    SnapshotSource inner_;
};

// From diffs: what no peer holds of what this node lacks is unavailable, and
// each peer lacks what this node holds and it does not, within what is
// referenced, on however many disks either side holds them.
MACHA_FAST_TEST("availability", test_the_survey_by_diff_finds_what_no_node_holds) {
    std::set<ObjectId> referenced;
    for (uint32_t i = 0; i < 3000; ++i)
        referenced.insert(numbered_id(i * 7919));
    const auto pick = [&](auto keep) {
        std::set<ObjectId> out;
        size_t i = 0;
        for (const auto& id : referenced)
            if (keep(i++))
                out.insert(id);
        return out;
    };
    auto mine = pick([](size_t i) { return i % 3 != 0; });
    auto first = pick([](size_t i) { return i % 5 != 0; });
    auto second = pick([](size_t i) { return i % 2 == 0; });
    // Garbage this node holds that nothing refers to.
    const auto garbage = numbered_id(0xfffffff0);
    mine.insert(garbage);

    std::vector<ObjectId> missing_here;
    for (const auto& id : referenced)
        if (!mine.contains(id))
            missing_here.push_back(id);
    std::vector<ObjectId> unavailable;
    for (const auto& id : missing_here)
        if (!first.contains(id) && !second.contains(id))
            unavailable.push_back(id);
    const auto lacking = [&](const std::set<ObjectId>& peer) {
        std::vector<ObjectId> out;
        for (const auto& id : mine)
            if (referenced.contains(id) && !peer.contains(id))
                out.push_back(id);
        return out;
    };

    // This node and the first peer each hold on two disks, some ids on both.
    const auto split = [](const std::set<ObjectId>& ids, size_t part) {
        std::set<ObjectId> out;
        size_t i = 0;
        for (const auto& id : ids)
            if (i++ % 3 != part)
                out.insert(id);
        return out;
    };
    TempDir dir;
    auto here_a = held_trie(dir.path() / "here-a", split(mine, 0));
    auto here_b = held_trie(dir.path() / "here-b", split(mine, 1));
    const auto here = std::make_shared<const TrieUnion>(
        std::vector{here_a->snapshot(), here_b->snapshot()});
    auto one_a = held_trie(dir.path() / "one-a", split(first, 0));
    auto one_b = held_trie(dir.path() / "one-b", split(first, 2));
    auto two = held_trie(dir.path() / "two", second);
    UnionSource one_source(std::make_shared<const TrieUnion>(
        std::vector{one_a->snapshot(), one_b->snapshot()}));
    SnapshotSource two_source(two->snapshot());
    std::vector<TrieSource*> peers{&one_source, &two_source};
    const std::vector<ObjectId> referenced_list(referenced.begin(), referenced.end());
    std::vector<PeerDiff> diffs;
    const auto survey = survey_by_diff(here, missing_here, IdLookup(referenced_list), peers, diffs);
    CHECK(survey.unavailable == unavailable);
    CHECK(survey.unknown.empty());
    CHECK(survey.peers_asked == 2);
    CHECK(survey.peers_failed == 0);
    REQUIRE(diffs.size() == 2);
    CHECK(diffs[0].answered);
    CHECK(diffs[0].lacks == lacking(first));
    CHECK(diffs[1].lacks == lacking(second));
    CHECK(diffs[1].root == two->snapshot().root_hash());

    // Alone, nothing anyone else holds: everything missing is unavailable.
    std::vector<TrieSource*> none;
    CHECK(survey_by_diff(here, missing_here, IdLookup(referenced_list), none, diffs).unavailable ==
          missing_here);

    // A peer that fails mid-diff: what the other does not hold might be its,
    // so it is unknown, and the other's lacks still stand.
    FailingSource failing(two->snapshot());
    std::vector<TrieSource*> broken{&one_source, &failing};
    const auto partial =
        survey_by_diff(here, missing_here, IdLookup(referenced_list), broken, diffs);
    std::vector<ObjectId> not_first;
    for (const auto& id : missing_here)
        if (!first.contains(id))
            not_first.push_back(id);
    CHECK(partial.peers_failed == 1);
    CHECK(partial.unavailable.empty());
    CHECK(partial.unknown == not_first);
    CHECK(diffs[0].answered);
    CHECK(diffs[0].lacks == lacking(first));
    CHECK(!diffs[1].answered);
}

// What this node lacks: the referenced set, a trie and a list beside it, less
// what any of its disks holds, for none to three disks.
MACHA_FAST_TEST("availability", test_what_this_node_lacks_is_the_referenced_set_less_every_disk) {
    std::set<ObjectId> viewed;
    for (uint32_t i = 0; i < 2000; ++i)
        viewed.insert(numbered_id(i * 104729));
    // Beside the view: some ids of its own, one the view has as well.
    std::vector<ObjectId> others{numbered_id(7), numbered_id(11), *viewed.begin()};
    std::set<ObjectId> referenced(viewed);
    referenced.insert(others.begin(), others.end());
    TempDir dir;
    auto view = held_trie(dir.path() / "view", viewed);
    const ReferencedSets sets(ReferencedSets::Views{view->snapshot(), std::nullopt}, others, {});
    REQUIRE(sets.size(RetentionClass::data) == referenced.size());

    std::vector<std::unique_ptr<ObjectTrie>> disks;
    std::vector<std::set<ObjectId>> held;
    for (size_t count = 0; count <= 3; ++count) {
        std::vector<ObjectTrie::Snapshot> parts;
        for (const auto& disk : disks)
            parts.push_back(disk->snapshot());
        std::vector<ObjectId> expected;
        for (const auto& id : referenced)
            if (std::none_of(held.begin(), held.end(),
                             [&](const std::set<ObjectId>& on) { return on.contains(id); }))
                expected.push_back(id);
        CHECK(sets.not_held(RetentionClass::data, parts) == expected);
        // The next disk holds every few referenced ids, one of the others,
        // and garbage nothing refers to.
        std::set<ObjectId> next;
        size_t i = 0;
        for (const auto& id : referenced)
            if (i++ % (count + 2) == count)
                next.insert(id);
        next.insert(others[count % 2]);
        next.insert(numbered_id(0xfffffff0 - static_cast<uint32_t>(count)));
        disks.push_back(held_trie(dir.path() / ("disk" + std::to_string(count)), next));
        held.push_back(next);
    }
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

    const auto table = item_availability(CatalogueView::of(catalogue), &survey);
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
    const auto blind = item_availability(CatalogueView::of(catalogue), nullptr);
    for (const auto& [id, entry] : blind)
        CHECK(entry.status == Availability::unknown);
    CHECK(blind.at("show").members == 3);

    // A parent cycle ends instead of looping.
    catalogue.items["show"].parent_id = "s1";
    CHECK(item_availability(CatalogueView::of(catalogue), &survey).size() == catalogue.items.size());
}

MACHA_FAST_TEST("availability", test_the_item_table_is_rebuilt_only_when_a_snapshot_changes) {
    CatalogueSnapshot merged;
    CatalogueItem movie;
    movie.id = "m";
    movie.media_ids = {"macha:x"};
    merged.items["m"] = movie;
    const auto catalogue = std::make_shared<const CatalogueView>(CatalogueView::of(merged));
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
MACHA_TEST("availability", test_what_only_a_host_out_of_reach_holds_is_unavailable) {
    // The second node alone holds a file. With the second out of reach, what
    // it holds is not reachable; once it is back, it is again.
    TestCluster cluster(ConfigProfile::isolated);
    const auto first_port = free_port();
    const auto second_port = free_port();
    auto first_config =
        cluster.node_config("blind-first", first_port, {{"127.0.0.1", second_port}});
    auto second_config =
        cluster.node_config("blind-second", second_port, {{"127.0.0.1", first_port}});
    for (auto* config : {&first_config, &second_config}) {
        config->replication = 2;
        config->write_copies = 2;
        config->catalogue.scanner.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
    }
    Service first(first_config, cluster.keys(), test_durability_window);
    auto second = std::make_unique<Service>(second_config, cluster.keys(), test_durability_window);
    first.start();
    second->start();
    (void)first.filesystem();
    (void)second->filesystem();
    REQUIRE(wait_until(
        [&] {
            return first.node().membership().active().size() == 2 &&
                   second->node().membership().active().size() == 2;
        },
        10s));

    const auto extent = first_config.extent_size;
    REQUIRE(retry_while_not_ready(
        [&] { write_file(first.filesystem(), "/kept.bin", pattern(extent * 2, 1)); }));
    const auto ids = [&] {
        std::vector<ObjectId> out;
        for (const auto& item : first.filesystem().getattr("/kept.bin").extents)
            out.push_back(item.id);
        return out;
    }();
    REQUIRE(wait_until(
        [&] {
            return std::all_of(ids.begin(), ids.end(), [&](const ObjectId& id) {
                return second->local_state().data().has(id);
            });
        },
        20s));
    // Both surveyed the file whole; then the second goes out of reach, and
    // only afterwards does the first lose its own copy, so it cannot fetch it
    // back: only the absent second holds it.
    REQUIRE(wait_until(
        [&] {
            const auto snapshot = first.availability().snapshot();
            return snapshot && snapshot->paths.contains("/kept.bin") &&
                   snapshot->survey.peers_asked == 1 && snapshot->survey.unavailable.empty();
        },
        20s));
    second->stop();
    second.reset();
    REQUIRE(wait_until([&] { return first.node().membership().active().size() == 1; }, 20s));
    for (const auto& id : ids)
        REQUIRE(first.local_state().data().remove(id));
    REQUIRE(retry_while_not_ready(
        [&] { write_file(first.filesystem(), "/after.bin", pattern(extent, 3)); }));
    const auto kept = [&] {
        const auto snapshot = first.availability().snapshot();
        if (!snapshot || !snapshot->paths.contains("/kept.bin"))
            return Availability::unknown;
        return availability_of(&snapshot->paths.at("/kept.bin"));
    };
    CHECK(wait_until([&] { return kept() == Availability::unavailable; }, 20s));

    second = std::make_unique<Service>(second_config, cluster.keys(), test_durability_window);
    second->start();
    CHECK(wait_until([&] { return kept() == Availability::complete; }, 30s));
    second->stop();
    first.stop();
}

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
        config->write_copies = 2;
        config->catalogue.scanner.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
    }

    Service first(first_config, cluster.keys(), test_durability_window);
    Service second(second_config, cluster.keys(), test_durability_window);
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
                // And a survey the peer could answer in full: while the peer
                // catches up with the new namespace its answer leaves extents
                // unknown, and the table shows their last known counts.
                const auto holed = path_of(*service, "/dir/holed.bin");
                const auto later = path_of(*service, "/later.bin");
                const auto survey = service->availability().snapshot();
                return holed && holed->extents_unavailable == 1 && later && later->extents == 1 &&
                       survey->survey.unknown.empty() && survey->survey.unavailable.size() == 1;
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

// A disk that comes back is seen without anything else changing: the survey
// follows the holdings' identity, not the namespace or the writes.
MACHA_TEST("availability", test_a_disk_coming_back_is_surveyed_without_a_namespace_change) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("availability-disk");
    config.replication = 1;
    config.write_copies = 1;
    config.metadata_write_copies = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    (void)service.filesystem();

    const auto extent = config.extent_size;
    REQUIRE(retry_while_not_ready(
        [&] { write_file(service.filesystem(), "/held.bin", pattern(extent * 2, 1)); }));
    const auto local = [&]() -> std::optional<uint64_t> {
        const auto snapshot = service.availability().snapshot();
        if (!snapshot)
            return {};
        const auto found = snapshot->paths.find("/held.bin");
        if (found == snapshot->paths.end())
            return {};
        return found->second.extents_local;
    };
    REQUIRE(wait_until([&] { return local() == 2u; }, 20s));

    const auto backend = config.storage_backends.at(0).path;
    auto away = backend;
    away += ".away";
    std::filesystem::rename(backend, away);
    service.local_state().data().refresh();
    REQUIRE(service.local_state().data().online_backends() == 0);
    REQUIRE(wait_until([&] { return local() == 0u; }, 20s));

    const auto head = service.metadata_manager().snapshot_view().hash;
    std::filesystem::rename(away, backend);
    service.local_state().data().refresh();
    REQUIRE(service.local_state().data().online_backends() == 1);
    CHECK(wait_until([&] { return local() == 2u; }, 20s));
    CHECK(service.metadata_manager().snapshot_view().hash == head);
    service.stop();
}
