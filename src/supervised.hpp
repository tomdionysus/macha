// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// Mandatory entry points for every subsystem-owned thread body. An exception
// escaping a thread lambda calls std::terminate(); these guard that boundary,
// and each defines what a fault means for the thread.
//
// Every fault is recorded against `name` (supervised_thread_statuses(), Status
// `threads`) and logged at ERROR. Threads of one pool share a name.

// A service loop: `body` runs until `stop`. If it throws, the body is run
// again after a backoff (1 s doubling to 60 s; back to 1 s after a run that
// lasted longer than the ceiling), until `stop`. A body that returns normally
// has finished and is not restarted. The body must hold no state across a
// restart that its own start does not rebuild.
void run_supervised_loop(std::string_view name, std::stop_token stop,
                         const std::function<void()>& body) noexcept;

// A task that runs once -- a connection, a transcode, a start-up phase, a
// scan. A fault is recorded and the thread ends; whatever owns the task
// decides what the failure means. Running it again would be wrong: the
// connection is gone, the transcode has a client, the phase has a deadline.
void run_supervised_once(std::string_view name, const std::function<void()>& body) noexcept;

// A thread owned by a supervised subsystem (SubsystemSupervisor), which
// rebuilds the whole instance from its durable state rather than rerunning
// one of its threads in place. A fault is recorded, `escalate` is called with
// the reason, and the thread ends.
void run_supervised_escalating(std::string_view name, const std::function<void()>& body,
                               const std::function<void(std::string)>& escalate) noexcept;

// Process-lifetime record per thread name. Names stay listed once seen, so a
// pool whose threads all ended still says how.
struct SupervisedThreadStatus {
    std::string name;
    // Threads of this name currently inside their body.
    size_t running{};
    // Threads of this name waiting out a backoff before their body reruns.
    size_t restarting{};
    uint64_t faults{};
    // Empty until the first fault. Code: exception, unknown_exception.
    std::string last_fault_code;
    std::string last_fault;
    uint64_t last_fault_unix_ms{};
};

std::vector<SupervisedThreadStatus> supervised_thread_statuses();

// The restart delay after `consecutive` faults (1-based): exposed for tests.
std::chrono::milliseconds supervised_restart_delay(size_t consecutive) noexcept;

} // namespace macha
