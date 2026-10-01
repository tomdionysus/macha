// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

// Published immutable state (the object ledger spec, A2): state that is
// rebuilt and read concurrently, held as an immutable snapshot behind an
// owning pointer that a publish swaps. A reader takes a handle and keeps it
// for as long as it needs; a publish never changes what a held handle
// reads; the old snapshot lives until its last holder lets go.
namespace macha {

template <class T> class Published {
  public:
    using Handle = std::shared_ptr<const T>;

    // Nothing is published until the first publish(): handle() is null.
    Published() = default;
    explicit Published(Handle initial) { publish(std::move(initial)); }
    Published(const Published&) = delete;
    Published& operator=(const Published&) = delete;

    // The current snapshot, or null before the first publish. Thread-safe;
    // waits on nothing but the pointer copy (no publish holds the lock for
    // longer than a swap).
    Handle handle() const {
        std::lock_guard lock(mutex_);
        return current_;
    }

    // Replaces the current snapshot. Requires a snapshot: a published state
    // is never withdrawn. Thread-safe. When this held the last reference to
    // the previous snapshot, the previous snapshot is destroyed here, after
    // the lock is released, so a reader never waits on a destructor;
    // otherwise its last holder destroys it.
    void publish(Handle next) {
        if (!next)
            throw std::invalid_argument("Published::publish: a snapshot is required");
        {
            std::lock_guard lock(mutex_);
            current_.swap(next);
        }
        // `next` now holds the previous snapshot and is released here.
    }
    void publish(T value) { publish(std::make_shared<const T>(std::move(value))); }

  private:
    mutable std::mutex mutex_;
    Handle current_;
};

} // namespace macha
