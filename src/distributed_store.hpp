// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cluster.hpp"
#include <atomic>
#include <set>

namespace macha {
class DistributedStore {
    NodeRuntime& n_;
    size_t repair_offset_{};
    size_t scrub_offset_{};
    size_t pull_offset_{};
    std::atomic_uint64_t foreground_bytes_{};
    std::atomic_int64_t last_foreground_ms_{};
    std::atomic<double> network_bps_{};

    std::vector<NodeInfo> ranked(const ObjectId&) const;
    std::vector<NodeInfo> owners(const ObjectId&) const;
    bool put_on(const NodeInfo&, const ObjectId&, std::span<const uint8_t>, bool foreground);
    std::optional<Bytes> get_from(const NodeInfo&, const ObjectId&, bool foreground);
    std::optional<Bytes> get_remote(const ObjectId&, size_t stripe, bool foreground,
                                    bool opportunistic_persist);
    void note_foreground(uint64_t);
    void note_network(uint64_t, Clock::duration);

  public:
    explicit DistributedStore(NodeRuntime& n) : n_(n) {}
    ObjectId put(std::span<const uint8_t>);
    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<Bytes> get(const ObjectId&, size_t stripe = 0, bool foreground = true);
    bool has_on(const NodeInfo&, const ObjectId&);
    bool should_own(const ObjectId&) const;
    void foreground_activity(uint64_t bytes) { note_foreground(bytes); }

    // Converges remote placement and proactively pulls live objects for which
    // this node has become an owner. The limit is bytes, not block count; zero
    // means unlimited. Returns bytes transferred across the network.
    uint64_t repair_once(uint64_t byte_budget = 0, const std::set<ObjectId>* live = nullptr);
    uint64_t scrub_once(uint64_t byte_budget = 0);

    uint64_t take_foreground_bytes() {
        return foreground_bytes_.exchange(0);
    }
    std::chrono::milliseconds foreground_idle_for() const;
    double estimated_network_bps() const {
        return network_bps_.load();
    }
};
} // namespace macha
