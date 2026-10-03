// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include "contract/thread_safety.hpp"
#include <mutex>
#include <stdexcept>
#include <utility>

// Published immutable state (object ledger spec, A2): an immutable snapshot
// behind an owning pointer that a publish swaps. A publish never changes what
// a held handle reads; the old snapshot lives until its last holder lets go.
namespace macha {

template <class T> class Published {
  public:
    using Handle = std::shared_ptr<const T>;

    Published() = default;
    explicit Published(Handle initial) { publish(std::move(initial)); }
    Published(const Published&) = delete;
    Published& operator=(const Published&) = delete;

    // The current snapshot, or null before the first publish. Thread-safe;
    // waits only on the pointer copy (a publish holds the lock for one swap).
    Handle handle() const {
        Lock lock(mutex_);
        return current_;
    }

    // Replaces the current snapshot; a published state is never withdrawn.
    // Thread-safe. The previous snapshot, if this held its last reference, is
    // destroyed after the lock is released, so a reader never waits on a
    // destructor.
    void publish(Handle next) {
        if (!next)
            throw std::invalid_argument("Published::publish: a snapshot is required");
        {
            Lock lock(mutex_);
            current_.swap(next);
        }
    }
    void publish(T value) { publish(std::make_shared<const T>(std::move(value))); }

  private:
    mutable Mutex mutex_;
    Handle current_ MACHA_GUARDED_BY(mutex_);
};

} // namespace macha
