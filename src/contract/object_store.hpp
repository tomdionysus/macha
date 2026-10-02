// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"
#include "types.hpp"

// Bytes by id (object ledger spec, B1). Implemented by StoragePool for DATA
// (one LocalStore per backend) and by a LocalStore for control. Knows nothing
// of references or claims.
namespace macha {

class ObjectStore {
  public:
    virtual ~ObjectStore() = default;

    // Whether this node holds the object. Thread-safe. Waits on nothing once
    // the presence index is warm; before that a miss is checked on the DATA
    // device. Never reports an object a put is still writing.
    static constexpr Waits has_waits = Waits::none;
    virtual bool has(const ObjectId&) const = 0;
};

} // namespace macha
