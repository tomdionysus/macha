// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include <cstddef>
#include <optional>

namespace macha {

// Password hashing (scrypt) admitted up to a limit, for every caller alike: a
// sign-in's check and a new or changed password. None free is a refusal,
// never a wait, so hashing can never hold every control worker. The limit is
// session.max_concurrent_password_checks, which the configuration keeps
// below the control workers. Thread safe.
class PasswordWork {
  public:
    explicit PasswordWork(size_t limit) noexcept : limit_(limit) {}
    PasswordWork(const PasswordWork&) = delete;
    PasswordWork& operator=(const PasswordWork&) = delete;

    // Held while hashing; released when it goes.
    class Slot {
      public:
        Slot(Slot&& other) noexcept : work_(other.work_) { other.work_ = nullptr; }
        Slot& operator=(Slot&&) = delete;
        ~Slot() {
            if (work_)
                work_->release();
        }

      private:
        friend class PasswordWork;
        explicit Slot(PasswordWork* work) noexcept : work_(work) {}
        PasswordWork* work_;
    };

    std::optional<Slot> try_acquire() {
        Lock lock(mutex_);
        if (in_flight_ >= limit_)
            return std::nullopt;
        ++in_flight_;
        return Slot(this);
    }

  private:
    void release() {
        Lock lock(mutex_);
        if (in_flight_)
            --in_flight_;
    }

    const size_t limit_;
    Mutex mutex_;
    size_t in_flight_ MACHA_GUARDED_BY(mutex_){};
};

} // namespace macha
