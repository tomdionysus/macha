// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// One cursor, one budget, one page (the object ledger spec, A3): the shape
// of every bounded walk a contract offers. A walk is
// (state, cursor, budget) -> (page, next cursor).
namespace macha {

// A position the caller owns: the last key handed out, or none for the
// start. Serialisable wherever Key is.
template <class Key> struct Cursor {
    std::optional<Key> after;
    bool operator==(const Cursor&) const = default;
};

// Why a walk returned.
enum class Stop : uint8_t {
    end,       // the walk reached the end; the pass is complete
    budget,    // an operation, byte or item bound was spent
    deadline,  // the budget's or the context's deadline passed
    cancelled, // the context was cancelled
    yield,     // the yield source asked for the turn back
};

template <class Item, class Key> struct Page {
    std::vector<Item> items;
    Cursor<Key> next;
    Stop stopped{Stop::end};
    bool complete() const noexcept { return stopped == Stop::end; }
};

// Whoever decides when background work gives way (repair's weighted share,
// a higher class being active). Injected, so a budget is not a pure value
// and says so: its answers depend on service-level state.
class YieldSource {
  public:
    virtual ~YieldSource() = default;
    virtual bool should_yield() const = 0;
};

// What a walk may spend, and who is spending it. Bounds are optional: an
// absent bound never stops a walk. The yield source, if any, must outlive
// the budget. Single owner: one walk spends a budget.
class Budget {
  public:
    using Clock = std::chrono::steady_clock;

    explicit Budget(WorkContext context = WorkContext{}) noexcept : context_(context) {}

    Budget& operations(size_t limit) noexcept {
        operations_left_ = limit;
        return *this;
    }
    Budget& bytes(uint64_t limit) noexcept {
        bytes_left_ = limit;
        return *this;
    }
    Budget& deadline(Clock::time_point at) noexcept {
        deadline_ = at;
        return *this;
    }
    Budget& yield_to(const YieldSource* source) noexcept {
        yield_ = source;
        return *this;
    }

    const WorkContext& context() const noexcept { return context_; }

    // Why the walk must stop before its next step, if it must: cancellation
    // first, then either deadline (the budget's or the context's), then the
    // yield source. Bounds are checked by take_*.
    std::optional<Stop> must_stop(Clock::time_point now = Clock::now()) const {
        if (context_.cancelled())
            return Stop::cancelled;
        if ((deadline_ && now >= *deadline_) || context_.expired(now))
            return Stop::deadline;
        if (yield_ && yield_->should_yield())
            return Stop::yield;
        return std::nullopt;
    }
    // Spends one operation. False, spending nothing, when none is left.
    bool take_operation() noexcept {
        if (!operations_left_)
            return true;
        if (*operations_left_ == 0)
            return false;
        --*operations_left_;
        return true;
    }
    // Spends `n` bytes. False, spending nothing, when fewer than `n` are left.
    bool take_bytes(uint64_t n) noexcept {
        if (!bytes_left_)
            return true;
        if (*bytes_left_ < n)
            return false;
        *bytes_left_ -= n;
        return true;
    }
    std::optional<size_t> operations_left() const noexcept { return operations_left_; }
    std::optional<uint64_t> bytes_left() const noexcept { return bytes_left_; }

  private:
    WorkContext context_;
    std::optional<size_t> operations_left_;
    std::optional<uint64_t> bytes_left_;
    std::optional<Clock::time_point> deadline_;
    const YieldSource* yield_{};
};

} // namespace macha
