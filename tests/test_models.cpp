// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

using namespace macha;
using namespace macha::test_support;

namespace {

ObjectId model_object(uint32_t value) {
    Bytes bytes(8);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
        bytes[i + 4] = static_cast<uint8_t>(0xa5U ^ bytes[i]);
    }
    return object_id(bytes);
}

NodeId model_node(uint32_t value) {
    const auto hash = sha256(Bytes{static_cast<uint8_t>(value & 0xffU),
                                   static_cast<uint8_t>((value >> 8U) & 0xffU),
                                   static_cast<uint8_t>((value >> 16U) & 0xffU),
                                   static_cast<uint8_t>((value >> 24U) & 0xffU)});
    NodeId out;
    std::copy_n(hash.bytes.begin(), out.bytes.size(), out.bytes.begin());
    return out;
}

void check_snapshot_equal(const MetadataSnapshot& actual, const MetadataSnapshot& expected) {
    CHECK(actual.metadata_voters == expected.metadata_voters);
    CHECK(actual.data_replication == expected.data_replication);
    CHECK(actual.extent_size == expected.extent_size);
    CHECK(actual.mutation_sequences == expected.mutation_sequences);
    CHECK(actual.catalogue_root == expected.catalogue_root);
    CHECK(actual.entries == expected.entries);
    CHECK(actual.garbage == expected.garbage);
    CHECK(actual.node_status == expected.node_status);
    CHECK(actual.identity_resets == expected.identity_resets);
    CHECK(actual.merge_parents == expected.merge_parents);
    CHECK(actual.conflicts == expected.conflicts);
}

MACHA_FAST_TEST("models", test_metadata_delta_state_model) {
    auto state = decode_snapshot(genesis_metadata().payload);
    const auto mutator = model_node(1);
    const auto retire_node = model_node(2);

    // Exercise a deterministic sequence containing simultaneous upserts,
    // erasures, catalogue changes, garbage retirement/reaffirmation and
    // idempotency-clock advancement. Each transition is encoded and decoded
    // before it is applied, so this checks the real wire format as well as the
    // state transition.
    for (uint32_t step = 1; step <= 96; ++step) {
        auto expected = state;
        expected.mutation_sequences[mutator] = step;

        FsEntry directory;
        directory.type = EntryType::directory;
        directory.mode = 0755;
        expected.entries["/model"] = directory;

        const auto path = "/model/file-" + std::to_string(step % 11);
        FsEntry file;
        file.type = EntryType::file;
        file.mode = 0640;
        file.size = 1000 + step;
        file.version = step;
        file.extents.push_back({0, file.size, model_object(1000 + step), false});
        expected.entries[path] = file;

        if (step > 12 && step % 4 == 0)
            expected.entries.erase("/model/file-" + std::to_string((step + 3) % 11));

        if (step % 3 == 0)
            expected.catalogue_root = model_object(2000 + step);
        else if (step % 7 == 0)
            expected.catalogue_root.reset();

        if (step % 5 == 0) {
            GarbageRef retired;
            retired.id = model_object(3000 + step);
            retired.retired_at_ns = 1'000'000 + step;
            retired.retirement_id = retire_node;
            expected.garbage.push_back(retired);
        }
        if (step % 8 == 0 && !expected.garbage.empty())
            expected.garbage.erase(expected.garbage.begin());
        if (step % 9 == 0 && !expected.garbage.empty())
            expected.garbage.back().retired_at_ns += 500;

        const auto delta = metadata_delta(state, expected);
        REQUIRE(delta.has_value());
        const auto encoded = encode_metadata_delta(*delta);
        const auto decoded = decode_metadata_delta(encoded);
        const auto applied = apply_metadata_delta(state, decoded);
        check_snapshot_equal(applied, expected);

        // Full snapshot serialization must agree with the delta path at every
        // generated state, not only at a hand-picked endpoint.
        check_snapshot_equal(decode_snapshot(encode_snapshot(applied)), expected);
        state = std::move(expected);
    }

    auto incompatible = state;
    ++incompatible.data_replication;
    CHECK(!metadata_delta(state, incompatible).has_value());

    incompatible = state;
    incompatible.mutation_sequences[mutator] = state.mutation_sequences.at(mutator) - 1;
    CHECK(!metadata_delta(state, incompatible).has_value());
}

MACHA_FAST_TEST("models", test_hydration_scheduler_state_model) {
    std::vector<ObjectId> foreground;
    std::vector<ObjectId> background;
    for (uint32_t i = 0; i < 24; ++i) foreground.push_back(model_object(4000 + i));
    for (uint32_t i = 0; i < 6; ++i) background.push_back(model_object(5000 + i));

    HydrationHint high{"foreground", foreground, 1000, "current", FrameType::read_ahead};
    HydrationHint low{"background", background, 100, "next", FrameType::speculative};
    // Reinforcement is part of the scheduler contract: overlapping producers
    // for the same ordered run add weight without duplicating the run.
    HydrationHint reinforcement{"foreground", foreground, 500, "prediction",
                                FrameType::speculative};

    std::set<ObjectId> present;
    HydrationScheduler scheduler;
    std::size_t foreground_next = 0;
    std::size_t background_next = 0;
    std::optional<std::size_t> first_background_turn;

    for (std::size_t turn = 0; turn < foreground.size() + background.size(); ++turn) {
        auto request = scheduler.next(
            {high, low, reinforcement},
            [&](const ObjectId& id) { return present.contains(id); });
        REQUIRE(request.has_value());

        if (request->run_id == "foreground") {
            REQUIRE(foreground_next < foreground.size());
            CHECK(request->sequence_index == foreground_next);
            CHECK(request->object == foreground[foreground_next]);
            CHECK(request->priority == 1500);
            CHECK(request->frame_type == FrameType::read_ahead);
            ++foreground_next;
        } else {
            REQUIRE(request->run_id == "background");
            REQUIRE(background_next < background.size());
            CHECK(request->sequence_index == background_next);
            CHECK(request->object == background[background_next]);
            CHECK(request->priority == 100);
            if (!first_background_turn) first_background_turn = turn;
            ++background_next;
        }
        CHECK(!present.contains(request->object));
        present.insert(request->object);
    }

    CHECK(foreground_next == foreground.size());
    CHECK(background_next == background.size());
    REQUIRE(first_background_turn.has_value());
    // A 15:1 reinforced priority ratio is allowed to bias service heavily, but
    // weighted fair scheduling must service the lower-priority run before the
    // high-priority run is exhausted.
    CHECK(*first_background_turn < foreground.size());
    CHECK(!scheduler.next({high, low, reinforcement},
                          [&](const ObjectId& id) { return present.contains(id); })
               .has_value());

    // An unavailable prefix may stall its own ordered run, but it may not stall
    // an independent run and the scheduler must never jump over that prefix.
    scheduler.reset();
    present.clear();
    const auto blocked_object = foreground.front();
    auto blocked = [&](const ObjectId& id) { return id == blocked_object; };
    auto other = scheduler.next({high, low},
                                [&](const ObjectId& id) { return present.contains(id); }, blocked);
    REQUIRE(other.has_value());
    CHECK(other->run_id == "background");
    CHECK(other->sequence_index == 0);
    present.insert(other->object);

    CHECK(!scheduler.next({high}, [&](const ObjectId& id) { return present.contains(id); }, blocked)
               .has_value());
}

MACHA_FAST_TEST("models", test_capacity_placement_invariants_model) {
    std::vector<NodeInfo> nodes;
    for (uint32_t i = 0; i < 6; ++i) {
        NodeInfo node;
        node.id = model_node(100 + i);
        node.host = "node-" + std::to_string(i);
        node.port = static_cast<uint16_t>(7000 + i);
        node.failure_domain = "site-" + std::to_string(i);
        node.capacity = (i + 1) * 1024 * 1024;
        nodes.push_back(node);
    }

    for (uint32_t key_number = 0; key_number < 256; ++key_number) {
        const auto key = model_object(6000 + key_number);
        for (std::size_t replicas = 1; replicas <= nodes.size(); ++replicas) {
            const auto order = capacity_placement_nodes(key.bytes, nodes, replicas);
            const auto again = capacity_placement_nodes(key.bytes, nodes, replicas);
            REQUIRE(order.size() == nodes.size());
            REQUIRE(again.size() == order.size());
            for (std::size_t i = 0; i < order.size(); ++i)
                CHECK(order[i].id == again[i].id);

            std::set<NodeId> unique;
            std::set<std::string> preferred_domains;
            for (std::size_t i = 0; i < order.size(); ++i) {
                CHECK(unique.insert(order[i].id).second);
                if (i < replicas) preferred_domains.insert(order[i].failure_domain);
            }
            CHECK(preferred_domains.size() == replicas);
        }
    }

    // R=1 uses monotonic weighted rendezvous: adding a node may steal an
    // object's shard, but it must never move the shard between two old nodes.
    const auto before = std::vector<NodeInfo>(nodes.begin(), nodes.begin() + 5);
    const auto added = nodes.back().id;
    std::size_t stolen = 0;
    for (uint32_t key_number = 0; key_number < 1024; ++key_number) {
        const auto key = model_object(7000 + key_number);
        const auto old_owner = capacity_placement_nodes(key.bytes, before, 1).front().id;
        const auto new_owner = capacity_placement_nodes(key.bytes, nodes, 1).front().id;
        if (old_owner != new_owner) {
            CHECK(new_owner == added);
            ++stolen;
        }
    }
    CHECK(stolen > 0);
}

} // namespace
