// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/claim_walk.hpp"

#include "observation.hpp"

namespace macha {

ClaimWalkStep ClaimWalk::step(const ObjectLedger& ledger, ClaimRestorer& restorer) {
    static auto& examine = observations().histogram("maintenance.claim_walk.examine_us");
    static auto& examined = observations().counter("maintenance.claim_walk.examined");
    static auto& missing = observations().counter("maintenance.claim_walk.missing");

    ClaimWalkStep step;
    Budget budget;
    budget.operations(step_bound);
    const auto page = ledger.claimed(type_, cursor_, budget);
    step.unfinished = page.stopped == Stop::budget;
    std::optional<ObjectId> resume = cursor_.after;
    for (const auto& id : page.items) {
        const auto started = Clock::now();
        const bool held = ledger.held(type_, id);
        examine.record(elapsed_us(started));
        examined.fetch_add(1, std::memory_order_relaxed);
        ++step.examined;
        if (!held) {
            missing.fetch_add(1, std::memory_order_relaxed);
            ++step.missing;
            const auto outcome = restorer.restore(type_, id);
            if (outcome == ClaimRestorer::Outcome::waiting_for_credit) {
                // Resume at this claim when credit returns.
                cursor_.after = resume;
                step.waiting_for_credit = true;
                step.unfinished = false;
                return step;
            }
            if (outcome == ClaimRestorer::Outcome::restored)
                ++step.restored;
        }
        resume = id;
    }
    cursor_ = page.next;
    return step;
}

} // namespace macha
