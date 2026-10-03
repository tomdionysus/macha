// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace macha {

// Admission for a held media request: acquired before waiting, released after.
// A held request is parked as an HttpDeferral (an fd and a small struct), so
// the limits bound fairness and memory across sessions, not threads.
class SegmentHoldArbiter {
  public:
    // Which limit a refused request met.
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

        // Idempotent; also runs from the destructor, so an exception releases it.
        void reset() {
            if (owner_) owner_->release(session_);
            owner_ = nullptr;
        }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };

    SegmentHoldArbiter(size_t max_session_holds, size_t max_concurrent_holds)
        : max_session_(max_session_holds), max_total_(max_concurrent_holds) {}

    void reconfigure(size_t max_session_holds, size_t max_concurrent_holds) {
        Lock lock(mutex_);
        max_session_ = max_session_holds;
        max_total_ = max_concurrent_holds;
    }

    // Never waits: a request the node cannot afford to hold is refused now,
    // not queued behind the ones already held.
    std::optional<Hold> try_acquire(std::string_view session, Refusal* why = nullptr) {
        Lock lock(mutex_);
        std::string key(session);
        auto it = per_session_.find(key);
        const size_t held = it == per_session_.end() ? 0 : it->second;
        // Per-session first, so a deeply prefetching player is refused for
        // exceeding its own share rather than reported as exhausting the node.
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
        Lock lock(mutex_);
        return total_;
    }

    size_t outstanding(std::string_view session) const {
        Lock lock(mutex_);
        auto it = per_session_.find(std::string(session));
        return it == per_session_.end() ? 0 : it->second;
    }

  private:
    void release(const std::string& session) {
        Lock lock(mutex_);
        auto it = per_session_.find(session);
        if (it != per_session_.end() && it->second > 0 && --it->second == 0)
            per_session_.erase(it);
        if (total_ > 0) --total_;
    }

    mutable Mutex mutex_;
    size_t max_session_ MACHA_GUARDED_BY(mutex_){};
    size_t max_total_ MACHA_GUARDED_BY(mutex_){};
    size_t total_ MACHA_GUARDED_BY(mutex_){};
    std::map<std::string, size_t, std::less<>> per_session_ MACHA_GUARDED_BY(mutex_);
};

} // namespace macha
