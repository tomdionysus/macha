// SPDX-License-Identifier: GPL-3.0-or-later
#include "observation.hpp"

#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <utility>

namespace macha {
namespace {

uint64_t minus(uint64_t later, uint64_t earlier) noexcept {
    return later >= earlier ? later - earlier : 0;
}

// Octave of a bucket at or above 8: the bucket holds values whose leading
// bit is 2^octave.
size_t bucket_octave(size_t bucket) noexcept {
    return (bucket - 8) / observation_sub_buckets + 3;
}

bool route_word(std::string_view segment) noexcept {
    if (segment.empty() || segment.size() > 32)
        return false;
    return std::all_of(segment.begin(), segment.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || c == '_' || c == '-'; });
}

} // namespace

size_t observation_bucket(uint64_t value) noexcept {
    if (value < 8)
        return static_cast<size_t>(value);
    const auto octave = static_cast<size_t>(63 - std::countl_zero(value));
    const auto sub = static_cast<size_t>((value >> (octave - 3)) & 7U);
    return 8 + (octave - 3) * observation_sub_buckets + sub;
}

uint64_t observation_bucket_lower(size_t bucket) noexcept {
    if (bucket < 8)
        return bucket;
    const auto octave = bucket_octave(bucket);
    const auto sub = (bucket - 8) % observation_sub_buckets;
    return static_cast<uint64_t>(8 + sub) << (octave - 3);
}

uint64_t observation_bucket_upper(size_t bucket) noexcept {
    if (bucket < 8)
        return bucket;
    const auto width = uint64_t{1} << (bucket_octave(bucket) - 3);
    return observation_bucket_lower(bucket) + (width - 1);
}

uint64_t HistogramSnapshot::quantile(double q) const noexcept {
    if (count == 0)
        return 0;
    q = std::clamp(q, 0.0, 1.0);
    const auto wanted = std::ceil(q * static_cast<double>(count));
    const auto rank = std::clamp<uint64_t>(static_cast<uint64_t>(wanted), 1, count);
    uint64_t seen = 0;
    for (size_t bucket = 0; bucket < buckets.size(); ++bucket) {
        seen += buckets[bucket];
        if (seen >= rank)
            return std::min(observation_bucket_upper(bucket), max);
    }
    return max;
}

HistogramSnapshot HistogramSnapshot::since(const HistogramSnapshot& earlier) const noexcept {
    HistogramSnapshot window;
    size_t highest = buckets.size();
    for (size_t bucket = 0; bucket < buckets.size(); ++bucket) {
        window.buckets[bucket] = minus(buckets[bucket], earlier.buckets[bucket]);
        window.count += window.buckets[bucket];
        if (window.buckets[bucket])
            highest = bucket;
    }
    window.sum = minus(sum, earlier.sum);
    if (highest != buckets.size())
        window.max = std::min(observation_bucket_upper(highest), max);
    return window;
}

void LatencyHistogram::record(uint64_t value) noexcept {
    buckets_[observation_bucket(value)].fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(value, std::memory_order_relaxed);
    auto seen = max_.load(std::memory_order_relaxed);
    while (seen < value && !max_.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

HistogramSnapshot LatencyHistogram::snapshot() const noexcept {
    HistogramSnapshot snapshot;
    for (size_t bucket = 0; bucket < buckets_.size(); ++bucket) {
        snapshot.buckets[bucket] = buckets_[bucket].load(std::memory_order_relaxed);
        snapshot.count += snapshot.buckets[bucket];
    }
    snapshot.sum = sum_.load(std::memory_order_relaxed);
    snapshot.max = max_.load(std::memory_order_relaxed);
    return snapshot;
}

ObservationSnapshot ObservationSnapshot::since(const ObservationSnapshot& earlier) const {
    ObservationSnapshot window;
    for (const auto& [name, histogram] : histograms) {
        const auto before = earlier.histograms.find(name);
        window.histograms.emplace(name, before == earlier.histograms.end()
                                            ? histogram.since(HistogramSnapshot{})
                                            : histogram.since(before->second));
    }
    for (const auto& [name, value] : counters) {
        const auto before = earlier.counters.find(name);
        window.counters.emplace(name,
                                minus(value, before == earlier.counters.end() ? 0 : before->second));
    }
    return window;
}

Observations::Observations(size_t max_series, size_t max_events)
    : max_series_(max_series), max_events_(max_events) {}

LatencyHistogram& Observations::histogram(std::string_view name) {
    std::lock_guard lock(mutex_);
    if (auto found = histograms_.find(name); found != histograms_.end())
        return *found->second;
    if (histograms_.size() >= max_series_) {
        overflowed_.fetch_add(1, std::memory_order_relaxed);
        return overflow_histogram_;
    }
    return *histograms_.emplace(std::string(name), std::make_unique<LatencyHistogram>())
                .first->second;
}

std::atomic<uint64_t>& Observations::counter(std::string_view name) {
    std::lock_guard lock(mutex_);
    if (auto found = counters_.find(name); found != counters_.end())
        return *found->second;
    if (counters_.size() >= max_series_) {
        overflowed_.fetch_add(1, std::memory_order_relaxed);
        return overflow_counter_;
    }
    return *counters_.emplace(std::string(name), std::make_unique<std::atomic<uint64_t>>(0))
                .first->second;
}

void Observations::event(ObservationEvent event) {
    std::lock_guard lock(mutex_);
    if (events_.size() >= max_events_) {
        events_.pop_front();
        events_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    events_.push_back(std::move(event));
}

ObservationSnapshot Observations::snapshot() const {
    ObservationSnapshot snapshot;
    std::lock_guard lock(mutex_);
    for (const auto& [name, histogram] : histograms_)
        snapshot.histograms.emplace(name, histogram->snapshot());
    for (const auto& [name, counter] : counters_)
        snapshot.counters.emplace(name, counter->load(std::memory_order_relaxed));
    snapshot.histograms.emplace(std::string(overflow_series), overflow_histogram_.snapshot());
    snapshot.counters.emplace(std::string(overflow_series),
                              overflowed_.load(std::memory_order_relaxed));
    snapshot.counters.emplace(std::string(events_dropped_series),
                              events_dropped_.load(std::memory_order_relaxed));
    return snapshot;
}

std::deque<ObservationEvent> Observations::drain_events() {
    std::lock_guard lock(mutex_);
    return std::exchange(events_, {});
}

Observations& observations() {
    // Never destroyed: probes on threads that outlive static destruction at
    // exit must still find it.
    static auto* registry = new Observations();
    return *registry;
}

uint64_t elapsed_us(Clock::time_point started) noexcept {
    const auto elapsed = Clock::now() - started;
    if (elapsed.count() <= 0)
        return 0;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

std::string observation_route_label(std::string_view method, std::string_view path) {
    std::string label = "api ";
    if (method == "GET" || method == "POST" || method == "PUT" || method == "PATCH" ||
        method == "DELETE")
        label += method;
    else
        label += "OTHER";
    static constexpr std::string_view api_prefix = "/api/";
    static constexpr std::string_view v1_prefix = "/api/v1/";
    if (!path.starts_with(api_prefix))
        return label + " web";
    if (!path.starts_with(v1_prefix))
        return label + " /api/:other";
    label += " /api/v1";
    auto rest = path.substr(v1_prefix.size());
    for (size_t kept = 0; kept < 3 && !rest.empty(); ++kept) {
        const auto slash = rest.find('/');
        const auto segment = rest.substr(0, slash);
        label += '/';
        label += route_word(segment) ? std::string(segment) : std::string(":id");
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    return label;
}

std::string render_observation_window(uint64_t start_unix_ms, uint64_t end_unix_ms,
                                      std::string_view version,
                                      const ObservationSnapshot& window,
                                      const std::map<std::string, uint64_t>& gauges) {
    Json::Object histograms;
    for (const auto& [name, histogram] : window.histograms) {
        if (!histogram.count)
            continue;
        Json::Array buckets;
        for (size_t bucket = 0; bucket < histogram.buckets.size(); ++bucket)
            if (histogram.buckets[bucket])
                buckets.push_back(Json::Array{Json(static_cast<uint64_t>(bucket)),
                                              Json(histogram.buckets[bucket])});
        histograms[name] = Json::Object{{"count", histogram.count},
                                        {"sum", histogram.sum},
                                        {"max", histogram.max},
                                        {"p50", histogram.quantile(0.5)},
                                        {"p90", histogram.quantile(0.9)},
                                        {"p99", histogram.quantile(0.99)},
                                        {"buckets", std::move(buckets)}};
    }
    Json::Object counters;
    for (const auto& [name, value] : window.counters)
        if (value)
            counters[name] = value;
    Json::Object gauge_values;
    for (const auto& [name, value] : gauges)
        gauge_values[name] = value;
    return Json(Json::Object{{"kind", "window"},
                             {"start_ms", start_unix_ms},
                             {"end_ms", end_unix_ms},
                             {"version", std::string(version)},
                             {"histograms", std::move(histograms)},
                             {"counters", std::move(counters)},
                             {"gauges", std::move(gauge_values)}})
        .dump();
}

std::string render_observation_event(const ObservationEvent& event) {
    Json::Object fields;
    for (const auto& [name, value] : event.numbers)
        fields[name] = value;
    for (const auto& [name, value] : event.texts)
        fields[name] = value;
    return Json(Json::Object{{"kind", "event"},
                             {"at_ms", event.unix_ms},
                             {"event", event.name},
                             {"fields", std::move(fields)}})
        .dump();
}

ObservationLog::ObservationLog(std::filesystem::path path, uint64_t max_bytes)
    : path_(std::move(path)), max_bytes_(max_bytes) {}

void ObservationLog::append(const std::string& line) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    const auto size = std::filesystem::file_size(path_, ec);
    if (!ec && size > 0 && size + line.size() + 1 > max_bytes_) {
        auto rotated = path_;
        rotated += ".1";
        std::filesystem::rename(path_, rotated, ec);
    }
    std::ofstream out(path_, std::ios::app | std::ios::binary);
    out << line << '\n';
    out.flush();
    if (!out) {
        if (!failing_)
            Log::debug("observation: cannot write " + path_.string());
        failing_ = true;
        return;
    }
    failing_ = false;
}

ObservationRecorder::ObservationRecorder(Observations& observations,
                                         std::shared_ptr<ObservationLog> log, std::string version,
                                         std::chrono::milliseconds interval, GaugeSampler sampler)
    : observations_(observations), log_(std::move(log)), version_(std::move(version)),
      interval_(interval), sampler_(std::move(sampler)) {}

ObservationRecorder::~ObservationRecorder() {
    stop();
}

void ObservationRecorder::start() {
    previous_unix_ms_ = unix_ms();
    thread_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("observation", stop, [this, stop] {
            while (true) {
                {
                    std::unique_lock lock(wait_mutex_);
                    wait_cv_.wait_for(lock, stop, interval_, [] { return false; });
                    if (stop.stop_requested())
                        return;
                }
                tick(unix_ms());
            }
        });
    });
}

void ObservationRecorder::stop() {
    if (!thread_.joinable())
        return;
    thread_.request_stop();
    wait_cv_.notify_all();
    thread_.join();
    tick(unix_ms());
}

void ObservationRecorder::tick(uint64_t now_unix_ms) {
    std::lock_guard lock(tick_mutex_);
    auto current = observations_.snapshot();
    std::map<std::string, uint64_t> gauges;
    if (sampler_) {
        try {
            gauges = sampler_();
        } catch (const std::exception& error) {
            Log::debug("observation: gauge sampling failed: " + std::string(error.what()));
        }
    }
    for (const auto& event : observations_.drain_events())
        log_->append(render_observation_event(event));
    log_->append(render_observation_window(previous_unix_ms_, now_unix_ms, version_,
                                           current.since(previous_), gauges));
    previous_ = std::move(current);
    previous_unix_ms_ = now_unix_ms;
}

} // namespace macha
