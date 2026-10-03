// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>

// Clang's thread-safety analysis (object ledger spec, A1 and A2): checked by
// Clang with -Wthread-safety as errors, ignored by GCC. Every lock is one of
// the types below, so every lock is checked.
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
#define MACHA_ACQUIRE_SHARED(...) MACHA_THREAD_ANNOTATION(acquire_shared_capability(__VA_ARGS__))
#define MACHA_RELEASE_SHARED(...) MACHA_THREAD_ANNOTATION(release_shared_capability(__VA_ARGS__))
#define MACHA_RELEASE_GENERIC(...) MACHA_THREAD_ANNOTATION(release_generic_capability(__VA_ARGS__))
#define MACHA_TRY_ACQUIRE(...) MACHA_THREAD_ANNOTATION(try_acquire_capability(__VA_ARGS__))
#define MACHA_REQUIRES(...) MACHA_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define MACHA_REQUIRES_SHARED(...) MACHA_THREAD_ANNOTATION(requires_shared_capability(__VA_ARGS__))
#define MACHA_GUARDED_BY(x) MACHA_THREAD_ANNOTATION(guarded_by(x))
#define MACHA_PT_GUARDED_BY(x) MACHA_THREAD_ANNOTATION(pt_guarded_by(x))
#define MACHA_ACQUIRED_BEFORE(...) MACHA_THREAD_ANNOTATION(acquired_before(__VA_ARGS__))
#define MACHA_ACQUIRED_AFTER(...) MACHA_THREAD_ANNOTATION(acquired_after(__VA_ARGS__))
#define MACHA_RETURN_CAPABILITY(x) MACHA_THREAD_ANNOTATION(lock_returned(x))
#define MACHA_NO_THREAD_SAFETY_ANALYSIS MACHA_THREAD_ANNOTATION(no_thread_safety_analysis)

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

// A mutex never held across I/O: holders do memory work only.
class MACHA_CAPABILITY("mutex") Mutex {
    std::mutex mutex_;

  public:
    void lock() MACHA_ACQUIRE() { mutex_.lock(); }
    void unlock() MACHA_RELEASE() { mutex_.unlock(); }
    bool try_lock() MACHA_TRY_ACQUIRE(true) { return mutex_.try_lock(); }
    std::mutex& native() noexcept { return mutex_; }
};

// A mutex some holder keeps across device or network I/O, so taking it is
// waiting on that I/O: a no-I/O region may not.
class MACHA_CAPABILITY("I/O mutex") IoMutex {
    std::mutex mutex_;

  public:
    void lock() MACHA_ACQUIRE() MACHA_EXCLUDES(no_io) { mutex_.lock(); }
    void unlock() MACHA_RELEASE() { mutex_.unlock(); }
    std::mutex& native() noexcept { return mutex_; }
};

// A reader-writer mutex never held across I/O.
class MACHA_CAPABILITY("shared mutex") SharedMutex {
    std::shared_mutex mutex_;

  public:
    void lock() MACHA_ACQUIRE() { mutex_.lock(); }
    void unlock() MACHA_RELEASE() { mutex_.unlock(); }
    void lock_shared() MACHA_ACQUIRE_SHARED() { mutex_.lock_shared(); }
    void unlock_shared() MACHA_RELEASE_SHARED() { mutex_.unlock_shared(); }
    std::shared_mutex& native() noexcept { return mutex_; }
};

// A reader-writer mutex some holder keeps across I/O.
class MACHA_CAPABILITY("I/O shared mutex") IoSharedMutex {
    std::shared_mutex mutex_;

  public:
    void lock() MACHA_ACQUIRE() MACHA_EXCLUDES(no_io) { mutex_.lock(); }
    void unlock() MACHA_RELEASE() { mutex_.unlock(); }
    void lock_shared() MACHA_ACQUIRE_SHARED() MACHA_EXCLUDES(no_io) { mutex_.lock_shared(); }
    void unlock_shared() MACHA_RELEASE_SHARED() { mutex_.unlock_shared(); }
    std::shared_mutex& native() noexcept { return mutex_; }
};

// Holds a Mutex or IoMutex for its scope; may be released and retaken, and
// lends its std::unique_lock to a condition variable's wait (which releases
// and retakes it inside the wait, invisibly to the analysis).
class MACHA_SCOPED_CAPABILITY Lock {
    std::unique_lock<std::mutex> lock_;

  public:
    explicit Lock(Mutex& mutex) MACHA_ACQUIRE(mutex) : lock_(mutex.native()) {}
    explicit Lock(IoMutex& mutex) MACHA_ACQUIRE(mutex) MACHA_EXCLUDES(no_io)
        : lock_(mutex.native()) {}
    ~Lock() MACHA_RELEASE() {}
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void lock() MACHA_ACQUIRE() { lock_.lock(); }
    void unlock() MACHA_RELEASE() { lock_.unlock(); }
    bool owns_lock() const noexcept { return lock_.owns_lock(); }
    std::unique_lock<std::mutex>& native() noexcept { return lock_; }
};

// Holds a SharedMutex or IoSharedMutex exclusively for its scope.
class MACHA_SCOPED_CAPABILITY WriteLock {
    std::unique_lock<std::shared_mutex> lock_;

  public:
    explicit WriteLock(SharedMutex& mutex) MACHA_ACQUIRE(mutex) : lock_(mutex.native()) {}
    explicit WriteLock(IoSharedMutex& mutex) MACHA_ACQUIRE(mutex) MACHA_EXCLUDES(no_io)
        : lock_(mutex.native()) {}
    ~WriteLock() MACHA_RELEASE() {}
    WriteLock(const WriteLock&) = delete;
    WriteLock& operator=(const WriteLock&) = delete;
};

// Holds a SharedMutex or IoSharedMutex shared for its scope.
class MACHA_SCOPED_CAPABILITY ReadLock {
    std::shared_lock<std::shared_mutex> lock_;

  public:
    explicit ReadLock(SharedMutex& mutex) MACHA_ACQUIRE_SHARED(mutex) : lock_(mutex.native()) {}
    explicit ReadLock(IoSharedMutex& mutex) MACHA_ACQUIRE_SHARED(mutex) MACHA_EXCLUDES(no_io)
        : lock_(mutex.native()) {}
    ~ReadLock() MACHA_RELEASE() {}
    ReadLock(const ReadLock&) = delete;
    ReadLock& operator=(const ReadLock&) = delete;
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
