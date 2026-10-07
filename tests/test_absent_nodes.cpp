// SPDX-License-Identifier: GPL-3.0-or-later
//
// What lets a node carry on when another never returns: deletion waits on
// this node's own clock, and a node unheard of for the horizon is forgotten.
#include "cluster/membership.hpp"
#include "codec.hpp"
#include "storage/unreferenced_since.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include <chrono>
#include <fstream>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

ObjectId id(uint8_t n) {
    ObjectId out{};
    out.bytes[0] = n;
    return out;
}

// The wait runs from this node's first sighting, whatever the time now is
// when it is asked again.
MACHA_FAST_TEST("absent_nodes", test_an_object_matures_a_grace_after_its_first_sighting) {
    TempDir dir;
    UnreferencedSince seen(dir.path() / "unreferenced.bin");
    CHECK(!seen.matured(id(1), 1'000, 500ms));
    CHECK(!seen.matured(id(1), 1'499, 500ms));
    CHECK(seen.matured(id(1), 1'500, 500ms));
    // A second object sighted later waits its own grace.
    CHECK(!seen.matured(id(2), 1'500, 500ms));
    CHECK(seen.matured(id(2), 2'000, 500ms));
    // No grace: gone at first sight.
    CHECK(seen.matured(id(3), 2'000, 0ms));
    CHECK(seen.size() == 3);
}

MACHA_FAST_TEST("absent_nodes", test_an_object_referenced_again_starts_its_wait_afresh) {
    TempDir dir;
    UnreferencedSince seen(dir.path() / "unreferenced.bin");
    CHECK(!seen.matured(id(1), 1'000, 500ms));
    seen.forget(id(1));
    CHECK(seen.size() == 0);
    CHECK(!seen.matured(id(1), 1'600, 500ms));
    CHECK(seen.matured(id(1), 2'100, 500ms));
}

MACHA_FAST_TEST("absent_nodes", test_a_clock_set_back_restarts_the_wait) {
    TempDir dir;
    UnreferencedSince seen(dir.path() / "unreferenced.bin");
    CHECK(!seen.matured(id(1), 10'000, 500ms));
    CHECK(!seen.matured(id(1), 4'000, 500ms));
    CHECK(!seen.matured(id(1), 4'499, 500ms));
    CHECK(seen.matured(id(1), 4'500, 500ms));
}

// A completed sweep drops the sightings it did not renew: those objects are
// no longer held.
MACHA_FAST_TEST("absent_nodes", test_a_completed_sweep_drops_sightings_of_objects_no_longer_held) {
    TempDir dir;
    UnreferencedSince seen(dir.path() / "unreferenced.bin");
    (void)seen.matured(id(1), 1'000, 500ms);
    (void)seen.matured(id(2), 1'000, 500ms);
    seen.pass_complete();
    CHECK(seen.size() == 2);
    (void)seen.matured(id(2), 1'100, 500ms);
    seen.pass_complete();
    CHECK(seen.size() == 1);
    // The survivor kept its first sighting.
    CHECK(seen.matured(id(2), 1'500, 500ms));
}

// A restart keeps the sightings, so a node restarted more often than the
// grace still deletes; an unreadable table only starts the waits again.
MACHA_FAST_TEST("absent_nodes", test_sightings_survive_a_restart) {
    TempDir dir;
    const auto path = dir.path() / "retention" / "unreferenced.bin";
    {
        UnreferencedSince seen(path);
        (void)seen.matured(id(1), 1'000, 500ms);
        (void)seen.matured(id(2), 1'200, 500ms);
        seen.save();
        // Nothing changed: nothing is written.
        std::filesystem::remove(path);
        seen.save();
        CHECK(!std::filesystem::exists(path));
        seen.forget(id(2));
        seen.save();
        CHECK(std::filesystem::exists(path));
    }
    {
        UnreferencedSince seen(path);
        CHECK(seen.size() == 1);
        CHECK(seen.matured(id(1), 1'500, 500ms));
        // Loaded sightings count as renewed for the first sweep after it.
        seen.pass_complete();
        CHECK(seen.size() == 1);
    }
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "damaged";
    UnreferencedSince seen(path);
    CHECK(seen.size() == 0);
}

MACHA_FAST_TEST("absent_nodes", test_the_sightings_codec_round_trips_and_refuses_damage) {
    const std::map<ObjectId, uint64_t> sightings{{id(1), 7}, {id(9), 1'234'567}};
    const auto bytes = encode_unreferenced_since(sightings);
    CHECK(decode_unreferenced_since(bytes) == sightings);
    CHECK(decode_unreferenced_since(encode_unreferenced_since({})).empty());
    const auto refused = [](Bytes damaged) {
        try {
            (void)decode_unreferenced_since(damaged);
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
    auto schema = bytes;
    schema[0] ^= 0xff;
    CHECK(refused(schema));
    // The same object twice.
    Writer twice;
    twice.u32(1);
    twice.u64(2);
    for (int i = 0; i < 2; ++i) {
        twice.fixed(id(1).bytes);
        twice.u64(7);
    }
    CHECK(refused(twice.take()));
}

NodeInfo node_at(uint16_t port) {
    NodeInfo node;
    node.id = random_node_id();
    node.host = "127.0.0.1";
    node.port = port;
    node.seen_unix_ms = unix_ms();
    return node;
}

// A node unheard of for the horizon is forgotten, its stale gossip does not
// teach it back, and its own connection does.
MACHA_FAST_TEST("absent_nodes", test_a_node_unheard_of_for_the_horizon_is_forgotten) {
    TempDir dir;
    const auto roster = dir.path() / "known-nodes.bin";
    const auto self = node_at(57401);
    auto peer = node_at(57402);
    {
        // The horizon is never shorter than twice dead_after: 200 ms here.
        // Each check sits at least 70 ms from the edge it tests, so a loaded
        // machine's late wakeup does not cross it.
        Membership membership(self, 100ms, roster, 0ms);
        membership.observe(peer, true);
        CHECK(membership.directly_reachable(peer.id));
        CHECK(membership.directly_reachable(self.id));
        CHECK(membership.forget_unseen() == 0);
        std::this_thread::sleep_for(130ms);
        CHECK(!membership.directly_reachable(peer.id));
        CHECK(membership.forget_unseen() == 0);
        REQUIRE(membership.all().size() == 2);
        std::this_thread::sleep_for(140ms);
        CHECK(membership.forget_unseen() == 1);
        CHECK(membership.all().size() == 1);
        CHECK(membership.all_known_reachable());

        membership.observe(peer, false);
        CHECK(membership.all().size() == 1);
        membership.observe(peer, true);
        CHECK(membership.all().size() == 2);
        std::this_thread::sleep_for(280ms);
        CHECK(membership.forget_unseen() == 1);
    }
    // The roster on disk forgot it too.
    Membership recovered(self, 100ms, roster, 0ms);
    CHECK(recovered.all().size() == 1);
}

// A node still gossiping is not forgotten, a restart does not restart the
// wait from nothing, and no horizon means nobody is forgotten.
MACHA_FAST_TEST("absent_nodes", test_forgetting_counts_from_when_the_node_was_last_heard_of) {
    TempDir dir;
    const auto roster = dir.path() / "known-nodes.bin";
    const auto self = node_at(57401);
    auto peer = node_at(57402);
    {
        Membership membership(self, 40ms, roster, 0ms);
        membership.observe(peer, true);
        for (int i = 0; i < 4; ++i) {
            std::this_thread::sleep_for(40ms);
            peer.seen_unix_ms = unix_ms();
            membership.observe(peer, false);
            CHECK(membership.forget_unseen() == 0);
        }
        CHECK(!membership.directly_reachable(peer.id));
    }
    {
        Membership recovered(self, 40ms, roster, 0ms);
        REQUIRE(recovered.all().size() == 2);
        std::this_thread::sleep_for(100ms);
        CHECK(recovered.forget_unseen() == 1);
    }
    Membership keeps(self, 40ms, {});
    keeps.observe(peer, true);
    std::this_thread::sleep_for(100ms);
    CHECK(keeps.forget_unseen() == 0);
    CHECK(keeps.all().size() == 2);
}

} // namespace
