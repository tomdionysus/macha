// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "log.hpp"
#include "net.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
    bool records_activity() const noexcept {
        return frame_type_ == FrameType::foreground || frame_type_ == FrameType::read_ahead;
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
};

// Event-driven byte admission at blocking DATA resource boundaries. Lower
// classes may borrow all non-reserved capacity, but can never consume the
// viewer headroom. A waiting viewer also closes lower-class admission until it
// has acquired its bounded credit. CONTROL does not enter this object.
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
    bool available(FrameType frame_type, uint64_t bytes) const noexcept;
    void release(FrameType frame_type, uint64_t bytes);

  public:
    DataResourceArbiter(uint64_t capacity_bytes, uint64_t viewer_reserve_bytes,
                        uint64_t background_concurrency = 0,
                        std::chrono::milliseconds no_progress_deadline = {});
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

inline bool DataResourceArbiter::available(FrameType frame_type, uint64_t bytes) const noexcept {
    if (used_bytes_ > capacity_bytes_ - bytes)
        return false;
    if (viewer(frame_type))
        return true;
    if (waiting_viewers_)
        return false;
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
    auto wait_predicate = [&] {
        return stopping_ || context.cancelled() || context.expired() ||
               available(frame_type, bytes);
    };
    while (!wait_predicate()) {
        if (!counted_wait) {
            counted_wait = true;
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
    if (stopping_ || !available(frame_type, bytes))
        return {};
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
    return {capacity_bytes_, viewer_reserve_bytes_, used_bytes_, peak_used_bytes_,
            viewer_admissions_, loader_admissions_, speculative_admissions_,
            viewer_waits_, loader_waits_, speculative_waits_, cancelled_waits_,
            background_concurrency_, lower_active_, peak_lower_active_};
}

} // namespace macha
