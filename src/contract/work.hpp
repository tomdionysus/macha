// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

// Who is asking and what an operation may wait on (object ledger spec, A1).
// Primitives: no I/O.
namespace macha {

// What an operation may wait on, declared on its contract, and who may call
// it: Dannan's, the one definition. `locks` means it takes a lock something
// holds across I/O, which makes that I/O its wait too. Control never waits on
// the DATA device or the network (law 1); every other class on anything.
using dannan::sched::default_waits;
using dannan::sched::includes;
using dannan::sched::local_waits;
using dannan::sched::may_enter;
using dannan::sched::ThreadSafety;
using dannan::sched::Waits;

// Who is asking, what class of work it is, what it may wait on, for how
// long, and whether they have given up. Covers every class; DataWorkContext
// is the DATA specialisation.
class WorkContext {
  public:
    using Clock = std::chrono::steady_clock;

    // `origin` names who is asking ("GET /api/v1/catalogue/items"): a
    // string with static storage, reported by the wait guard. `allowed`
    // narrows what the work may wait on (a read that must answer from
    // memory allows no network wait); it never widens the class's default.
    explicit WorkContext(FrameType frame_type = FrameType::loader, Clock::time_point deadline = {},
                         std::atomic_bool* cancelled = nullptr, const char* origin = "unnamed",
                         std::optional<Waits> allowed = std::nullopt) noexcept
        : frame_type_(frame_type), deadline_(deadline), cancelled_(cancelled), origin_(origin),
          allowed_(static_cast<Waits>(
              static_cast<uint8_t>(default_waits(work_class(frame_type))) &
              static_cast<uint8_t>(allowed.value_or(default_waits(work_class(frame_type)))))) {}

    FrameType frame_type() const noexcept {
        return frame_type_;
    }
    WorkClass work() const noexcept {
        return work_class(frame_type_);
    }
    Waits allowed() const noexcept {
        return allowed_;
    }
    const char* origin() const noexcept {
        return origin_;
    }
    Clock::time_point deadline() const noexcept {
        return deadline_;
    }
    std::atomic_bool* cancellation() const noexcept {
        return cancelled_;
    }
    bool cancelled() const noexcept {
        return cancelled_ && cancelled_->load(std::memory_order_relaxed);
    }
    // A default deadline (the epoch) means none.
    bool expired(Clock::time_point now = Clock::now()) const noexcept {
        return deadline_ != Clock::time_point{} && now >= deadline_;
    }

  private:
    FrameType frame_type_;
    Clock::time_point deadline_;
    std::atomic_bool* cancelled_;
    const char* origin_;
    Waits allowed_;
};

// The runtime boundary check, called by each operation that declares a
// device or network wait. A refusal is counted and logged once per (origin,
// operation); tests switch the guard to throw.
class WaitGuard {
  public:
    enum class Mode { record, throw_on_violation };
    static void set_mode(Mode) noexcept;
    static Mode mode() noexcept;
    // Refusals since start.
    static uint64_t violations() noexcept;
    // Whether the entry was allowed; throws std::logic_error on a refusal in
    // throw mode.
    static bool enter(const WorkContext&, Waits declared, std::string_view operation);
};

} // namespace macha
