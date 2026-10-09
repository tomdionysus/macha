// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/object_ledger.hpp"
#include "contract/published.hpp"
#include "storage/retention.hpp"

namespace macha {

// Claims forward to the ClaimStore, holdings to the class's object store;
// owns the published horizons. The owner keeps all three stores alive for the
// ledger's lifetime.
class RetentionLedger final : public ObjectLedger {
    ClaimStore& claims_;
    const ObjectStore& data_;
    const ObjectStore& control_;
    Published<InventoryHorizon> inventory_;
    Published<ReleaseHorizon> release_;

  public:
    RetentionLedger(ClaimStore& claims, const ObjectStore& data,
                    const ObjectStore& control) noexcept
        : claims_(claims), data_(data), control_(control) {}

    Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                     Budget&) const override;
    bool held(RetentionClass, const ObjectId&) const override;
    HeldView held_view(RetentionClass type) const override {
        return (type == RetentionClass::data ? data_ : control_).held_view();
    }
    InventoryHandle inventory() const override { return inventory_.handle(); }
    ReleaseHandle release() const override { return release_.handle(); }
    void publish(InventoryHandle) override;
    bool publish(ReleaseBuild) override;
    bool retained(RetentionClass type, const ObjectId& id) const override {
        return claims_.retained(type, id);
    }
    size_t release_unreferenced(RetentionClass, const ReleaseHorizon&,
                                size_t operation_budget) override;
    size_t prune_unclaimed(RetentionClass, size_t operation_budget) override;
    bool compact_if_needed(size_t record_threshold) override {
        return claims_.compact_if_needed(record_threshold);
    }
};

} // namespace macha
