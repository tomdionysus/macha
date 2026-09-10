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
    //
    // This density rule alone is unreachable once backoff reaches its ceiling:
    // a window only ever holds failure_window/max_backoff attempts, so any
    // policy where that quotient is below max_failures_in_window can never
    // park at the ceiling and retries forever. The shipped publication policy
    // was exactly that (30 min / 30 s = 60 attempts against a threshold of
    // 100), and on 2026-09-10 gbni-1 retried one inode 68 times and counting,
    // at DEBUG, with `parked_publications` reading 0.
    size_t max_failures_in_window{5};
    std::chrono::milliseconds failure_window{std::chrono::minutes(10)};
    std::chrono::milliseconds initial_backoff{std::chrono::seconds(1)};
    std::chrono::milliseconds max_backoff{std::chrono::seconds(60)};
    // Backstop for the case the density rule cannot see: an item that has not
    // succeeded once for this long parks however sparsely it is retried. Held
    // separately from failure_window because the two answer different
    // questions -- that one is "is this flapping?", this one is "is this ever
    // going to work?" -- and because tying them together would park every
    // publication on this cluster whenever the wireless node or the WAN link
    // is out for longer than the flap window. 0 disables it.
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
        // Unbroken failure for longer than the backstop. Measured from the
        // start of the current run rather than first_failure_, so an item that
        // has succeeded since is judged on its current run, not its history.
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
