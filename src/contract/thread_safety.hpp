// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <mutex>

// Clang's thread-safety analysis (object ledger spec, A1 and A2): checked by
// Clang with -Wthread-safety (warnings are errors), ignored by GCC. Only
// annotated code is checked.
#if defined(__clang__)
#define MACHA_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define MACHA_THREAD_ANNOTATION(x)
#endif

#define MACHA_CAPABILITY(x) MACHA_THREAD_ANNOTATION(capability(x))
#define MACHA_SCOPED_CAPABILITY MACHA_THREAD_ANNOTATION(scoped_lockable)
#define MACHA_ACQUIRE(...) MACHA_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define MACHA_RELEASE(...) MACHA_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define MACHA_EXCLUDES(...) MACHA_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))

namespace macha {

// A role, not a lock: "this code waits on no I/O". An operation declared to
// wait on nothing holds a NoIoRegion for its body; whatever waits on I/O
// (such as a lock a writer holds across its I/O) is MACHA_EXCLUDES(no_io), so
// reaching it inside the region is a Clang compile error. One object suffices:
// the analysis is per function and no runtime state hangs on it.
class MACHA_CAPABILITY("no-I/O region") NoIo {};
extern NoIo no_io;

class MACHA_SCOPED_CAPABILITY NoIoRegion {
  public:
    NoIoRegion() MACHA_ACQUIRE(no_io) {}
    ~NoIoRegion() MACHA_RELEASE() {}
    NoIoRegion(const NoIoRegion&) = delete;
    NoIoRegion& operator=(const NoIoRegion&) = delete;
};

// A per-object mutex a writer holds across its device I/O (LocalStore's
// object_mutex), so waiting for it is waiting on that I/O. Keeps the mutex
// alive and held for the guard's lifetime.
class ObjectLock {
    std::shared_ptr<std::mutex> mutex_;
    std::lock_guard<std::mutex> guard_;

  public:
    explicit ObjectLock(std::shared_ptr<std::mutex> mutex) MACHA_EXCLUDES(no_io)
        : mutex_(std::move(mutex)), guard_(*mutex_) {}
    ~ObjectLock() = default;
    ObjectLock(const ObjectLock&) = delete;
    ObjectLock& operator=(const ObjectLock&) = delete;
};

} // namespace macha
