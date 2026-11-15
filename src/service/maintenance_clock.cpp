// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/maintenance_clock.hpp"

namespace macha {
namespace {

int64_t system_wall_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Clock::time_point SystemMaintenanceClock::now() const {
    return Clock::now();
}

int64_t SystemMaintenanceClock::wall_ns() const {
    return system_wall_ns();
}

void SystemMaintenanceClock::wait_until(NodeEvents& events, std::stop_token stop,
                                        Clock::time_point deadline,
                                        const std::function<bool()>& ready) {
    (void)wait_for_events(events, stop, deadline, ready);
}

ManualMaintenanceClock::ManualMaintenanceClock(std::chrono::milliseconds poll)
    : steady_(Clock::now().time_since_epoch().count()), wall_ns_(system_wall_ns()), poll_(poll) {}

void ManualMaintenanceClock::advance(Clock::duration by) {
    steady_.fetch_add(by.count(), std::memory_order_acq_rel);
    wall_ns_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(by).count(),
                       std::memory_order_acq_rel);
}

Clock::time_point ManualMaintenanceClock::now() const {
    return Clock::time_point(Clock::duration(steady_.load(std::memory_order_acquire)));
}

int64_t ManualMaintenanceClock::wall_ns() const {
    return wall_ns_.load(std::memory_order_acquire);
}

void ManualMaintenanceClock::wait_until(NodeEvents& events, std::stop_token stop,
                                        Clock::time_point deadline,
                                        const std::function<bool()>& ready) {
    while (!stop.stop_requested() && now() < deadline) {
        const auto seen = events.total();
        if (ready())
            return;
        (void)events.wait_for_change(seen, poll_, stop);
    }
}

} // namespace macha
