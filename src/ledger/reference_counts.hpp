// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ledger/object_trie.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace macha {

// What the namespace refers to, counted per object and kept on the state
// device: its DATA extents (one count per reference, so an extent two files
// share goes only with its last reference) and the tree's own nodes, at the
// namespace root they were last brought to. Moved from root to root by the
// tree diff, so a restart resumes where it stopped instead of walking the
// namespace. Single owner (the filesystem's census); views are read from any
// thread.
class ReferenceCounts {
  public:
    ReferenceCounts(std::filesystem::path dir, std::array<uint8_t, 32> key, size_t cache_bytes);

    struct Totals {
        uint64_t entries{};
        uint64_t extent_count{};
    };
    // The root the counts are at; none before the first count, for a
    // namespace that is not a tree, and after an interrupted change.
    std::optional<ObjectId> root() const noexcept { return root_; }
    Totals totals() const noexcept { return totals_; }

    // Replaces every count: one item per reference, in any order.
    void reset(const std::optional<ObjectId>& root, std::vector<ObjectId> extents,
               std::vector<ObjectId> nodes, Totals);
    // One added for each item in, one taken for each item out. False, and
    // nothing changed, when something to take out is not counted.
    bool change(const ObjectId& root, std::vector<ObjectId> extents_in,
                std::vector<ObjectId> extents_out, std::vector<ObjectId> nodes_in,
                std::vector<ObjectId> nodes_out, Totals);

    // Frozen views of the ids counted now (a record's value is its count).
    struct Views {
        ObjectTrie::Snapshot extents;
        ObjectTrie::Snapshot nodes;
    };
    Views views() const;

  private:
    std::filesystem::path dir_;
    ObjectTrie extents_;
    ObjectTrie nodes_;
    std::optional<ObjectId> root_;
    Totals totals_;

    // What the tries hold: a root and totals, or dirty while they change.
    void write_state(bool clean) const;
};

} // namespace macha
