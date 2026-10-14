// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"
#include "ledger/trie_union.hpp"
#include "types.hpp"

#include <functional>

// Bytes by id (object ledger spec, B1). Implemented by StoragePool for DATA
// (one LocalStore per backend) and by a LocalStore for control. Knows nothing
// of references or claims.
namespace macha {

// The identity of what a store holds: its held trie's root hash, which
// depends only on the held ids, so it changes when and only when what is
// held changes, and any two stores holding the same ids share it. `removals` counts ids that stopped being held
// since the process started (a removal, a loss, a disk going away): a view
// whose removals equal the store's never claims what the store has dropped,
// though the store may have gained since. `complete` is false while a disk is
// still being seeded, when the store cannot say what it holds.
struct HeldIdentity {
    Hash256 hash{};
    uint64_t removals{};
    bool complete{};
    bool operator==(const HeldIdentity&) const = default;
};

// A frozen view of what a store holds. `held` reads the view, not the store:
// ledger pages, through the ledger's cache from the state device, never a
// DATA device. Safe from any thread for as long as the view is kept.
struct HeldView {
    HeldIdentity identity;
    std::function<bool(const ObjectId&)> held;
    // Every held id: the union of each online disk's held trie, of those
    // seeded while the identity is incomplete. Null from a disk's own store
    // until it is seeded.
    std::shared_ptr<const TrieUnion> trie;
};

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

    // What this store holds, frozen at one moment, and the identity that
    // names it. Taken from the held ledgers: it reads no DATA device, only
    // the ledger nodes below what changed on disks holding ids in common. A
    // memo derived from a view is current exactly while the store's identity
    // equals the view's.
    static constexpr Waits held_view_waits = local_waits;
    static constexpr ThreadSafety held_view_safety = ThreadSafety::thread_safe;
    virtual HeldView held_view() const = 0;
};

} // namespace macha
