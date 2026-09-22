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
// local store operations and answers one question: is this device currently
// slow enough that work nobody is waiting for should stand aside?
//
// One monitor covers a whole StoragePool, not one durability domain -- it said
// otherwise here until 0.53.0 and the claim was simply false. A pool may hold
// several backends on several devices, and this cannot tell them apart, so one
// slow backend makes the pool pressured for work bound anywhere in it. That is
// wrong in principle and harmless in this cluster today, where gbni-1 and es-1
// each configure exactly one DATA backend and fi-1 holds no extents at all;
// it starts to bite the moment a second backend is configured. Per-device
// pressure would also need the arbiter to know an operation's destination
// device, which it cannot: admission happens before placement picks a
// backend. Recorded rather than papered over.
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
        // What an operation on a healthy device is expected to cost:
        // a fixed per-operation budget plus a budget per MiB transferred.
        // Deliberately generous -- roughly 8 MB/s sustained -- because the
        // question is not "is this device fast" but "is this device far worse
        // than it should be".
        //
        // A fixed millisecond threshold was the first design and it was wrong
        // in a way worth recording: it compared a 4 MiB extent write against
        // the same 50 ms as a 4 KiB read, so every storage node doing ordinary
        // work declared permanent pressure. On 2026-09-22 that pinned a node
        // into "pressured" nine seconds after boot and held an operator's
        // import to one background lease for an afternoon.
        std::chrono::milliseconds overhead{25};
        std::chrono::milliseconds per_mib{120};
        // Pressure when the moving average of actual/expected exceeds this
        // percentage, released when it falls back under the lower one. A
        // device three times slower than a generous expectation is in trouble;
        // one merely at the expectation is working.
        uint32_t slowdown_percent{300};
        uint32_t release_percent{150};
        // A single operation this far past what it should have cost trips
        // pressure at once, whatever the average says. The moving average
        // catches sustained degradation; it cannot catch the event that
        // started this work -- one 17.7 s extent write among fifty healthy
        // ones, which moves a 1/16 average from 19% only to 237%, under the
        // 300% line. That write scored 3,505% on its own against its own
        // expectation, and one operation that far out is a device in trouble
        // now, not a statistic to be averaged.
        //
        // This was an absolute 2 s until 0.53.0, which was the last guess
        // about hardware left in the model and an unequal one: 2 s is 396% of
        // expectation for a 4 MiB write and 8,000% of it for a 4 KiB read, so
        // the same figure meant "mildly slow" for one operation and
        // "catastrophic" for another. A ratio needs no such guess and scales
        // with the operation, which is the whole argument the per-MiB
        // expectation rests on.
        //
        // 1000% is chosen against the expectation being deliberately generous
        // -- 25 ms + 120 ms/MiB is roughly 8 MB/s, several times slower than
        // the spinners in this cluster actually are. Ten times a budget that
        // loose is not jitter on any device; it is one that has stopped
        // serving. It sits well above the 300% sustained line so ordinary
        // variance cannot reach it, and well below the ~4,800% a single
        // sample would need to carry the 1/16 average over that line by
        // itself, which is the gap this trip exists to close.
        uint32_t outlier_percent{1000};
    };

    struct Sample {
        uint64_t operations{};
        uint64_t bytes{};
        // Exponentially weighted mean completion latency, in microseconds.
        // Reported for operators; it is NOT what pressure is decided on,
        // because it cannot be compared against anything without knowing the
        // size of the operations that produced it.
        uint64_t mean_us{};
        // Worst completion latency seen in the recent window, in microseconds.
        // Kept because a mean of 40 ms hides the 17 s write that actually broke
        // a viewer, and an operator reading a status page needs to see it.
        uint64_t worst_us{};
        // The signal pressure is actually decided on: the moving average of
        // actual/expected as a percentage. 100 means the device is performing
        // exactly as expected for the work it was given.
        uint64_t slowdown_percent{};
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

        // What this operation should have cost on a device that is coping,
        // given its size. Comparing against this rather than against a fixed
        // millisecond figure is the whole of the fix: a 4 MiB write and a 4 KiB
        // read are not the same event and must not be held to the same number.
        constexpr uint64_t mib = 1024 * 1024;
        const auto expected_us =
            static_cast<uint64_t>(thresholds_.overhead.count()) * 1000 +
            (bytes * static_cast<uint64_t>(thresholds_.per_mib.count()) * 1000) / mib;
        const auto ratio = expected_us ? (micros * 100) / expected_us : 100;

        auto slowdown = slowdown_percent_.load(std::memory_order_relaxed);
        const auto next_slowdown =
            slowdown ? (slowdown * (weight_denominator - 1) + ratio) / weight_denominator : ratio;
        slowdown_percent_.store(next_slowdown, std::memory_order_relaxed);

        // Hysteresis, evaluated here so the admission path is a single relaxed
        // load rather than a comparison it has to get right at every call site.
        if (next_slowdown > thresholds_.slowdown_percent ||
            (thresholds_.outlier_percent && ratio > thresholds_.outlier_percent)) {
            if (!pressured_.exchange(true, std::memory_order_relaxed))
                pressure_onsets_.fetch_add(1, std::memory_order_relaxed);
        } else if (next_slowdown < thresholds_.release_percent) {
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
                      slowdown_percent_.load(std::memory_order_relaxed),
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
    std::atomic<uint64_t> slowdown_percent_{};
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
