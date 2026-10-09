// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ledger/object_trie.hpp"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace macha {

// One side of a diff: a trie that answers, for a batch of prefixes, the
// subtrees one byte below each and the records below each. A snapshot here;
// a peer's trie over the wire.
class TrieSource {
  public:
    virtual ~TrieSource() = default;
    virtual ObjectTrie::Summary root() = 0;
    virtual std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix>) = 0;
    virtual std::vector<std::vector<ObjectTrie::Record>>
    records(std::span<const ObjectTrie::Prefix>) = 0;
};

// A snapshot as a source. Keeps its own copy of the snapshot.
class SnapshotSource final : public TrieSource {
  public:
    explicit SnapshotSource(ObjectTrie::Snapshot snapshot) : snapshot_(std::move(snapshot)) {}
    ObjectTrie::Summary root() override { return {snapshot_.size(), snapshot_.root_hash()}; }
    std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override {
        return snapshot_.children(p);
    }
    std::vector<std::vector<ObjectTrie::Record>>
    records(std::span<const ObjectTrie::Prefix> p) override {
        return snapshot_.records(p);
    }

  private:
    ObjectTrie::Snapshot snapshot_;
};

// An id whose record differs: held by one side only, or with another value.
struct TrieDifference {
    ObjectId id;
    std::optional<Bytes> left;
    std::optional<Bytes> right;
    bool operator==(const TrieDifference&) const = default;
};

// What a diff asked of each side.
struct TrieDiffCost {
    // Batches of questions: one children() and one records() a side count
    // as one round each.
    size_t rounds{};
    size_t prefixes{};
    size_t records{};
};

// Every id whose record differs between the two sides, each once, in no
// particular order. Descends a level at a time: each side is asked once per
// level about every subtree whose summaries differ, and records are read
// once neither side holds more than a leaf below a prefix. Where the sides
// agree nothing below is read, so the cost follows the difference, not the
// size. `pause`, if given, is called between rounds.
TrieDiffCost diff_tries(TrieSource& left, TrieSource& right,
                        const std::function<void(const TrieDifference&)>& visit,
                        const std::function<void()>& pause = {});

// The records of `wanted` whose ids none of `held` holds, in id order: one
// pass over each in step, a page at a time. `pause`, if given, is called
// between pages.
void records_not_held(const ObjectTrie::Snapshot& wanted,
                      std::span<const ObjectTrie::Snapshot> held,
                      const std::function<void(const ObjectTrie::Record&)>& visit,
                      const std::function<void()>& pause = {});

// The wire form of a question to a peer's trie and its answers. `root` asks
// for the summary of the peer's current trie and pins that snapshot under
// its hash; `children` and `records` ask about prefixes of the snapshot
// pinned under `at`. At most trie_question_max prefixes. Decoding throws
// DecodeError.
enum class TrieQuestionKind : uint8_t { root = 0, children = 1, records = 2 };
enum class TrieName : uint8_t { held_data = 0 };
inline constexpr size_t trie_question_max = 64;
struct TrieQuestion {
    TrieQuestionKind kind{TrieQuestionKind::root};
    TrieName trie{TrieName::held_data};
    Hash256 at{};
    std::vector<ObjectTrie::Prefix> prefixes;
    bool operator==(const TrieQuestion&) const = default;
};
Bytes encode_trie_question(const TrieQuestion&);
TrieQuestion decode_trie_question(std::span<const uint8_t>);
Bytes encode_trie_root(const ObjectTrie::Summary&);
ObjectTrie::Summary decode_trie_root(std::span<const uint8_t>);
// Only the non-empty slots travel.
Bytes encode_trie_children(std::span<const ObjectTrie::Children>);
std::vector<ObjectTrie::Children> decode_trie_children(std::span<const uint8_t>);
Bytes encode_trie_records(std::span<const std::vector<ObjectTrie::Record>>);
std::vector<std::vector<ObjectTrie::Record>> decode_trie_records(std::span<const uint8_t>);

} // namespace macha
