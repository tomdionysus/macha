// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/object_ledger.hpp"
#include "storage/retention.hpp"

namespace macha {

// ObjectLedger at stage 0: claims forward to the RetentionStore, holdings to
// the class's object store. Holds references; its owner keeps all three
// alive for its lifetime.
class RetentionLedger final : public ObjectLedger {
    const RetentionStore& claims_;
    const ObjectStore& data_;
    const ObjectStore& control_;

  public:
    RetentionLedger(const RetentionStore& claims, const ObjectStore& data,
                    const ObjectStore& control) noexcept
        : claims_(claims), data_(data), control_(control) {}

    Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                     Budget&) const override;
    bool held(RetentionClass, const ObjectId&) const override;
};

} // namespace macha
