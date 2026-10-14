// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ledger/object_trie.hpp"
#include "ledger/trie_diff.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace macha {

// Several tries' records read as one trie: each id once, with the value of
// the first part that holds it. Its summaries are those one trie over the
// same records would have, so it diffs against any trie.
//
// A subtree only one part holds, or that every part holding it holds alike,
// is that part's own: nothing below it is read. Only subtrees several parts
// hold differently are worked out, reading the parts' nodes below them; a
// union made with the one before it as a hint reuses each subtree whose
// parts are as they were, so its cost follows what changed.
//
// Immutable; thread safe; reads the parts' nodes (the state device) only
// while it is made and when asked about a subtree several parts share.
class TrieUnion {
  public:
    explicit TrieUnion(std::vector<ObjectTrie::Snapshot> parts,
                       const TrieUnion* hint = nullptr);

    ObjectTrie::Summary root() const noexcept;
    uint64_t size() const noexcept { return root().count; }
    Hash256 root_hash() const noexcept { return root().hash; }
    std::optional<Bytes> get(const ObjectId&) const;
    // As ObjectTrie::Snapshot's.
    std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix>) const;
    std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix>,
                                                         uint64_t most = UINT64_MAX) const;
    const std::vector<ObjectTrie::Snapshot>& parts() const noexcept { return parts_; }

  private:
    struct Node;
    std::vector<ObjectTrie::Snapshot> parts_;
    std::shared_ptr<const Node> root_;

    std::shared_ptr<const Node> merge(const ObjectTrie::Prefix&,
                                      std::vector<ObjectTrie::Summary> inputs,
                                      const std::shared_ptr<const Node>& hint) const;
    std::vector<ObjectTrie::Record> merged_records(const ObjectTrie::Prefix&,
                                                   uint64_t most) const;
};

// A union as a source.
class UnionSource final : public TrieSource {
  public:
    explicit UnionSource(std::shared_ptr<const TrieUnion> trie) : trie_(std::move(trie)) {}
    ObjectTrie::Summary root() override { return trie_->root(); }
    std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override {
        return trie_->children(p);
    }
    std::vector<std::vector<ObjectTrie::Record>>
    records(std::span<const ObjectTrie::Prefix> p) override {
        return trie_->records(p);
    }

  private:
    std::shared_ptr<const TrieUnion> trie_;
};

} // namespace macha
