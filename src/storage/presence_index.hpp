// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <cstddef>
#include <set>
#include <span>

namespace macha {

// Which loose objects a LocalStore holds, as far as it knows, and whether it
// knows everything (the object ledger plan, P). A primitive: no threads, no
// I/O, no lock of its own -- the store holds its index mutex around every
// call -- so its whole phase space is tested exhaustively against a model.
//
// Events, and the state each leaves for the id it names:
//   installed  a put finished writing it:      present, not pruned (a forgotten
//                                              mark may stay: only a listing of
//                                              an absent id reads it, and the id
//                                              can only become absent again by an
//                                              event that sets it anew)
//   observed   a stat or the scan saw it:      present
//   forgotten  removed (or its stamp lost):    absent; remembered while warming
//   pruned     an empty file was deleted:      absent and pruned; remembered while warming
//   listed     warm-up read its name:          present, unless pruned or forgotten
//   warmed     warm-up listed every name:      authoritative from now on; the
//                                              forgotten set is no longer needed
//
// Invariants: an id forgotten or pruned before warm-up publishes a listing
// that contains it stays absent; once authoritative, an id is present exactly
// when its last event was installed, observed or listed-and-kept.
class PresenceIndex {
    std::set<ObjectId> present_;
    std::set<ObjectId> pruned_;
    std::set<ObjectId> forgotten_while_warming_;
    bool authoritative_{};

  public:
    enum class Answer { present, absent, unknown };

    void installed(const ObjectId&);
    void observed(const ObjectId&);
    void forgotten(const ObjectId&);
    void pruned(const ObjectId&);
    void listed(std::span<const ObjectId>);
    void warmed();

    // present if known present; absent once authoritative; else unknown,
    // and the caller asks the device.
    Answer answer(const ObjectId&) const;
    bool authoritative() const noexcept {
        return authoritative_;
    }
    size_t size() const noexcept {
        return present_.size();
    }
};

} // namespace macha
