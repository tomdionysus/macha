// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "io_pressure.hpp"
#include "log.hpp"
#include "net.hpp"

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

// Immutable provenance and bounds for DATA-plane work. CONTROL deliberately
// remains outside this pool on its independently reserved transport/executors.
class DataWorkContext {
  public:
    using Clock = std::chrono::steady_clock;

  private:
    FrameType frame_type_{FrameType::loader};
    uint64_t quantum_bytes_{};
    Clock::time_point deadline_{};
    std::atomic_bool* cancelled_{};
    // A no-progress budget, distinct from the absolute deadline above. Work
    // that legitimately takes a long time must not be cancelled for taking it,
    // but work that is not moving at all must eventually fail rather than wait
    // forever: on 2026-09-09 all eight publication workers on es-1 sat in an
    // unbounded acquire holding 492 MB between them, so nothing failed,
    // nothing retried and nothing parked. The counter is shared across the
    // workers of one pipeline, so progress anywhere re-arms the window.
    const std::atomic_uint64_t* progress_{};
    std::chrono::milliseconds no_progress_budget_{};

  public:
    explicit DataWorkContext(FrameType frame_type = FrameType::loader,
                             uint64_t quantum_bytes = 0,
                             Clock::time_point deadline = {},
                             std::atomic_bool* cancelled = nullptr,
                             const std::atomic_uint64_t* progress = nullptr,
                             std::chrono::milliseconds no_progress_budget = {})
        : frame_type_(frame_type), quantum_bytes_(quantum_bytes), deadline_(deadline),
          cancelled_(cancelled), progress_(progress), no_progress_budget_(no_progress_budget) {
        if (frame_type == FrameType::control)
            throw std::invalid_argument("control is not a DATA work class");
    }

    // Present only when the caller supplied both a counter and a budget.
    const std::atomic_uint64_t* progress() const noexcept {
        return no_progress_budget_.count() ? progress_ : nullptr;
    }
    std::chrono::milliseconds no_progress_budget() const noexcept { return no_progress_budget_; }

    FrameType frame_type() const noexcept { return frame_type_; }
    uint64_t quantum_bytes() const noexcept { return quantum_bytes_; }
    Clock::time_point deadline() const noexcept { return deadline_; }
    std::atomic_bool* cancellation() const noexcept { return cancelled_; }
    bool cancelled() const noexcept {
        return cancelled_ && cancelled_->load(std::memory_order_relaxed);
    }
    bool expired(Clock::time_point now = Clock::now()) const noexcept {
        return deadline_ != Clock::time_point{} && now >= deadline_;
    }
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
    // Device pressure, so an operator can see the reason a loader slowed down
    // rather than inferring it. Without this the mechanism is invisible and
    // indistinguishable from the node being mysteriously slow.
    bool device_pressured{};
    uint64_t device_service_us{};
    uint64_t device_worst_us{};
    // The signal pressure is decided on: actual/expected as a percentage,
    // where 100 is a device performing exactly as expected for the work it
    // was given. Reported because a mean latency alone cannot be judged
    // without knowing the size of the operations behind it.
    uint64_t device_slowdown_percent{};
    uint64_t device_pressure_onsets{};
    // How much work this mechanism actually turned away. Onsets say the device
    // went under; this says what it cost. Without both, an operator looking at
    // a slow node cannot tell a loader being deliberately held back from a
    // node that is simply unwell, which is exactly the question that could not
    // be answered during the 2026-09-22 incident. The counter existed as a
    // private member from the day the gate shipped and was never incremented
    // and never reported.
    uint64_t pressure_refusals{};
};

// Event-driven byte admission at blocking DATA resource boundaries. Lower
// classes may borrow all non-reserved capacity, but can never consume the
// viewer headroom. A waiting viewer also closes lower-class admission until it
// has acquired its bounded credit. CONTROL does not enter this object.
//
// Bytes are not the only contended resource, and on 2026-09-19 they were not
// the one that broke: every byte budget here was satisfied while a viewer's
// read sat behind a 17-second extent write on the same spindle. So admission
// also consults measured device service time, and refuses loader and
// speculative work while the disk it would use is slow. A viewer is never
// refused for pressure -- if the device is slow, the person waiting on it gets
// all of it.
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
    // Measured device service time, or null where there is no device to
    // measure (tests, and any arbiter not fronting a store).
    const DiskServiceMonitor* service_monitor_{};
    // Law 3: the loader is bounded, never stopped. Under pressure this many
    // background leases are still admitted, so publication and repair make
    // progress at a trickle instead of deadlocking behind a disk that is busy
    // because of them.
    uint64_t min_background_under_pressure_{1};
    uint64_t pressure_refusals_{};
    // Law 3 asks whether a viewer is *present*, not whether one happens to be
    // holding byte credit at this instant. Playback is bursty: between two
    // extents a viewer holds nothing, so deciding on credit alone readmitted
    // the loader at full concurrency in every gap and a viewer's next read
    // queued behind the extent write that gap had just let in. The rest of the
    // system already answers this question with an activity clock and
    // maintenance.foreground_quiet; this is how the arbiter reads the same
    // answer without taking a dependency on the node. Two relaxed atomic loads
    // and a clock read, called under the arbiter mutex and never re-entering
    // it. Null where there is no node (tests), which leaves the credit test
    // below as the whole answer, exactly as it was.
    std::function<bool()> viewer_recently_active_;
    // Background effort ceiling: how many loader/speculative leases may be
    // active at once. Each lease is one extent's worth of hashing,
    // encryption and transfer, so this bounds the CPU that publication and
    // repair can take between them; viewers are never counted. 0 = no limit.
    uint64_t background_concurrency_{};
    // A wait with no caller deadline fails after this long without a single
    // release anywhere in the arbiter. Zero waits for ever, which is what this
    // did unconditionally before -- and which turned a caller holding credit
    // while acquiring more into a silent permanent hang.
    std::chrono::milliseconds no_progress_deadline_{};
    uint64_t releases_{};
    uint64_t no_progress_failures_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t used_bytes_{};
    uint64_t lower_used_bytes_{};
    uint64_t lower_active_{};
    uint64_t peak_lower_active_{};
    uint64_t peak_used_bytes_{};
    uint64_t waiting_viewers_{};
    uint64_t waiting_loaders_{};
    uint64_t waiting_speculative_{};
    bool stopping_{};
    uint64_t viewer_admissions_{};
    uint64_t loader_admissions_{};
    uint64_t speculative_admissions_{};
    uint64_t viewer_waits_{};
    uint64_t loader_waits_{};
    uint64_t speculative_waits_{};
    uint64_t cancelled_waits_{};

    static bool viewer(FrameType frame_type) noexcept {
        return frame_type == FrameType::foreground || frame_type == FrameType::read_ahead;
    }
    static bool loader(FrameType frame_type) noexcept {
        return frame_type == FrameType::loader;
    }
    uint64_t charge(uint64_t bytes) const noexcept { return std::max<uint64_t>(1, bytes); }
    // `refused_for_pressure`, when given, says whether a false answer was this
    // mechanism's doing rather than an ordinary byte or concurrency bound. The
    // callers count it; counting here would re-count every condition-variable
    // wakeup of a single waiter and produce a number that means nothing.
    bool available(FrameType frame_type, uint64_t bytes,
                   bool* refused_for_pressure = nullptr) const;
    void release(FrameType frame_type, uint64_t bytes);

  public:
    DataResourceArbiter(uint64_t capacity_bytes, uint64_t viewer_reserve_bytes,
                        uint64_t background_concurrency = 0,
                        std::chrono::milliseconds no_progress_deadline = {});
    // Set once during node construction, before any work is admitted.
    void observe_device(const DiskServiceMonitor* monitor,
                        uint64_t min_background_under_pressure) {
        std::lock_guard lock(mutex_);
        service_monitor_ = monitor;
        min_background_under_pressure_ = std::max<uint64_t>(1, min_background_under_pressure);
    }
    // Set once during node construction, before any work is admitted.
    void observe_viewers(std::function<bool()> recently_active) {
        std::lock_guard lock(mutex_);
        viewer_recently_active_ = std::move(recently_active);
    }
    std::optional<Lease> acquire(const DataWorkContext& context, uint64_t bytes);
    std::optional<Lease> try_acquire(const DataWorkContext& context, uint64_t bytes);
    void stop();
    DataResourceStats stats() const;
};

inline DataResourceArbiter::DataResourceArbiter(uint64_t capacity_bytes,
                                                uint64_t viewer_reserve_bytes,
                                                uint64_t background_concurrency,
                                                std::chrono::milliseconds no_progress_deadline)
    : capacity_bytes_(capacity_bytes), viewer_reserve_bytes_(viewer_reserve_bytes),
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
    // Law 3: the loader yields only when it would otherwise make a viewer
    // wait. A slow device with nobody reading from it is a device doing its
    // job, and holding an operator's import back for it is exactly the
    // violation the law names -- it cost a 36 GB import an afternoon at 2 MB/s
    // on 2026-09-22 while nothing was being watched.
    //
    // Speculative work has no such protection: it sits below the loader, and
    // pressure alone is reason enough for it to stand aside.
    //
    // Ordered so the viewer-presence question is only asked when the answer can
    // change anything. The monitor's own cost discipline applies here too: this
    // runs on the admission path of every extent in the system, and a device
    // that is coping must not pay a clock read to be told so.
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
    // An operation larger than its entire class budget can never be admitted.
    // Fail it immediately instead of creating an immortal event-driven waiter.
    if (bytes > class_capacity)
        return {};
    std::unique_lock lock(mutex_);
    bool counted_wait = false;
    auto& waiters = viewer(frame_type)   ? waiting_viewers_
                    : loader(frame_type) ? waiting_loaders_
                                         : waiting_speculative_;
    bool refused_for_pressure = false;
    auto wait_predicate = [&] {
        return stopping_ || context.cancelled() || context.expired() ||
               available(frame_type, bytes, &refused_for_pressure);
    };
    while (!wait_predicate()) {
        if (!counted_wait) {
            counted_wait = true;
            // Once per waiter, not once per wakeup: this counts units of work
            // the gate turned away, which is what an operator needs beside the
            // onset count to tell a deliberate throttle from an unwell node.
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
            cv_.wait_until(lock, context.deadline(), wait_predicate);
        } else if (no_progress_deadline_ == std::chrono::milliseconds{}) {
            cv_.wait(lock, wait_predicate);
        } else {
            // Wait in no-progress windows rather than for ever. Any release
            // anywhere resets the window, so genuine contention -- where work
            // is flowing and this waiter simply has not reached the front --
            // waits as long as it takes. Only a wholly stalled arbiter, where
            // nothing was released for the entire window, gives up.
            const auto seen = releases_;
            if (!cv_.wait_for(lock, no_progress_deadline_,
                              [&] { return wait_predicate() || releases_ != seen; })) {
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    stopping_ = true;
    cv_.notify_all();
}

inline DataResourceStats DataResourceArbiter::stats() const {
    std::lock_guard lock(mutex_);
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
