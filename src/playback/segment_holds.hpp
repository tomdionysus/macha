// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace macha {

// Admission for a held media request.
//
// A hold is an explicitly admitted resource: acquired before waiting, released
// after, and never an implicit consequence of a thread happening to block.
// That distinction is what let the wait change underneath it. Until 0.43.0
// HttpServer ran a fixed worker pool shared by every route, a held request
// occupied one worker for the whole of its wait, and this budget existed to
// keep one deeply prefetching player from taking all sixteen and silencing
// Status. The server now parks a held request as a continuation
// (HttpDeferral) that costs an fd and a small struct; the policy here is
// exactly what it was, and the budget is a fairness and memory bound across
// sessions rather than a thread-protection one. Nothing here assumes a hold
// costs a thread, and nothing should.
class SegmentHoldArbiter {
  public:
    // Why a request was refused, so the refusal can say which limit it met
    // rather than only that it met one.
    enum class Refusal { session_limit, budget_exhausted };

    class Hold {
        friend class SegmentHoldArbiter;
        SegmentHoldArbiter* owner_{};
        std::string session_;

        Hold(SegmentHoldArbiter& owner, std::string session)
            : owner_(&owner), session_(std::move(session)) {}

      public:
        Hold() = default;
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        Hold(Hold&& other) noexcept
            : owner_(other.owner_), session_(std::move(other.session_)) {
            other.owner_ = nullptr;
        }
        Hold& operator=(Hold&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = other.owner_;
                session_ = std::move(other.session_);
                other.owner_ = nullptr;
            }
            return *this;
        }
        ~Hold() { reset(); }

        // Releasing is explicit and idempotent. A hold that is never released
        // is a leaked worker, so this runs from the destructor as well --
        // including when the wait ends by exception.
        void reset() {
            if (owner_) owner_->release(session_);
            owner_ = nullptr;
        }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };

    SegmentHoldArbiter(size_t max_session_holds, size_t max_concurrent_holds)
        : max_session_(max_session_holds), max_total_(max_concurrent_holds) {}

    void reconfigure(size_t max_session_holds, size_t max_concurrent_holds) {
        std::lock_guard lock(mutex_);
        max_session_ = max_session_holds;
        max_total_ = max_concurrent_holds;
    }

    // Never waits. A refusal is immediate by construction: the point of the
    // limits is that a request the node cannot afford to hold is answered now,
    // rather than queued behind the ones it is already holding.
    std::optional<Hold> try_acquire(std::string_view session, Refusal* why = nullptr) {
        std::lock_guard lock(mutex_);
        std::string key(session);
        auto it = per_session_.find(key);
        const size_t held = it == per_session_.end() ? 0 : it->second;
        // Per-session first, so a single deeply prefetching player is refused
        // for exceeding its own share rather than reported as having exhausted
        // the node -- the 0.32.14 scenario, where a native player queued thirty
        // requests and waited on the encoder for each of them in turn.
        if (max_session_ == 0 || held >= max_session_) {
            if (why) *why = Refusal::session_limit;
            return std::nullopt;
        }
        if (max_total_ == 0 || total_ >= max_total_) {
            if (why) *why = Refusal::budget_exhausted;
            return std::nullopt;
        }
        per_session_[key] = held + 1;
        ++total_;
        return Hold(*this, std::move(key));
    }

    size_t outstanding() const {
        std::lock_guard lock(mutex_);
        return total_;
    }

    size_t outstanding(std::string_view session) const {
        std::lock_guard lock(mutex_);
        auto it = per_session_.find(std::string(session));
        return it == per_session_.end() ? 0 : it->second;
    }

  private:
    void release(const std::string& session) {
        std::lock_guard lock(mutex_);
        auto it = per_session_.find(session);
        if (it != per_session_.end() && it->second > 0 && --it->second == 0)
            per_session_.erase(it);
        if (total_ > 0) --total_;
    }

    mutable std::mutex mutex_;
    size_t max_session_{};
    size_t max_total_{};
    size_t total_{};
    std::map<std::string, size_t, std::less<>> per_session_;
};

} // namespace macha
