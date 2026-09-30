// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/object_ledger.hpp"

#include <optional>

namespace macha {

// What the claim walk asks of its caller for a claimed object this node does
// not hold. The caller owns the network credit and the fetch.
class ClaimRestorer {
  public:
    virtual ~ClaimRestorer() = default;
    enum class Outcome : uint8_t { restored, not_restored, waiting_for_credit };
    virtual Outcome restore(RetentionClass, const ObjectId&) = 0;
};

struct ClaimWalkStep {
    size_t examined{};
    size_t missing{};
    size_t restored{};
    // A missing claim met a caller out of credit; the cursor stays before it.
    bool waiting_for_credit{};
    // The step reached its bound with claims left.
    bool unfinished{};
};

// A durable retention claim is a promise about this physical node, not an
// annotation on its namespace view: a claimed copy that scrub or corruption
// removed, belonging only to an unseen branch, is invisible to live-set
// repair. The claim walk examines a bounded slice of one class's claims per
// step, from a cursor it keeps across steps, and asks the restorer for each
// claim not held. Presence costs nothing; only a missing claim may spend.
class ClaimWalk {
  public:
    // Claims examined per step, per class.
    static constexpr size_t step_bound = 16;

    explicit ClaimWalk(RetentionClass type) noexcept : type_(type) {}

    ClaimWalkStep step(const ObjectLedger&, ClaimRestorer&);

    RetentionClass type() const noexcept { return type_; }
    const Cursor<ObjectId>& cursor() const noexcept { return cursor_; }

  private:
    RetentionClass type_;
    Cursor<ObjectId> cursor_;
};

} // namespace macha
