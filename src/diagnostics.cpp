// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <time.h>
#endif

namespace macha {
namespace {
constexpr double debug_high_thread_cpu_pct = 10.0;
constexpr double debug_recovered_thread_cpu_pct = 5.0;
constexpr auto debug_high_thread_cpu_repeat = std::chrono::seconds(60);

std::string thread_cpu_message(std::string_view prefix, std::string_view name,
                               int64_t wall_ms, uint64_t cpu_ms, double pct,
                               uint64_t iterations) {
    const auto tenths = static_cast<long long>(std::llround(pct * 10.0));
    return std::string(prefix) + " name=" + std::string(name) +
           " wall_ms=" + std::to_string(wall_ms) +
           " cpu_ms=" + std::to_string(cpu_ms) + " cpu_pct=" +
           std::to_string(tenths / 10) + "." + std::to_string(std::abs(tenths % 10)) +
           " iterations=" + std::to_string(iterations);
}
} // namespace

void set_thread_name(std::string_view name) noexcept {
    try {
        std::string copy(name.substr(0, 15));
#if defined(__APPLE__)
        (void)pthread_setname_np(copy.c_str());
#elif defined(__linux__)
        (void)pthread_setname_np(pthread_self(), copy.c_str());
#else
        (void)copy;
#endif
    } catch (...) {
    }
}

uint64_t thread_cpu_time_ns() noexcept {
#if defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const auto thread = mach_thread_self();
    const auto result = thread_info(thread, THREAD_BASIC_INFO,
                                    reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS)
        return 0;
    const uint64_t user = static_cast<uint64_t>(info.user_time.seconds) * 1'000'000'000ULL +
                          static_cast<uint64_t>(info.user_time.microseconds) * 1'000ULL;
    const uint64_t system = static_cast<uint64_t>(info.system_time.seconds) * 1'000'000'000ULL +
                            static_cast<uint64_t>(info.system_time.microseconds) * 1'000ULL;
    return user + system;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
#else
    return 0;
#endif
}

ThreadCpuReporter::ThreadCpuReporter(std::string name, std::chrono::milliseconds interval,
                                     bool report_idle)
    : name_(std::move(name)), interval_(interval), report_idle_(report_idle) {
    set_thread_name(name_);
}

void ThreadCpuReporter::tick(uint64_t iterations) {
    iterations_ += iterations;
    const auto now = Clock::now();
    if (now - wall_started_ < interval_)
        return;

    const auto cpu_now = thread_cpu_time_ns();
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - wall_started_)
                             .count();
    const auto cpu_ns = cpu_now >= cpu_started_ ? cpu_now - cpu_started_ : 0;
    const double pct = wall_ns > 0 ? (100.0 * static_cast<double>(cpu_ns) /
                                      static_cast<double>(wall_ns))
                                   : 0.0;

    const auto wall_ms = wall_ns / 1'000'000;
    const auto cpu_ms = cpu_ns / 1'000'000;
    if (Log::enabled(LogLevel::all) && (report_idle_ || pct >= 1.0 || iterations_ > 0)) {
        Log::trace(thread_cpu_message("DIAG thread", name_, wall_ms, cpu_ms, pct, iterations_));
    }

    // ALL already carries the complete periodic stream. DEBUG should stay quiet
    // for healthy threads but must make sustained CPU consumption attributable
    // without requiring an operator to reproduce the incident at ALL. Use
    // hysteresis and a long repeat interval so a genuinely hot worker is visible
    // without turning the diagnostic itself into log traffic.
    if (!Log::enabled(LogLevel::all) && Log::enabled(LogLevel::debug)) {
        if (pct >= debug_high_thread_cpu_pct) {
            if (!debug_high_cpu_ ||
                now - debug_last_report_ >= debug_high_thread_cpu_repeat) {
                Log::debug(thread_cpu_message("DIAG high thread CPU", name_, wall_ms,
                                              cpu_ms, pct, iterations_));
                debug_last_report_ = now;
            }
            debug_high_cpu_ = true;
        } else if (debug_high_cpu_ && pct <= debug_recovered_thread_cpu_pct) {
            Log::debug(thread_cpu_message("DIAG thread CPU recovered", name_, wall_ms,
                                          cpu_ms, pct, iterations_));
            debug_high_cpu_ = false;
        }
    }

    wall_started_ = now;
    cpu_started_ = cpu_now;
    iterations_ = 0;
}

DiagnosticLock::DiagnosticLock(std::mutex& mutex, std::string_view name,
                               std::chrono::milliseconds threshold)
    : lock_(mutex, std::defer_lock), name_(name), threshold_(threshold),
      enabled_(Log::enabled(LogLevel::all)) {
    if (!enabled_) {
        lock_.lock();
        return;
    }
    const auto started = Clock::now();
    lock_.lock();
    acquired_ = Clock::now();
    wait_ms_ = elapsed_ms(started);
}

DiagnosticLock::~DiagnosticLock() noexcept {
    try {
        if (!enabled_) {
            if (lock_.owns_lock())
                lock_.unlock();
            return;
        }
        const auto held_ms = elapsed_ms(acquired_);
        if (lock_.owns_lock())
            lock_.unlock();
        if (wait_ms_ < threshold_.count() && held_ms < threshold_.count())
            return;
        if (wait_ms_ >= threshold_.count())
            Log::trace("DIAG lock-wait lock=" + std::string(name_) +
                       " wait_ms=" + std::to_string(wait_ms_));
        if (held_ms >= threshold_.count())
            Log::trace("DIAG lock-held lock=" + std::string(name_) +
                       " held_ms=" + std::to_string(held_ms) +
                       " wait_ms=" + std::to_string(wait_ms_));
    } catch (...) {
    }
}

int64_t elapsed_ms(Clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
}

} // namespace macha
