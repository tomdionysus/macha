// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "metadata/namespace_tree.hpp"
#include "types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

// Which of the namespace's extents some reachable node holds, found by
// comparing nodes over the namespace tree (object ledger spec, decision log
// 2026-10-03). Tree node ids are content addresses, so two nodes agree on what
// a subtree is whatever generation each is at: a subtree a node holds whole is
// answered by its id alone, and only partial subtrees are descended.
//
// Primitives: no locks, no threads; I/O only through what they are given.
namespace macha {

// The extents referenced beneath a tree node, and how many of those
// references this node holds. An extent referenced twice counts twice.
struct Holding {
    uint64_t extents{};
    uint64_t held{};
    bool complete() const noexcept { return held == extents; }
    auto operator<=>(const Holding&) const = default;
};

// Whether this node holds a DATA extent.
using HeldFn = std::function<bool(const ObjectId&)>;

// One node's holdings over a namespace tree, by tree node id.
class HoldingsRollup {
  public:
    // Reads every tree node beneath `root` once (a shared subtree is counted
    // where it is referenced, read once) and asks `held` once per extent
    // reference. `pause`, if given, is called between tree nodes, for pacing.
    // Throws DecodeError if a tree node cannot be read.
    //
    // `carried`: a roll-up of another tree, taken while this node held what it
    // holds now. A subtree the two trees share keeps its count and is not
    // read again, so the cost is what differs between the trees. Counts of
    // nodes the new tree no longer has are carried along; every
    // `carries_max` builds a roll-up is made afresh and they are dropped.
    static HoldingsRollup build(const ObjectId& root, const NamespaceNodeStore& store,
                                const HeldFn& held, const std::function<void()>& pause = {},
                                const HoldingsRollup* carried = nullptr);
    static constexpr size_t carries_max = 64;

    const ObjectId& root() const noexcept { return root_; }
    Holding total() const { return nodes_.at(root_); }
    // None for a node that is not in this tree.
    std::optional<Holding> find(const ObjectId& node) const;
    size_t nodes() const noexcept { return nodes_.size(); }

    // The roll-up as bytes, kept so a restarted node can answer from it
    // while its store's presence index fills; decode refuses anything it
    // cannot read whole.
    Bytes encode() const;
    static HoldingsRollup decode(std::span<const uint8_t>);

  private:
    ObjectId root_{};
    std::map<ObjectId, Holding> nodes_;
    size_t carries_{};
};

// The roll-up a node answers peers from: the current one, while nothing has
// been lost since it was made; else, before any since a restart, the one
// kept from before it, while nothing has been lost since it was read back.
// None: the node cannot answer yet.
const HoldingsRollup* answering_rollup(const HoldingsRollup* current, uint64_t current_losses,
                                       const HoldingsRollup* kept, uint64_t kept_losses,
                                       uint64_t losses) noexcept;

// A node's answer about one tree node. `known`: the node is in its tree, and
// `holding` is its count. `children`, when not empty, has one flag per direct
// child in namespace_tree_children order: an extent it holds, or a child node
// it holds whole. A node that has the tree node's bytes answers `children`
// even when the node is not in its own tree.
struct NodeHoldings {
    bool known{};
    Holding holding{};
    std::vector<bool> children;
    auto operator<=>(const NodeHoldings&) const = default;
};

// What this node answers a peer asking about `node`.
NodeHoldings describe_holdings(const HoldingsRollup& rollup, const NamespaceNodeStore& store,
                               const HeldFn& held, const ObjectId& node);

// The wire form of a question (tree node ids) and its answers. A question
// carries at most tree_holdings_max ids. Decoding throws DecodeError.
inline constexpr size_t tree_holdings_max = 2048;
// The extents beneath the roll-up's root this node does not hold, sorted and
// unique: a descent into only the subtrees the roll-up does not count as held
// whole, so a node that holds everything reads one node. `pause` is called
// between tree nodes. Throws DecodeError if a node cannot be read.
std::vector<ObjectId> missing_extents(const HoldingsRollup&, const NamespaceNodeStore&,
                                      const HeldFn& held,
                                      const std::function<void()>& pause = {});

Bytes encode_tree_holdings_request(std::span<const ObjectId> nodes);
std::vector<ObjectId> decode_tree_holdings_request(std::span<const uint8_t>);
Bytes encode_tree_holdings_reply(std::span<const NodeHoldings> answers);
std::vector<NodeHoldings> decode_tree_holdings_reply(std::span<const uint8_t>);

// A reachable peer, asked about tree nodes: one answer per id, in order.
// Throws if the peer cannot be asked.
class PeerHoldings {
  public:
    virtual ~PeerHoldings() = default;
    virtual std::vector<NodeHoldings> ask(std::span<const ObjectId> nodes) = 0;
};

// What the reachable nodes hold between them.
struct AvailabilitySurvey {
    // Extents no asked node holds; sorted, unique.
    std::vector<ObjectId> unavailable;
    // Extents beneath a tree node no peer could describe, not held here:
    // whether a peer holds them is not known. Sorted, unique.
    std::vector<ObjectId> unknown;
    size_t peers_asked{};
    size_t peers_failed{};
    // Descent cost: rounds of questions, and tree node ids asked about.
    size_t rounds{};
    uint64_t nodes_asked{};

    bool is_unavailable(const ObjectId&) const;
    bool is_unknown(const ObjectId&) const;
};

// What a survey found at each tree node every peer answered for: the extents
// directly beneath it that nobody holds and the child nodes it went on into.
// Tree nodes are content addresses, so while no peer's holdings have changed
// and this node has lost nothing, a later survey of another tree finds the
// same at any node the two trees share, less what this node has gained, and
// need not ask about it.
struct SurveyMemo {
    struct Node {
        std::vector<ObjectId> unavailable;
        std::vector<ObjectId> descend;
    };
    std::map<ObjectId, Node> nodes;
};

// Descends from the root through the subtrees neither this node nor any one
// peer holds whole. A peer that fails once is not asked again; what only it
// could have described becomes `unknown`. Throws DecodeError if a tree node
// this node must read is missing.
//
// The extents beneath the roll-up's root that this node holds and `peer` does
// not, sorted: the peer is asked only about subtrees it does not hold whole
// and this node holds something under. None when the peer cannot answer or
// cannot describe a tree node it lacks, since what it holds is then unknown.
std::optional<std::vector<ObjectId>> extents_peer_lacks(const HoldingsRollup& local,
                                                        const NamespaceNodeStore& store,
                                                        const HeldFn& held, PeerHoldings& peer);

// `known`: a memo from a survey since which no peer's holdings have changed
// and this node has lost nothing. A node in it is settled from it and no peer
// is asked about it. `made` receives this survey's memo.
AvailabilitySurvey survey_availability(const HoldingsRollup& local,
                                       const NamespaceNodeStore& store, const HeldFn& held,
                                       std::span<PeerHoldings* const> peers,
                                       const SurveyMemo* known = nullptr,
                                       SurveyMemo* made = nullptr);

} // namespace macha
