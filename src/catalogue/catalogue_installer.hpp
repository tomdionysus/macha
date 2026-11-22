// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "cluster/node_events.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace macha {

// Keeps the catalogue view at this node's head: each head change installs
// the head's catalogue, the latest head when several arrive at once. An
// install that fails (a shard no node present can supply, or a control store
// too full to take one) is tried again on the next head or a membership change
// (Dannan's) at once, and on a storage change (the store node's) after a
// backoff of 1 s doubling to 5 min. Readers never wait on it.
class CatalogueInstaller {
    CatalogueManager& catalogue_;
    NodeEvents& events_;
    std::atomic_uint64_t attempts_{};
    std::jthread worker_;

    void loop(const std::stop_token&);

  public:
    CatalogueInstaller(CatalogueManager& catalogue, NodeEvents& events) noexcept
        : catalogue_(catalogue), events_(events) {}
    ~CatalogueInstaller() {
        stop();
    }
    CatalogueInstaller(const CatalogueInstaller&) = delete;
    CatalogueInstaller& operator=(const CatalogueInstaller&) = delete;

    void start();
    void request_stop();
    void stop();
    // Installs tried since start, failed or not.
    uint64_t attempts() const noexcept {
        return attempts_.load(std::memory_order_relaxed);
    }
};

} // namespace macha
