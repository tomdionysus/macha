// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

// The vocabulary every contract uses to say who is asking and what an
// operation may wait on (the object ledger spec, A1). Primitives: no I/O,
// tested exhaustively.
namespace macha {

// Who is asking, for how long, and whether they have given up. Every class,
// control included; DataWorkContext is the DATA specialisation.
class WorkContext {
  public:
    using Clock = std::chrono::steady_clock;

    // `origin` names who is asking ("GET /api/v1/catalogue/items"): a
    // string with static storage, reported by the wait guard.
    explicit WorkContext(FrameType frame_type = FrameType::loader, Clock::time_point deadline = {},
                         std::atomic_bool* cancelled = nullptr,
                         const char* origin = "unnamed") noexcept
        : frame_type_(frame_type), deadline_(deadline), cancelled_(cancelled), origin_(origin) {}

    FrameType frame_type() const noexcept { return frame_type_; }
    const char* origin() const noexcept { return origin_; }
    Clock::time_point deadline() const noexcept { return deadline_; }
    std::atomic_bool* cancellation() const noexcept { return cancelled_; }
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
};

// What an operation may wait on, declared on its contract. `locks` means it
// takes a lock that something holds across I/O, which makes the I/O its
// wait too; an operation declared `none` takes no such lock.
enum class Waits : uint8_t {
    none = 0,
    state_device = 1,
    data_device = 2,
    network = 4,
    locks = 8,
};

constexpr Waits operator|(Waits a, Waits b) noexcept {
    return static_cast<Waits>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr bool includes(Waits declared, Waits wait) noexcept {
    return (static_cast<uint8_t>(declared) & static_cast<uint8_t>(wait)) != 0;
}

// Control work must never wait on the DATA device or the network (law 1):
// the one combination the guard refuses. State-device waits are control's
// own (metadata, the control store); a lock wait is refused only through
// what it waits on, which the declaration names.
constexpr bool may_enter(FrameType frame_type, Waits declared) noexcept {
    return frame_type != FrameType::control ||
           !(includes(declared, Waits::data_device) || includes(declared, Waits::network));
}

// The runtime boundary check. Each operation that declares a device or
// network wait calls enter() with the caller's context. A refusal is counted
// and logged once per (origin, operation) in production; tests switch the
// guard to throw, so a control path reaching such an operation fails the test.
class WaitGuard {
  public:
    enum class Mode { record, throw_on_violation };
    static void set_mode(Mode) noexcept;
    static Mode mode() noexcept;
    // Refusals since start.
    static uint64_t violations() noexcept;
    // Returns whether the entry was allowed. Throws std::logic_error on a
    // refusal in throw mode.
    static bool enter(const WorkContext&, Waits declared, std::string_view operation);
};

} // namespace macha
