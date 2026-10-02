// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <deque>
#include <optional>

namespace macha {

// One retry discipline for every unit of work that can fail and try again:
// exponential backoff with a ceiling, a failure budget over a window, and a
// terminal "parked" outcome that hands the item to an operator instead of
// retrying forever. The policy is data; the state is per work item.
struct RetryPolicy {
    // More than this many failures inside `failure_window` parks the item. Alone it
    // never parks at the backoff ceiling when failure_window / max_backoff is lower.
    size_t max_failures_in_window{5};
    std::chrono::milliseconds failure_window{std::chrono::minutes(10)};
    std::chrono::milliseconds initial_backoff{std::chrono::seconds(1)};
    std::chrono::milliseconds max_backoff{std::chrono::seconds(60)};
    // Backstop for what the density rule cannot see: an item failing unbroken for
    // this long parks however sparsely retried. Separate from failure_window so a
    // long node or WAN outage does not park every item. 0 disables it.
    std::chrono::milliseconds max_failing_duration{std::chrono::hours(1)};
};

class RetryState {
  public:
    using Clock = std::chrono::steady_clock;

    // Records a failure. Returns the delay before the next attempt, or
    // nullopt when the budget is exhausted and the item must be parked.
    std::optional<std::chrono::milliseconds> failed(const RetryPolicy& policy,
                                                    Clock::time_point now = Clock::now()) {
        if (consecutive_ == 0)
            failing_since_ = now;
        ++consecutive_;
        ++total_;
        if (first_failure_ == Clock::time_point{})
            first_failure_ = now;
        last_failure_ = now;
        recent_.push_back(now);
        while (!recent_.empty() && now - recent_.front() > policy.failure_window)
            recent_.pop_front();
        if (recent_.size() > policy.max_failures_in_window)
            return std::nullopt;
        // Unbroken failure past the backstop, timed from the current run's start.
        if (policy.max_failing_duration.count() > 0 &&
            now - failing_since_ > policy.max_failing_duration)
            return std::nullopt;
        if (backoff_ == std::chrono::milliseconds{})
            backoff_ = policy.initial_backoff;
        else
            backoff_ = std::min(policy.max_backoff, backoff_ * 2);
        due_ = now + backoff_;
        return backoff_;
    }

    // A clean attempt resets the backoff and the consecutive count; the
    // window is kept so a flapping item still parks.
    void succeeded() {
        consecutive_ = 0;
        failing_since_ = {};
        backoff_ = {};
        due_ = {};
    }

    // Operator action: forget everything and allow an immediate attempt.
    void reset() {
        *this = RetryState{};
    }

    bool due(Clock::time_point now = Clock::now()) const noexcept {
        return now >= due_;
    }
    Clock::time_point due_at() const noexcept { return due_; }
    size_t consecutive_failures() const noexcept { return consecutive_; }
    size_t total_failures() const noexcept { return total_; }
    size_t failures_in_window() const noexcept { return recent_.size(); }
    Clock::time_point first_failure() const noexcept { return first_failure_; }
    Clock::time_point last_failure() const noexcept { return last_failure_; }
    // Start of the current unbroken run of failures; unset when not failing.
    Clock::time_point failing_since() const noexcept { return failing_since_; }
    std::chrono::milliseconds current_backoff() const noexcept { return backoff_; }

  private:
    std::deque<Clock::time_point> recent_;
    std::chrono::milliseconds backoff_{};
    Clock::time_point due_{};
    Clock::time_point first_failure_{};
    Clock::time_point last_failure_{};
    Clock::time_point failing_since_{};
    size_t consecutive_{};
    size_t total_{};
};

} // namespace macha
