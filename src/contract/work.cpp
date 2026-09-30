// SPDX-License-Identifier: GPL-3.0-or-later
#include "contract/work.hpp"
#include "contract/thread_safety.hpp"

#include "log.hpp"

#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace macha {

NoIo no_io;

namespace {

std::atomic<WaitGuard::Mode> guard_mode{WaitGuard::Mode::record};
std::atomic<uint64_t> guard_violations{};

// Operations already reported, so a hot path reached from control logs once.
std::mutex reported_mutex;
std::set<std::string, std::less<>> reported;

} // namespace

void WaitGuard::set_mode(Mode mode) noexcept {
    guard_mode.store(mode, std::memory_order_relaxed);
}

WaitGuard::Mode WaitGuard::mode() noexcept {
    return guard_mode.load(std::memory_order_relaxed);
}

uint64_t WaitGuard::violations() noexcept {
    return guard_violations.load(std::memory_order_relaxed);
}

bool WaitGuard::enter(const WorkContext& context, Waits declared, std::string_view operation) {
    if (may_enter(context.frame_type(), declared))
        return true;
    guard_violations.fetch_add(1, std::memory_order_relaxed);
    const std::string message = "control work entered " + std::string(operation) +
                                ", which waits on the DATA device or the network";
    if (mode() == Mode::throw_on_violation)
        throw std::logic_error(message);
    bool first = false;
    {
        std::lock_guard lock(reported_mutex);
        first = reported.emplace(operation).second;
    }
    if (first)
        Log::warn(message);
    return false;
}

} // namespace macha
