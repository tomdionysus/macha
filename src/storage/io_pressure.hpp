// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "log.hpp"

namespace macha {

// Measured DATA device service time: answers whether the device is slow enough
// that work nobody is waiting for should stand aside.
//
// One monitor covers a whole StoragePool and cannot tell its backends apart, so
// one slow backend pressures work bound anywhere in the pool. Per-device
// pressure is not possible because admission happens before placement picks a
// backend.
//
// Cost: two steady_clock reads and a few relaxed atomics per operation; no
// locks, no timer thread.
//
// Only operations through the pool are observed: a device saturated by another
// process raises the signal only once macha's own I/O slows. Pressure matters
// only when there is work to protect, and that work is the probe.
class DiskServiceMonitor {
  public:
    struct Thresholds {
        // Expected cost on a healthy device: fixed overhead plus per-MiB time.
        // Deliberately generous (about 8 MB/s): the question is "far worse than
        // it should be", not "fast". Scaling by size keeps a 4 MiB write and a
        // 4 KiB read from being held to the same figure.
        std::chrono::milliseconds overhead{25};
        std::chrono::milliseconds per_mib{120};
        // Pressure when the moving average of actual/expected exceeds this
        // percentage; released below release_percent.
        uint32_t slowdown_percent{300};
        uint32_t release_percent{150};
        // One operation past this ratio trips pressure at once. The 1/16
        // average cannot catch a lone catastrophic write among healthy ones; a
        // single sample would need ~4,800% to carry it over 300%. 1000% of a
        // budget this loose is a device that has stopped serving, well clear of
        // ordinary variance.
        uint32_t outlier_percent{1000};
    };

    struct Sample {
        uint64_t operations{};
        uint64_t bytes{};
        // EWMA completion latency, microseconds. Reported only; not size-aware,
        // so pressure is not decided on it.
        uint64_t mean_us{};
        // Worst completion latency in the recent window, microseconds; a mean
        // hides the single slow write that starves a viewer.
        uint64_t worst_us{};
        // Moving average of actual/expected, percent (100 = as expected). The
        // signal pressure is decided on.
        uint64_t slowdown_percent{};
        bool pressured{};
    };

    DiskServiceMonitor() = default;
    explicit DiskServiceMonitor(Thresholds thresholds) : thresholds_(thresholds) {}

    // Called once at node construction, before any operation is recorded. Not
    // assignment: the counters are atomics and the monitor is owned in place.
    void configure(Thresholds thresholds) noexcept {
        thresholds_ = thresholds;
    }

    // Records one completed DATA read or write, whatever class of work issued it.
    void note(std::chrono::nanoseconds elapsed, uint64_t bytes) noexcept {
        const auto micros =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                                      .count());
        operations_.fetch_add(1, std::memory_order_relaxed);
        bytes_.fetch_add(bytes, std::memory_order_relaxed);

        // Deliberately racy EWMA: a lost update is noise; a lock would sit on
        // the hot completion path of every extent write.
        auto mean = mean_us_.load(std::memory_order_relaxed);
        const auto next = mean ? (mean * (weight_denominator - 1) + micros) / weight_denominator
                               : micros;
        mean_us_.store(next, std::memory_order_relaxed);

        auto worst = worst_us_.load(std::memory_order_relaxed);
        while (micros > worst &&
               !worst_us_.compare_exchange_weak(worst, micros, std::memory_order_relaxed))
            ;

        // What this operation should have cost on a coping device, given its size.
        constexpr uint64_t mib = 1024 * 1024;
        const auto expected_us =
            static_cast<uint64_t>(thresholds_.overhead.count()) * 1000 +
            (bytes * static_cast<uint64_t>(thresholds_.per_mib.count()) * 1000) / mib;
        const auto ratio = expected_us ? (micros * 100) / expected_us : 100;

        auto slowdown = slowdown_percent_.load(std::memory_order_relaxed);
        const auto next_slowdown =
            slowdown ? (slowdown * (weight_denominator - 1) + ratio) / weight_denominator : ratio;
        slowdown_percent_.store(next_slowdown, std::memory_order_relaxed);

        // Hysteresis evaluated here so admission is a single relaxed load. Each
        // transition is logged once.
        if (next_slowdown > thresholds_.slowdown_percent ||
            (thresholds_.outlier_percent && ratio > thresholds_.outlier_percent)) {
            if (!pressured_.exchange(true, std::memory_order_relaxed)) {
                pressure_onsets_.fetch_add(1, std::memory_order_relaxed);
                log_transition("DATA device pressure onset", next_slowdown, ratio, micros, bytes);
            }
        } else if (next_slowdown < thresholds_.release_percent) {
            if (pressured_.exchange(false, std::memory_order_relaxed))
                log_transition("DATA device pressure released", next_slowdown, ratio, micros, bytes);
        }
    }

    // Whether deferrable work should stand aside. Interactive work must never
    // consult this: a person waiting on a slow disk gets all of it.
    bool pressured() const noexcept {
        return pressured_.load(std::memory_order_relaxed);
    }

    uint64_t pressure_onsets() const noexcept {
        return pressure_onsets_.load(std::memory_order_relaxed);
    }

    Sample sample() const noexcept {
        return Sample{operations_.load(std::memory_order_relaxed),
                      bytes_.load(std::memory_order_relaxed),
                      mean_us_.load(std::memory_order_relaxed),
                      worst_us_.load(std::memory_order_relaxed),
                      slowdown_percent_.load(std::memory_order_relaxed),
                      pressured_.load(std::memory_order_relaxed)};
    }

    // Clears the worst-case high-water mark. The mean and pressure state are
    // not resettable: they describe now.
    void forget_worst() noexcept {
        worst_us_.store(0, std::memory_order_relaxed);
    }

    const Thresholds& thresholds() const noexcept {
        return thresholds_;
    }

  private:
    static void log_transition(const char* what, uint64_t slowdown, uint64_t ratio, uint64_t micros,
                               uint64_t bytes) noexcept {
        try {
            Log::info(std::string(what) + " slowdown_percent=" + std::to_string(slowdown) +
                      " last_percent=" + std::to_string(ratio) + " last_us=" + std::to_string(micros) +
                      " last_bytes=" + std::to_string(bytes));
        } catch (...) {
            // A log line must never fail the I/O it describes.
        }
    }

    // 1/16 weight per sample: a modest outlier is absorbed, a catastrophic one
    // moves the mean on its own, and recovery takes many fast samples rather
    // than one lucky write.
    static constexpr uint64_t weight_denominator = 16;

    Thresholds thresholds_{};
    std::atomic<uint64_t> operations_{};
    std::atomic<uint64_t> bytes_{};
    std::atomic<uint64_t> mean_us_{};
    std::atomic<uint64_t> worst_us_{};
    std::atomic<uint64_t> slowdown_percent_{};
    std::atomic<uint64_t> pressure_onsets_{};
    std::atomic<bool> pressured_{};
};

// Times one local store operation and reports it on destruction, so early
// returns and exceptions are still measured.
class DiskServiceTimer {
  public:
    DiskServiceTimer(DiskServiceMonitor* monitor, uint64_t bytes) noexcept
        : monitor_(monitor), bytes_(bytes),
          started_(monitor ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{}) {}
    ~DiskServiceTimer() {
        if (monitor_)
            monitor_->note(std::chrono::steady_clock::now() - started_, bytes_);
    }
    DiskServiceTimer(const DiskServiceTimer&) = delete;
    DiskServiceTimer& operator=(const DiskServiceTimer&) = delete;

    // For a read, the size is only known once it has succeeded.
    void note_bytes(uint64_t bytes) noexcept {
        bytes_ = bytes;
    }

  private:
    DiskServiceMonitor* monitor_{};
    uint64_t bytes_{};
    std::chrono::steady_clock::time_point started_{};
};

} // namespace macha
