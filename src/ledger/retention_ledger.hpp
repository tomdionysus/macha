// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/object_ledger.hpp"
#include "contract/published.hpp"
#include "storage/retention.hpp"

namespace macha {

// ObjectLedger at stage 0: claims forward to the RetentionStore, holdings to
// the class's object store, the horizons are published snapshots it owns.
// Holds references to the stores; its owner keeps all three alive for its
// lifetime.
class RetentionLedger final : public ObjectLedger {
    const RetentionStore& claims_;
    const ObjectStore& data_;
    const ObjectStore& control_;
    Published<InventoryHorizon> inventory_;
    Published<ReleaseHorizon> release_;

  public:
    RetentionLedger(const RetentionStore& claims, const ObjectStore& data,
                    const ObjectStore& control) noexcept
        : claims_(claims), data_(data), control_(control) {}

    Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                     Budget&) const override;
    bool held(RetentionClass, const ObjectId&) const override;
    InventoryHandle inventory() const override { return inventory_.handle(); }
    ReleaseHandle release() const override { return release_.handle(); }
    void publish(InventoryHandle) override;
    bool publish(ReleaseBuild) override;
};

} // namespace macha
