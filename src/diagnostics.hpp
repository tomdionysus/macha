// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace macha {

// Best-effort diagnostics helpers. None of these are correctness dependencies:
// platform failures simply disable the associated measurement.
void set_thread_name(std::string_view) noexcept;
uint64_t thread_cpu_time_ns() noexcept;

class ThreadCpuReporter {
    std::string name_;
    Clock::time_point wall_started_{Clock::now()};
    uint64_t cpu_started_{thread_cpu_time_ns()};
    uint64_t iterations_{};
    std::chrono::milliseconds interval_;
    bool report_idle_{};
    bool debug_high_cpu_{};
    Clock::time_point debug_last_report_{};

  public:
    explicit ThreadCpuReporter(std::string name,
                               std::chrono::milliseconds interval = std::chrono::seconds(5),
                               bool report_idle = false);
    void tick(uint64_t iterations = 1);
};

// Use only around important shared locks. It reports waits/holds above the
// threshold at ALL and otherwise behaves like an ordinary unique_lock.
class DiagnosticLock {
    std::unique_lock<std::mutex> lock_;
    std::string_view name_;
    Clock::time_point acquired_{};
    std::chrono::milliseconds threshold_;
    int64_t wait_ms_{};
    bool enabled_{};

  public:
    DiagnosticLock(std::mutex&, std::string_view,
                   std::chrono::milliseconds threshold = std::chrono::milliseconds(10));
    ~DiagnosticLock() noexcept;
    DiagnosticLock(const DiagnosticLock&) = delete;
    DiagnosticLock& operator=(const DiagnosticLock&) = delete;
};

int64_t elapsed_ms(Clock::time_point started) noexcept;

} // namespace macha
