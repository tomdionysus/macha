// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "cluster/node_events.hpp"

#include <thread>

namespace macha {

// Keeps the catalogue view at this node's head: each head change installs
// the head's catalogue, the latest head when several arrive at once. An
// install that fails (a shard no node present can supply) is tried again on
// the next head, membership or storage change. Readers never wait on it.
class CatalogueInstaller {
    CatalogueManager& catalogue_;
    NodeEvents& events_;
    std::jthread worker_;

    void loop(std::stop_token);

  public:
    CatalogueInstaller(CatalogueManager& catalogue, NodeEvents& events) noexcept
        : catalogue_(catalogue), events_(events) {}
    ~CatalogueInstaller() { stop(); }
    CatalogueInstaller(const CatalogueInstaller&) = delete;
    CatalogueInstaller& operator=(const CatalogueInstaller&) = delete;

    void start();
    void request_stop();
    void stop();
};

} // namespace macha
