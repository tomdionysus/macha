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

    // Whether this node holds the object. Never reports an object a put is
    // still writing, and changes nothing. Once the presence index is warm it
    // waits on nothing; until then a miss is checked on the store's device
    // (DATA, or the state device for control) under the object's lock, which
    // a put of that object holds across its write.
    static constexpr Waits has_waits = Waits::data_device | Waits::locks;
    static constexpr ThreadSafety has_safety = ThreadSafety::thread_safe;
    virtual bool has(const ObjectId&) const = 0;

    // Advances whenever an object this store held may no longer be held: a
    // removal, an empty file pruned, a backend going away. Whatever was
    // derived from has() before a change in it may be overtaken. An atomic
    // read.
    static constexpr Waits losses_waits = Waits::none;
    static constexpr ThreadSafety losses_safety = ThreadSafety::thread_safe;
    virtual uint64_t losses() const noexcept = 0;

    // Whether has() now answers from the presence index alone: false while
    // the index is still being filled after a start, when a miss costs a
    // device read. A walk that asks has() of every object waits for this.
    static constexpr Waits indexed_waits = Waits::none;
    static constexpr ThreadSafety indexed_safety = ThreadSafety::thread_safe;
    virtual bool indexed() const noexcept = 0;
};

} // namespace macha
