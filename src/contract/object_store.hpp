// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"
#include "types.hpp"

// Bytes by id (the object ledger spec, B1). Implemented by StoragePool for
// DATA (one LocalStore per backend) and by a LocalStore for control. It knows
// nothing of references or claims. T2 moves has() behind it; the rest of the
// table in B1 follows as each consumer converts.
namespace macha {

class ObjectStore {
  public:
    virtual ~ObjectStore() = default;

    // Whether this node holds the object. Thread-safe. Waits on nothing once
    // the store has warmed its presence index; before that, a miss is checked
    // on the DATA device (LocalStore::has). Never reports an object a put is
    // still writing.
    static constexpr Waits has_waits = Waits::none;
    virtual bool has(const ObjectId&) const = 0;
};

} // namespace macha
