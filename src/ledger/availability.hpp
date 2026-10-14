// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/id_lookup.hpp"
#include "ledger/trie_diff.hpp"
#include "ledger/trie_union.hpp"
#include "types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

// Which referenced objects some reachable node holds, found by diffing this
// node's held trie against each peer's (replication-by-diff proposal).
//
// Primitives: no locks, no threads; I/O only through what they are given.
namespace macha {

// Whether this node holds a DATA extent.
using HeldFn = std::function<bool(const ObjectId&)>;

// What the reachable nodes hold between them.
struct AvailabilitySurvey {
    // Referenced, and held by no node asked; sorted, unique.
    std::vector<ObjectId> unavailable;
    // Referenced, not held here or by any peer that answered, while some peer
    // did not answer: whether that peer holds them is not known. Sorted,
    // unique.
    std::vector<ObjectId> unknown;
    size_t peers_asked{};
    size_t peers_failed{};
    // Diff cost: rounds of questions, and prefixes asked about.
    size_t rounds{};
    uint64_t nodes_asked{};

    bool is_unavailable(const ObjectId&) const;
    bool is_unknown(const ObjectId&) const;
};

// One peer's part in a survey. `answered`: the diff ran to the end, and
// `root` is the held trie it was of. `lacks`: what is referenced, held here
// and not by the peer, sorted.
struct PeerDiff {
    bool answered{};
    Hash256 root{};
    std::vector<ObjectId> lacks;
};

// The survey from trie diffs: this node's held trie against each peer's.
// `missing_here` (sorted) is what is referenced that this node lacks; what an
// answering peer holds of it is available. The rest is unavailable when every
// peer answered and unknown otherwise. `peers` gets one entry per peer, in
// order; a peer that fails is not asked again in this survey.
AvailabilitySurvey survey_by_diff(const std::shared_ptr<const TrieUnion>& mine,
                                  const std::vector<ObjectId>& missing_here,
                                  const IdLookup& referenced, std::span<TrieSource* const> sources,
                                  std::vector<PeerDiff>& peers,
                                  const std::function<void()>& pause = {});

// For a survey some peer could not finish: what `last` found held by no
// reachable node stays so in `next`, less what this node now holds, and is no
// longer unknown.
void keep_unavailable(AvailabilitySurvey& next, const AvailabilitySurvey& last,
                      const HeldFn& held);

} // namespace macha
