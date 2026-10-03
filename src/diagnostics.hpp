// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"
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
// This process's resident set in bytes; 0 where the platform cannot say.
uint64_t process_resident_bytes();

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

// DiagnosticLock over a std::mutex not yet converted to the annotated types.
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

// DiagnosticLock over an annotated mutex, checked like Lock.
class MACHA_SCOPED_CAPABILITY TimedLock {
    DiagnosticLock lock_;

  public:
    TimedLock(Mutex& mutex, std::string_view name,
              std::chrono::milliseconds threshold = std::chrono::milliseconds(10))
        MACHA_ACQUIRE(mutex)
        : lock_(mutex.native(), name, threshold) {}
    TimedLock(IoMutex& mutex, std::string_view name,
              std::chrono::milliseconds threshold = std::chrono::milliseconds(10))
        MACHA_ACQUIRE(mutex)
        : lock_(mutex.native(), name, threshold) {}
    ~TimedLock() MACHA_RELEASE() {}
    TimedLock(const TimedLock&) = delete;
    TimedLock& operator=(const TimedLock&) = delete;
};

int64_t elapsed_ms(Clock::time_point started) noexcept;

} // namespace macha
