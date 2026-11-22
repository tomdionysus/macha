// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/catalogue_installer.hpp"

#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <chrono>

namespace macha {

void CatalogueInstaller::start() {
    if (worker_.joinable())
        return;
    worker_ = std::jthread([this](const std::stop_token& stop) {
        run_supervised_loop("catalogue-installer", stop, [this, stop] { loop(stop); });
    });
}

void CatalogueInstaller::request_stop() {
    // The worker's wait ends on its stop token.
    if (worker_.joinable())
        worker_.request_stop();
}

void CatalogueInstaller::stop() {
    request_stop();
    if (worker_.joinable())
        worker_.join();
}

void CatalogueInstaller::loop(const std::stop_token& stop) {
    using Clock = std::chrono::steady_clock;
    // A failed install is tried again on a new head or a membership change at once: either
    // may bring what it lacked. A storage change is raised for every object the node
    // stores, so under an import or repair it waits out a backoff, 1 s doubling to 5 min.
    constexpr auto first_backoff = std::chrono::seconds(1);
    constexpr auto max_backoff = std::chrono::minutes(5);
    bool first = true;
    bool failed = false;
    auto backoff = Clock::duration::zero();
    auto storage_retry_at = Clock::time_point::max();
    uint64_t seen_metadata = 0;
    uint64_t seen_membership = 0;
    uint64_t seen_storage = 0;
    const auto storage_due = [&] {
        return failed && events_.count(node_event::storage) != seen_storage &&
               Clock::now() >= storage_retry_at;
    };
    const auto due = [&] {
        return first || events_.count(node_event::metadata) != seen_metadata ||
               (failed && events_.count(node_event::membership) != seen_membership) ||
               storage_due();
    };
    while (!stop.stop_requested()) {
        if (due()) {
            first = false;
            seen_metadata = events_.count(node_event::metadata);
            seen_membership = events_.count(node_event::membership);
            seen_storage = events_.count(node_event::storage);
            attempts_.fetch_add(1, std::memory_order_relaxed);
            try {
                catalogue_.follow_head();
                failed = false;
                backoff = Clock::duration::zero();
                storage_retry_at = Clock::time_point::max();
            } catch (const std::exception& e) {
                failed = true;
                backoff = backoff == Clock::duration::zero()
                              ? Clock::duration(first_backoff)
                              : std::min<Clock::duration>(backoff * 2, max_backoff);
                storage_retry_at = Clock::now() + backoff;
                Log::debug(std::string("catalogue install deferred: ") + e.what());
            }
            continue;
        }
        // A storage change seen before the backoff ends is taken up when it does.
        const auto deadline = failed && events_.count(node_event::storage) != seen_storage
                                  ? storage_retry_at
                                  : Clock::time_point::max();
        (void)wait_for_events(events_, stop, deadline, due);
    }
}

} // namespace macha
