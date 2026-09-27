// SPDX-License-Identifier: GPL-3.0-or-later
#include "supervised.hpp"
#include "log.hpp"
#include "types.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <optional>

namespace macha {
namespace {

constexpr auto restart_initial = std::chrono::seconds(1);
constexpr auto restart_ceiling = std::chrono::seconds(60);

struct Registry {
    std::mutex mutex;
    std::map<std::string, SupervisedThreadStatus, std::less<>> threads;

    SupervisedThreadStatus& entry_locked(std::string_view name) {
        auto it = threads.find(name);
        if (it == threads.end()) {
            it = threads.emplace(std::string(name), SupervisedThreadStatus{}).first;
            it->second.name = std::string(name);
        }
        return it->second;
    }
};

Registry& registry() {
    // Leaked on purpose: threads may still be ending while static destructors
    // run at process exit.
    static auto* instance = new Registry;
    return *instance;
}

void entered(std::string_view name) noexcept {
    try {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        ++r.entry_locked(name).running;
    } catch (...) {
    }
}

void left(std::string_view name) noexcept {
    try {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        auto& entry = r.entry_locked(name);
        if (entry.running) --entry.running;
    } catch (...) {
    }
}

void set_restarting(std::string_view name, bool waiting) noexcept {
    try {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        auto& entry = r.entry_locked(name);
        if (waiting)
            ++entry.restarting;
        else if (entry.restarting)
            --entry.restarting;
    } catch (...) {
    }
}

struct Fault {
    std::string code;
    std::string message;
};

void record(std::string_view name, const Fault& fault, std::string_view outcome) noexcept {
    try {
        {
            auto& r = registry();
            std::lock_guard lock(r.mutex);
            auto& entry = r.entry_locked(name);
            ++entry.faults;
            entry.last_fault_code = fault.code;
            entry.last_fault = fault.message;
            entry.last_fault_unix_ms = unix_ms();
        }
        Log::error("thread '" + std::string(name) + "' faulted (" + fault.code +
                   "): " + fault.message + "; " + std::string(outcome));
    } catch (...) {
        // Recording itself failed (e.g. allocation failure building the
        // message). Still must not let anything escape a thread entry point.
    }
}

// Runs the body once. Empty when it returned, the fault when it threw.
std::optional<Fault> run_body(std::string_view name, const std::function<void()>& body) noexcept {
    entered(name);
    std::optional<Fault> fault;
    try {
        body();
    } catch (const std::exception& e) {
        try {
            fault = Fault{"exception", e.what()};
        } catch (...) {
            fault.emplace();
        }
    } catch (...) {
        try {
            fault = Fault{"unknown_exception", "a non-standard exception escaped the thread body"};
        } catch (...) {
            fault.emplace();
        }
    }
    left(name);
    return fault;
}

} // namespace

std::chrono::milliseconds supervised_restart_delay(size_t consecutive) noexcept {
    std::chrono::milliseconds delay = restart_initial;
    for (size_t i = 1; i < consecutive && delay < restart_ceiling; ++i) delay *= 2;
    return std::min<std::chrono::milliseconds>(delay, restart_ceiling);
}

void run_supervised_loop(std::string_view name, std::stop_token stop,
                         const std::function<void()>& body) noexcept {
    size_t consecutive = 0;
    while (true) {
        const auto started = std::chrono::steady_clock::now();
        const auto fault = run_body(name, body);
        if (!fault) return;
        if (stop.stop_requested()) {
            record(name, *fault, "stopping, not restarted");
            return;
        }
        // A run that outlived the ceiling was healthy until it faulted; it
        // starts the backoff over rather than inheriting an old streak.
        if (std::chrono::steady_clock::now() - started > restart_ceiling) consecutive = 0;
        const auto delay = supervised_restart_delay(++consecutive);
        record(name, *fault, "restarting in " + std::to_string(delay.count()) + " ms");
        set_restarting(name, true);
        try {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, delay, [] { return false; });
        } catch (...) {
        }
        set_restarting(name, false);
        if (stop.stop_requested()) return;
    }
}

void run_supervised_once(std::string_view name, const std::function<void()>& body) noexcept {
    if (const auto fault = run_body(name, body)) record(name, *fault, "thread ended");
}

void run_supervised_escalating(std::string_view name, const std::function<void()>& body,
                               const std::function<void(std::string)>& escalate) noexcept {
    const auto fault = run_body(name, body);
    if (!fault) return;
    record(name, *fault, "escalated to its subsystem's supervisor");
    try {
        if (escalate) escalate("thread '" + std::string(name) + "' faulted: " + fault->message);
    } catch (...) {
    }
}

std::vector<SupervisedThreadStatus> supervised_thread_statuses() {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    std::vector<SupervisedThreadStatus> out;
    out.reserve(r.threads.size());
    for (const auto& [_, status] : r.threads) out.push_back(status);
    return out;
}

} // namespace macha
