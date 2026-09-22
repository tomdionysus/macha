// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace macha {

// What a disk is actually doing, measured rather than declared.
//
// Every other bound on DATA work in this codebase is stated up front: bytes in
// flight, concurrent operations, a maintenance bandwidth fraction. None of them
// is derived from the device. That is why one ordinary ingest could take a node
// to 91% iowait with single 4 MiB extent writes at 17.7 s while every declared
// budget was satisfied: the bookkeeping was correct and the disk was gone.
//
// This is the primitive that was missing. It records completion latency of
// local store operations on one durability domain and answers one question:
// is this device currently slow enough that work nobody is waiting for should
// stand aside?
//
// Cost discipline: two steady_clock reads and a handful of relaxed atomic
// updates per operation, no locks and no timer thread. If measuring service
// time measurably costs service time, this has failed on its own terms.
//
// It observes only operations that pass through the pool, which has a
// consequence worth stating: a device saturated by something outside macha --
// libtorrent writing to its save path, another process entirely -- raises this
// signal only once macha's own reads and writes start taking longer as a
// result. That is the right shape rather than a gap. A disk nobody is reading
// can be as busy as it likes and starve nothing; pressure is only meaningful
// when there is work to protect, and when there is, that work is itself the
// probe.
class DiskServiceMonitor {
  public:
    struct Thresholds {
        // Service time the backend defends. Above this, work nobody is waiting
        // for is refused admission to this device.
        std::chrono::milliseconds target{50};
        // Hysteresis floor: pressure is released when the average falls back
        // below this. Without a gap, a device sitting at the threshold
        // oscillates and the loader stutters instead of yielding.
        std::chrono::milliseconds release{20};
    };

    struct Sample {
        uint64_t operations{};
        uint64_t bytes{};
        // Exponentially weighted mean completion latency, in microseconds.
        uint64_t mean_us{};
        // Worst completion latency seen in the recent window, in microseconds.
        // Kept because a mean of 40 ms hides the 17 s write that actually broke
        // a viewer, and an operator reading a status page needs to see it.
        uint64_t worst_us{};
        bool pressured{};
    };

    DiskServiceMonitor() = default;
    explicit DiskServiceMonitor(Thresholds thresholds) : thresholds_(thresholds) {}

    // Set once during construction of the node, before any operation is
    // recorded. Not assignment: the counters are atomics and the monitor is
    // owned in place by the pool it measures.
    void configure(Thresholds thresholds) noexcept {
        thresholds_ = thresholds;
    }

    // Records one completed local store operation. Called from the completion
    // path of every DATA read and write, whatever class of work issued it --
    // the device does not care who queued the request, and neither does this.
    void note(std::chrono::nanoseconds elapsed, uint64_t bytes) noexcept {
        const auto micros =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                                      .count());
        operations_.fetch_add(1, std::memory_order_relaxed);
        bytes_.fetch_add(bytes, std::memory_order_relaxed);

        // A deliberately racy EWMA. Concurrent completions may interleave and
        // lose an update; over the rates involved that changes the mean by
        // noise, and the alternative is a lock on the hot completion path of
        // every extent write in the system.
        auto mean = mean_us_.load(std::memory_order_relaxed);
        const auto next = mean ? (mean * (weight_denominator - 1) + micros) / weight_denominator
                               : micros;
        mean_us_.store(next, std::memory_order_relaxed);

        auto worst = worst_us_.load(std::memory_order_relaxed);
        while (micros > worst &&
               !worst_us_.compare_exchange_weak(worst, micros, std::memory_order_relaxed))
            ;

        // Hysteresis, evaluated here so the admission path is a single relaxed
        // load rather than a comparison it has to get right at every call site.
        const auto target_us = static_cast<uint64_t>(thresholds_.target.count()) * 1000;
        const auto release_us = static_cast<uint64_t>(thresholds_.release.count()) * 1000;
        if (next > target_us) {
            if (!pressured_.exchange(true, std::memory_order_relaxed))
                pressure_onsets_.fetch_add(1, std::memory_order_relaxed);
        } else if (next < release_us) {
            pressured_.store(false, std::memory_order_relaxed);
        }
    }

    // Is this device slow enough right now that deferrable work should stand
    // aside? Interactive work must never consult this: if the disk is slow, a
    // person waiting on it gets all of it.
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
                      pressured_.load(std::memory_order_relaxed)};
    }

    // Forgets the worst-case high-water mark. An operator clearing it after
    // reading it is the intended use; the mean and the pressure state are
    // deliberately not resettable, because they describe now.
    void forget_worst() noexcept {
        worst_us_.store(0, std::memory_order_relaxed);
    }

    const Thresholds& thresholds() const noexcept {
        return thresholds_;
    }

  private:
    // 1/16 weight on each new sample. A modest outlier is absorbed -- one
    // 200 ms write against a 2 ms mean does not gate anything -- while a
    // catastrophic one moves the mean past the target on its own, which is
    // deliberate: a single 4-second write means the device is already in
    // trouble, and that single write is the event that starved a viewer on
    // 2026-09-19. Recovery takes roughly 50 fast samples from a half-second
    // mean, so the loader is not handed the disk back on one lucky write
    // either.
    static constexpr uint64_t weight_denominator = 16;

    Thresholds thresholds_{};
    std::atomic<uint64_t> operations_{};
    std::atomic<uint64_t> bytes_{};
    std::atomic<uint64_t> mean_us_{};
    std::atomic<uint64_t> worst_us_{};
    std::atomic<uint64_t> pressure_onsets_{};
    std::atomic<bool> pressured_{};
};

// Times one local store operation and reports it on destruction. Declared at
// the top of a store entry point; the measurement then cannot be forgotten on
// an early return or skipped by an exception.
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
