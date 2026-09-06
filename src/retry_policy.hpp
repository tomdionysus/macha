// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <deque>
#include <optional>

namespace macha {

// One retry discipline for every unit of work that can fail and try again:
// exponential backoff with a ceiling, a failure budget over a window, and a
// terminal "parked" outcome that hands the item to an operator instead of
// retrying forever. Discipline 2 of TODO/2026-09-06-self-healing-disciplines-
// plan.md: before it, three separate loops retried at fixed intervals with
// no ceiling (a doomed inode at ~35/s for hours), the RPC layer waited
// without deadline, and the only circuit breaker was the startup timeout.
//
// The policy is data; the state is per work item. Production defaults are
// conservative; tests override them to keep fault injection fast.
struct RetryPolicy {
    // More than this many failures inside `failure_window` parks the item.
    size_t max_failures_in_window{5};
    std::chrono::milliseconds failure_window{std::chrono::minutes(10)};
    std::chrono::milliseconds initial_backoff{std::chrono::seconds(1)};
    std::chrono::milliseconds max_backoff{std::chrono::seconds(60)};
};

class RetryState {
  public:
    using Clock = std::chrono::steady_clock;

    // Records a failure. Returns the delay before the next attempt, or
    // nullopt when the budget is exhausted and the item must be parked.
    std::optional<std::chrono::milliseconds> failed(const RetryPolicy& policy,
                                                    Clock::time_point now = Clock::now()) {
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
    std::chrono::milliseconds current_backoff() const noexcept { return backoff_; }

  private:
    std::deque<Clock::time_point> recent_;
    std::chrono::milliseconds backoff_{};
    Clock::time_point due_{};
    Clock::time_point first_failure_{};
    Clock::time_point last_failure_{};
    size_t consecutive_{};
    size_t total_{};
};

} // namespace macha
