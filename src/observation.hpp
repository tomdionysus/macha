// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "types.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// Measurement probes, written only to a local file. Nothing here reaches
// Status or the API, and no probe may change a node decision: each is a
// counter increment, a histogram record or a bounded-queue event.
namespace macha {

// Log-linear: values 0-7 have a bucket each, and every power of two from 8 up
// is split into eight equal buckets, so a bucket's width is at most 1/8 of
// its lower bound and any quantile read from it is within 12.5% of the value
// recorded. 496 buckets cover the whole uint64_t range.
//
// Invariants, for every uint64_t v:
//   observation_bucket_lower(observation_bucket(v)) <= v
//   v <= observation_bucket_upper(observation_bucket(v))
// and buckets are contiguous: lower(b + 1) == upper(b) + 1.
inline constexpr size_t observation_sub_buckets = 8;
inline constexpr size_t observation_bucket_count = 8 + 61 * observation_sub_buckets;

size_t observation_bucket(uint64_t value) noexcept;
// Precondition for both: bucket < observation_bucket_count.
uint64_t observation_bucket_lower(size_t bucket) noexcept;
uint64_t observation_bucket_upper(size_t bucket) noexcept;

// A histogram's counts at one moment, or over a window (the difference of
// two moments). A plain value; every operation on it is deterministic.
struct HistogramSnapshot {
    std::array<uint64_t, observation_bucket_count> buckets{};
    uint64_t count{}; // == the sum of buckets
    uint64_t sum{};
    uint64_t max{};

    // The smallest bucket upper bound at or above which a fraction q of the
    // recorded values lie, capped at max. q is clamped to [0, 1]; an empty
    // snapshot answers 0. quantile(1.0) == max.
    uint64_t quantile(double q) const noexcept;
    // This snapshot less an earlier one of the same histogram. Precondition:
    // earlier was taken from the same histogram no later than this one, so
    // every bucket is monotone. max of the window is not recoverable from two
    // cumulative maxima; it is the upper bound of the window's highest
    // non-empty bucket, capped at this snapshot's max.
    HistogramSnapshot since(const HistogramSnapshot& earlier) const noexcept;
    bool operator==(const HistogramSnapshot&) const = default;
};

// Lock-free: record() is three relaxed atomic adds and a compare-exchange
// loop on the maximum, safe from any thread. A snapshot taken while records
// land may see a record's bucket but not yet its sum; count is derived from
// the buckets, so count and quantiles always agree with each other.
class LatencyHistogram {
    std::array<std::atomic<uint64_t>, observation_bucket_count> buckets_{};
    std::atomic<uint64_t> sum_{};
    std::atomic<uint64_t> max_{};

  public:
    void record(uint64_t value) noexcept;
    HistogramSnapshot snapshot() const noexcept;
};

// A one-off fact with its own time: a startup finished, a backend went
// offline. Fields are rendered as JSON strings or numbers.
struct ObservationEvent {
    uint64_t unix_ms{};
    std::string name;
    std::map<std::string, uint64_t> numbers;
    std::map<std::string, std::string> texts;
    bool operator==(const ObservationEvent&) const = default;
};

struct ObservationSnapshot {
    std::map<std::string, HistogramSnapshot, std::less<>> histograms;
    std::map<std::string, uint64_t, std::less<>> counters;
    // This snapshot less an earlier one of the same registry. A series the
    // earlier snapshot lacks counts from zero.
    ObservationSnapshot since(const ObservationSnapshot& earlier) const;
};

// Named histograms and counters, and a bounded queue of events.
//
// Bounds: at most max_series histograms and max_series counters are ever
// created; a name beyond that is folded into the series named
// `overflow_series` and counted in the counter of that name, so a label
// derived from input (an HTTP path) cannot grow memory. At most max_events
// events wait to be written; beyond that the oldest is dropped and counted.
// Series are never removed, and a returned reference is valid for the
// registry's life.
class Observations {
  public:
    static constexpr std::string_view overflow_series = "observation.overflow";
    static constexpr std::string_view events_dropped_series = "observation.events_dropped";
    static constexpr size_t default_max_series = 512;
    static constexpr size_t default_max_events = 256;

    explicit Observations(size_t max_series = default_max_series,
                          size_t max_events = default_max_events);

    LatencyHistogram& histogram(std::string_view name);
    std::atomic<uint64_t>& counter(std::string_view name);
    void add(std::string_view name, uint64_t amount = 1) {
        counter(name).fetch_add(amount, std::memory_order_relaxed);
    }
    void record(std::string_view name, uint64_t value) {
        histogram(name).record(value);
    }
    void event(ObservationEvent);

    ObservationSnapshot snapshot() const;
    // Removes and returns every waiting event, oldest first.
    std::deque<ObservationEvent> drain_events();

  private:
    const size_t max_series_;
    const size_t max_events_;
    mutable Mutex mutex_;
    std::map<std::string, std::unique_ptr<LatencyHistogram>, std::less<>> histograms_
        MACHA_GUARDED_BY(mutex_);
    std::map<std::string, std::unique_ptr<std::atomic<uint64_t>>, std::less<>> counters_
        MACHA_GUARDED_BY(mutex_);
    std::deque<ObservationEvent> events_ MACHA_GUARDED_BY(mutex_);
    LatencyHistogram overflow_histogram_;
    // Where increments to a counter past the bound go; never reported.
    std::atomic<uint64_t> overflow_counter_{};
    std::atomic<uint64_t> overflowed_{};
    std::atomic<uint64_t> events_dropped_{};
};

// The process's registry. One node per process in production; in-process
// test clusters share it, which only mixes their figures.
Observations& observations();

// Microseconds since `started`, never negative.
uint64_t elapsed_us(Clock::time_point started) noexcept;

// Records the time from construction to destruction, in microseconds.
class ObservedDuration {
    LatencyHistogram& histogram_;
    Clock::time_point started_{Clock::now()};

  public:
    explicit ObservedDuration(LatencyHistogram& histogram) noexcept : histogram_(histogram) {}
    ~ObservedDuration() {
        histogram_.record(elapsed_us(started_));
    }
    ObservedDuration(const ObservedDuration&) = delete;
    ObservedDuration& operator=(const ObservedDuration&) = delete;
};

// The series name for an HTTP request's latency: "api " + method + route.
// Routes under /api/ keep at most three segments after /api/v1/, each kept
// only if it is 1-32 characters of [a-z_-] and replaced by ":id" otherwise;
// anything outside /api/ is "web". Methods other than the usual five are
// "OTHER". A pure function of its inputs.
std::string observation_route_label(std::string_view method, std::string_view path);

// One JSON line per window. Histograms and counters that did not move in the
// window are omitted; each histogram carries its count, sum, max, p50, p90,
// p99 and its non-empty buckets as [index, count] pairs, so windows can be
// merged exactly offline. Deterministic: equal inputs render equal text.
std::string render_observation_window(uint64_t start_unix_ms, uint64_t end_unix_ms,
                                      std::string_view version,
                                      const ObservationSnapshot& window,
                                      const std::map<std::string, uint64_t>& gauges);
std::string render_observation_event(const ObservationEvent&);

// Appends lines to one file, rotating it to `<path>.1` (replacing any older
// one) once it reaches max_bytes, so the pair never exceeds about twice that.
// Best effort: a failure to write loses the line and is logged at DEBUG once
// per failure run. Throws only on allocation failure. Thread-safe.
class ObservationLog {
    std::filesystem::path path_;
    uint64_t max_bytes_;
    // Held across the append and the rotation.
    IoMutex mutex_;
    bool failing_ MACHA_GUARDED_BY(mutex_){};

  public:
    ObservationLog(std::filesystem::path path, uint64_t max_bytes);
    const std::filesystem::path& path() const noexcept {
        return path_;
    }
    void append(const std::string& line);
};

// Writes a window every `interval` from its own thread: the registry's
// movement since the previous window, the gauges sampled at that moment, and
// any events waiting. stop() writes a final window, so a clean shutdown's
// last figures and its shutdown event are not lost. The thread is a
// supervised loop named "observation", so it is listed in Status `threads`.
class ObservationRecorder {
  public:
    using GaugeSampler = std::function<std::map<std::string, uint64_t>()>;

    ObservationRecorder(Observations&, std::shared_ptr<ObservationLog>, std::string version,
                        std::chrono::milliseconds interval, GaugeSampler);
    ~ObservationRecorder();
    ObservationRecorder(const ObservationRecorder&) = delete;
    ObservationRecorder& operator=(const ObservationRecorder&) = delete;

    void start();
    void stop();
    // One window ending at now_unix_ms. Called by the thread; public so the
    // window logic is testable without one. Not concurrently with itself.
    void tick(uint64_t now_unix_ms);

  private:
    Observations& observations_;
    std::shared_ptr<ObservationLog> log_;
    std::string version_;
    std::chrono::milliseconds interval_;
    GaugeSampler sampler_;
    // Held across a window's write to the log.
    IoMutex tick_mutex_;
    ObservationSnapshot previous_ MACHA_GUARDED_BY(tick_mutex_);
    uint64_t previous_unix_ms_ MACHA_GUARDED_BY(tick_mutex_){};
    // Guards nothing; orders stop() against the thread's wait.
    Mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    std::jthread thread_;
};

} // namespace macha
