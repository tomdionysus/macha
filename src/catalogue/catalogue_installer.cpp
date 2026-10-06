// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/catalogue_installer.hpp"

#include "log.hpp"
#include "supervised.hpp"

namespace macha {

void CatalogueInstaller::start() {
    if (worker_.joinable())
        return;
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("catalogue-installer", stop, [this, stop] { loop(stop); });
    });
}

void CatalogueInstaller::request_stop() {
    if (!worker_.joinable())
        return;
    worker_.request_stop();
    {
        Lock lock(events_.wait_mutex);
    }
    events_.wait_cv.notify_all();
}

void CatalogueInstaller::stop() {
    request_stop();
    if (worker_.joinable())
        worker_.join();
}

void CatalogueInstaller::loop(std::stop_token stop) {
    const auto retry_events = [this] {
        return events_.count(NodeEvent::topology) + events_.count(NodeEvent::storage);
    };
    // The first pass installs whatever the head holds at start.
    bool first = true;
    bool failed = false;
    uint64_t seen_metadata = 0;
    uint64_t seen_retry = 0;
    const auto due = [&] {
        return first || events_.count(NodeEvent::metadata) != seen_metadata ||
               (failed && retry_events() != seen_retry);
    };
    while (!stop.stop_requested()) {
        if (due()) {
            first = false;
            seen_metadata = events_.count(NodeEvent::metadata);
            seen_retry = retry_events();
            try {
                catalogue_.follow_head();
                failed = false;
            } catch (const std::exception& e) {
                failed = true;
                Log::debug(std::string("catalogue install deferred: ") + e.what());
            }
            continue;
        }
        Lock lock(events_.wait_mutex);
        events_.wait_cv.wait(lock.native(), stop, due);
    }
}

} // namespace macha
