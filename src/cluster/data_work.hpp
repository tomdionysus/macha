// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "storage/io_pressure.hpp"
#include "log.hpp"
#include "cluster/net.hpp"
#include "contract/work.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace macha {

// Immutable provenance and bounds for DATA-plane work. CONTROL stays outside
// this pool on its own reserved transport and executors.
class DataWorkContext : public WorkContext {
    uint64_t quantum_bytes_{};
    // No-progress budget, distinct from the absolute deadline: slow work is
    // not cancelled, but work not moving at all fails. The counter is shared
    // by a pipeline's workers, so progress anywhere re-arms the window.
    const std::atomic_uint64_t* progress_{};
    std::chrono::milliseconds no_progress_budget_{};

  public:
    explicit DataWorkContext(FrameType frame_type = FrameType::loader,
                             uint64_t quantum_bytes = 0,
                             Clock::time_point deadline = {},
                             std::atomic_bool* cancelled = nullptr,
                             const std::atomic_uint64_t* progress = nullptr,
                             std::chrono::milliseconds no_progress_budget = {})
        : WorkContext(frame_type, deadline, cancelled), quantum_bytes_(quantum_bytes),
          progress_(progress), no_progress_budget_(no_progress_budget) {
        if (frame_type == FrameType::control)
            throw std::invalid_argument("control is not a DATA work class");
    }

    // Present only when the caller supplied both a counter and a budget.
    const std::atomic_uint64_t* progress() const noexcept {
        return no_progress_budget_.count() ? progress_ : nullptr;
    }
    std::chrono::milliseconds no_progress_budget() const noexcept { return no_progress_budget_; }

    uint64_t quantum_bytes() const noexcept { return quantum_bytes_; }
};

struct DataResourceStats {
    uint64_t capacity_bytes{};
    uint64_t viewer_reserve_bytes{};
    uint64_t used_bytes{};
    uint64_t peak_used_bytes{};
    uint64_t viewer_admissions{};
    uint64_t loader_admissions{};
    uint64_t speculative_admissions{};
    uint64_t viewer_waits{};
    uint64_t loader_waits{};
    uint64_t speculative_waits{};
    uint64_t cancelled_waits{};
    // Background effort ceiling and its use (loader + speculative leases).
    uint64_t background_limit{};
    uint64_t background_active{};
    uint64_t peak_background_active{};
    // Device pressure, so an operator can see why a loader slowed.
    bool device_pressured{};
    uint64_t device_service_us{};
    uint64_t device_worst_us{};
    // The pressure signal: actual/expected service time as a percentage
    // (100 = as expected for the operations' size).
    uint64_t device_slowdown_percent{};
    uint64_t device_pressure_onsets{};
    // Work the pressure gate turned away; with the onsets, tells a
    // deliberately throttled loader from an unwell node.
    uint64_t pressure_refusals{};
};

// Event-driven byte admission at blocking DATA resource boundaries. Lower
// classes may borrow all capacity except the viewer reserve, and a waiting
// viewer closes lower-class admission until it is served. Admission also
// consults measured device service time and refuses loader and speculative
// work while the disk is slow; a viewer is never refused for pressure.
// CONTROL does not enter this object.
class DataResourceArbiter {
  public:
    using Clock = DataWorkContext::Clock;

    class Lease {
        friend class DataResourceArbiter;
        DataResourceArbiter* owner_{};
        FrameType frame_type_{FrameType::speculative};
        uint64_t bytes_{};

        Lease(DataResourceArbiter& owner, FrameType frame_type, uint64_t bytes)
            : owner_(&owner), frame_type_(frame_type), bytes_(bytes) {}

      public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : owner_(other.owner_), frame_type_(other.frame_type_), bytes_(other.bytes_) {
            other.owner_ = nullptr;
            other.bytes_ = 0;
        }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = other.owner_;
                frame_type_ = other.frame_type_;
                bytes_ = other.bytes_;
                other.owner_ = nullptr;
                other.bytes_ = 0;
            }
            return *this;
        }
        ~Lease() { reset(); }
        void reset();
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        uint64_t bytes() const noexcept { return bytes_; }
    };

  private:
    uint64_t capacity_bytes_{};
    uint64_t viewer_reserve_bytes_{};
    // Null where no device is measured (tests).
    const DiskServiceMonitor* service_monitor_{};
    // Law 3: the loader is bounded, never stopped. Under pressure this many
    // background leases are still admitted, so publication and repair keep
    // trickling rather than deadlocking behind a disk they are loading.
    uint64_t min_background_under_pressure_{1};
    uint64_t pressure_refusals_{};
    // Law 3 asks whether a viewer is present, not whether one holds credit
    // now: playback is bursty and holds nothing between extents. Reads the
    // node's activity clock (maintenance.foreground_quiet); called under the
    // arbiter mutex and must not re-enter it. Null (tests): credit alone decides.
    std::function<bool()> viewer_recently_active_;
    // Maximum concurrent loader/speculative leases (one extent's hashing,
    // encryption and transfer each), bounding background CPU. Viewers are not
    // counted. 0 = no limit.
    uint64_t background_concurrency_{};
    // A wait with no caller deadline fails after this long with no release
    // anywhere in the arbiter, so a caller holding credit while acquiring
    // more cannot hang silently. Zero waits for ever.
    std::chrono::milliseconds no_progress_deadline_{};
    uint64_t releases_{};
    uint64_t no_progress_failures_{};
    mutable Mutex mutex_;
    std::condition_variable cv_;
    uint64_t used_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t lower_used_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t lower_active_ MACHA_GUARDED_BY(mutex_){};
    uint64_t peak_lower_active_ MACHA_GUARDED_BY(mutex_){};
    uint64_t peak_used_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t waiting_viewers_ MACHA_GUARDED_BY(mutex_){};
    uint64_t waiting_loaders_ MACHA_GUARDED_BY(mutex_){};
    uint64_t waiting_speculative_ MACHA_GUARDED_BY(mutex_){};
    bool stopping_ MACHA_GUARDED_BY(mutex_){};
    uint64_t viewer_admissions_ MACHA_GUARDED_BY(mutex_){};
    uint64_t loader_admissions_ MACHA_GUARDED_BY(mutex_){};
    uint64_t speculative_admissions_ MACHA_GUARDED_BY(mutex_){};
    uint64_t viewer_waits_ MACHA_GUARDED_BY(mutex_){};
    uint64_t loader_waits_ MACHA_GUARDED_BY(mutex_){};
    uint64_t speculative_waits_ MACHA_GUARDED_BY(mutex_){};
    uint64_t cancelled_waits_ MACHA_GUARDED_BY(mutex_){};

    static bool viewer(FrameType frame_type) noexcept {
        return frame_type == FrameType::foreground || frame_type == FrameType::read_ahead;
    }
    static bool loader(FrameType frame_type) noexcept {
        return frame_type == FrameType::loader;
    }
    uint64_t charge(uint64_t bytes) const noexcept { return std::max<uint64_t>(1, bytes); }
    // `refused_for_pressure` reports whether a false answer was the pressure
    // gate's doing. Callers count it once per waiter, not per wakeup.
    bool available(FrameType frame_type, uint64_t bytes,
                   bool* refused_for_pressure = nullptr) const MACHA_REQUIRES(mutex_);
    void release(FrameType frame_type, uint64_t bytes);

  public:
    DataResourceArbiter(uint64_t capacity_bytes, uint64_t viewer_reserve_bytes,
                        uint64_t background_concurrency = 0,
                        std::chrono::milliseconds no_progress_deadline = {},
                        std::function<bool()> viewer_recently_active = {});
    // Set once when the node's DATA store recovers, before it admits work.
    void observe_device(const DiskServiceMonitor* monitor,
                        uint64_t min_background_under_pressure) {
        Lock lock(mutex_);
        service_monitor_ = monitor;
        min_background_under_pressure_ = std::max<uint64_t>(1, min_background_under_pressure);
    }
    std::optional<Lease> acquire(const DataWorkContext& context, uint64_t bytes);
    std::optional<Lease> try_acquire(const DataWorkContext& context, uint64_t bytes);
    void stop();
    DataResourceStats stats() const;
};

inline DataResourceArbiter::DataResourceArbiter(uint64_t capacity_bytes,
                                                uint64_t viewer_reserve_bytes,
                                                uint64_t background_concurrency,
                                                std::chrono::milliseconds no_progress_deadline,
                                                std::function<bool()> viewer_recently_active)
    : capacity_bytes_(capacity_bytes), viewer_reserve_bytes_(viewer_reserve_bytes),
      viewer_recently_active_(std::move(viewer_recently_active)),
      background_concurrency_(background_concurrency),
      no_progress_deadline_(no_progress_deadline) {
    if (!capacity_bytes_ || !viewer_reserve_bytes_ || viewer_reserve_bytes_ >= capacity_bytes_)
        throw std::invalid_argument("DATA resource capacity must exceed viewer reserve");
}

inline bool DataResourceArbiter::available(FrameType frame_type, uint64_t bytes,
                                           bool* refused_for_pressure) const {
    if (refused_for_pressure)
        *refused_for_pressure = false;
    if (used_bytes_ > capacity_bytes_ - bytes)
        return false;
    if (viewer(frame_type))
        return true;
    if (waiting_viewers_)
        return false;
    // Law 3: the loader yields to pressure only when a viewer is present; a
    // slow device nobody is reading from must not hold an import back.
    // Speculative work yields to pressure alone. Viewer presence is checked
    // only under pressure, keeping the uncontended admission path cheap.
    if (service_monitor_ && service_monitor_->pressured() &&
        lower_active_ >= min_background_under_pressure_) {
        const bool viewer_present = waiting_viewers_ > 0 || used_bytes_ > lower_used_bytes_ ||
                                    (viewer_recently_active_ && viewer_recently_active_());
        if (frame_type == FrameType::speculative || viewer_present) {
            if (refused_for_pressure)
                *refused_for_pressure = true;
            return false;
        }
    }
    if (background_concurrency_ && lower_active_ >= background_concurrency_)
        return false;
    const auto lower_capacity = capacity_bytes_ - viewer_reserve_bytes_;
    if (bytes > lower_capacity)
        return false;
    if (lower_used_bytes_ > lower_capacity - bytes)
        return false;
    if (frame_type == FrameType::speculative && waiting_loaders_)
        return false;
    return true;
}

inline std::optional<DataResourceArbiter::Lease>
DataResourceArbiter::acquire(const DataWorkContext& context, uint64_t requested_bytes) {
    const auto frame_type = context.frame_type();
    if (frame_type == FrameType::control)
        throw std::invalid_argument("control cannot acquire DATA resource credit");
    const auto bytes = charge(requested_bytes);
    const auto class_capacity = viewer(frame_type)
                                    ? capacity_bytes_
                                    : capacity_bytes_ - viewer_reserve_bytes_;
    // Larger than the whole class budget: never admissible, fail now.
    if (bytes > class_capacity)
        return {};
    Lock lock(mutex_);
    bool counted_wait = false;
    auto& waiters = viewer(frame_type)   ? waiting_viewers_
                    : loader(frame_type) ? waiting_loaders_
                                         : waiting_speculative_;
    bool refused_for_pressure = false;
    auto wait_predicate = [&]() MACHA_REQUIRES(mutex_) {
        return stopping_ || context.cancelled() || context.expired() ||
               available(frame_type, bytes, &refused_for_pressure);
    };
    while (!wait_predicate()) {
        if (!counted_wait) {
            counted_wait = true;
            // Once per waiter, not per wakeup: counts work turned away.
            if (refused_for_pressure)
                ++pressure_refusals_;
            ++waiters;
            if (viewer(frame_type))
                ++viewer_waits_;
            else if (loader(frame_type))
                ++loader_waits_;
            else
                ++speculative_waits_;
        }
        if (context.deadline() != Clock::time_point{}) {
            cv_.wait_until(lock.native(), context.deadline(), wait_predicate);
        } else if (no_progress_deadline_ == std::chrono::milliseconds{}) {
            cv_.wait(lock.native(), wait_predicate);
        } else {
            // Wait in no-progress windows: any release anywhere re-arms the
            // window, so only a wholly stalled arbiter gives up.
            const auto seen = releases_;
            if (!cv_.wait_for(lock.native(), no_progress_deadline_,
                              [&]() MACHA_REQUIRES(mutex_) { return wait_predicate() || releases_ != seen; })) {
                ++no_progress_failures_;
                if (counted_wait)
                    --waiters;
                cv_.notify_all();
                Log::warn("DATA credit wait abandoned after " +
                          std::to_string(no_progress_deadline_.count()) +
                          " ms with no release anywhere: class=" +
                          std::string(frame_type_name(frame_type)) +
                          " bytes=" + std::to_string(bytes) +
                          " used=" + std::to_string(used_bytes_) + "/" +
                          std::to_string(capacity_bytes_) +
                          " lower_active=" + std::to_string(lower_active_) + "/" +
                          std::to_string(background_concurrency_) +
                          " waiting_viewers=" + std::to_string(waiting_viewers_) +
                          " waiting_loaders=" + std::to_string(waiting_loaders_));
                return {};
            }
        }
    }
    if (counted_wait)
        --waiters;
    if (stopping_ || context.cancelled() || context.expired()) {
        ++cancelled_waits_;
        cv_.notify_all();
        return {};
    }
    used_bytes_ += bytes;
    if (!viewer(frame_type)) {
        lower_used_bytes_ += bytes;
        ++lower_active_;
        peak_lower_active_ = std::max(peak_lower_active_, lower_active_);
    }
    peak_used_bytes_ = std::max(peak_used_bytes_, used_bytes_);
    if (viewer(frame_type))
        ++viewer_admissions_;
    else if (loader(frame_type))
        ++loader_admissions_;
    else
        ++speculative_admissions_;
    return Lease(*this, frame_type, bytes);
}

inline std::optional<DataResourceArbiter::Lease>
DataResourceArbiter::try_acquire(const DataWorkContext& context, uint64_t requested_bytes) {
    const auto frame_type = context.frame_type();
    if (frame_type == FrameType::control)
        throw std::invalid_argument("control cannot acquire DATA resource credit");
    const auto bytes = charge(requested_bytes);
    const auto class_capacity = viewer(frame_type)
                                    ? capacity_bytes_
                                    : capacity_bytes_ - viewer_reserve_bytes_;
    if (bytes > class_capacity || context.cancelled() || context.expired())
        return {};
    Lock lock(mutex_);
    bool refused_for_pressure = false;
    if (stopping_ || !available(frame_type, bytes, &refused_for_pressure)) {
        if (refused_for_pressure)
            ++pressure_refusals_;
        return {};
    }
    used_bytes_ += bytes;
    if (!viewer(frame_type)) {
        lower_used_bytes_ += bytes;
        ++lower_active_;
        peak_lower_active_ = std::max(peak_lower_active_, lower_active_);
    }
    peak_used_bytes_ = std::max(peak_used_bytes_, used_bytes_);
    if (viewer(frame_type))
        ++viewer_admissions_;
    else if (loader(frame_type))
        ++loader_admissions_;
    else
        ++speculative_admissions_;
    return Lease(*this, frame_type, bytes);
}

inline void DataResourceArbiter::release(FrameType frame_type, uint64_t bytes) {
    Lock lock(mutex_);
    ++releases_;
    used_bytes_ -= std::min(used_bytes_, bytes);
    if (!viewer(frame_type)) {
        lower_used_bytes_ -= std::min(lower_used_bytes_, bytes);
        if (lower_active_) --lower_active_;
    }
    cv_.notify_all();
}

inline void DataResourceArbiter::Lease::reset() {
    if (!owner_)
        return;
    owner_->release(frame_type_, bytes_);
    owner_ = nullptr;
    bytes_ = 0;
}

inline void DataResourceArbiter::stop() {
    Lock lock(mutex_);
    stopping_ = true;
    cv_.notify_all();
}

inline DataResourceStats DataResourceArbiter::stats() const {
    Lock lock(mutex_);
    const auto device = service_monitor_ ? service_monitor_->sample() : DiskServiceMonitor::Sample{};
    return {capacity_bytes_,           viewer_reserve_bytes_,
            used_bytes_,               peak_used_bytes_,
            viewer_admissions_,        loader_admissions_,
            speculative_admissions_,   viewer_waits_,
            loader_waits_,             speculative_waits_,
            cancelled_waits_,          background_concurrency_,
            lower_active_,             peak_lower_active_,
            device.pressured,          device.mean_us,
            device.worst_us,           device.slowdown_percent,
            service_monitor_ ? service_monitor_->pressure_onsets() : 0,
            pressure_refusals_};
}

} // namespace macha
