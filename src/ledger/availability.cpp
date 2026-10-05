// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/availability.hpp"

#include "codec.hpp"
#include "crypto.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace macha {

namespace {

Bytes read_node(const NamespaceNodeStore& store, const ObjectId& id) {
    auto encoded = store.get(id);
    if (!encoded)
        throw DecodeError("namespace tree node unavailable: " + to_string(id));
    return std::move(*encoded);
}

Holding roll_up(const ObjectId& id, const NamespaceNodeStore& store, const HeldFn& held,
                const std::function<void()>& pause, std::map<ObjectId, Holding>& nodes) {
    if (const auto found = nodes.find(id); found != nodes.end())
        return found->second;
    if (pause)
        pause();
    Holding sum;
    for (const auto& child : namespace_tree_children(read_node(store, id))) {
        if (child.extent) {
            ++sum.extents;
            if (held(child.id))
                ++sum.held;
        } else {
            const auto beneath = roll_up(child.id, store, held, pause, nodes);
            sum.extents += beneath.extents;
            sum.held += beneath.held;
        }
    }
    nodes.emplace(id, sum);
    return sum;
}

void sort_unique(std::vector<ObjectId>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

} // namespace

HoldingsRollup HoldingsRollup::build(const ObjectId& root, const NamespaceNodeStore& store,
                                     const HeldFn& held, const std::function<void()>& pause,
                                     const HoldingsRollup* carried) {
    HoldingsRollup rollup;
    rollup.root_ = root;
    if (carried && carried->carries_ < carries_max) {
        rollup.nodes_ = carried->nodes_;
        rollup.carries_ = carried->carries_ + 1;
    }
    (void)roll_up(root, store, held, pause, rollup.nodes_);
    return rollup;
}

std::optional<Holding> HoldingsRollup::find(const ObjectId& node) const {
    const auto found = nodes_.find(node);
    if (found == nodes_.end())
        return {};
    return found->second;
}

NodeHoldings describe_holdings(const HoldingsRollup& rollup, const NamespaceNodeStore& store,
                               const HeldFn& held, const ObjectId& node) {
    NodeHoldings answer;
    if (const auto holding = rollup.find(node)) {
        answer.known = true;
        answer.holding = *holding;
    }
    const auto encoded = store.get(node);
    if (!encoded)
        return answer;
    for (const auto& child : namespace_tree_children(*encoded)) {
        if (child.extent) {
            answer.children.push_back(held(child.id));
        } else {
            const auto beneath = rollup.find(child.id);
            answer.children.push_back(beneath && beneath->complete());
        }
    }
    return answer;
}

Bytes encode_tree_holdings_request(std::span<const ObjectId> nodes) {
    if (nodes.size() > tree_holdings_max)
        throw std::invalid_argument("too many tree nodes in one holdings question");
    Writer writer;
    writer.u32(static_cast<uint32_t>(nodes.size()));
    for (const auto& node : nodes)
        writer.fixed(node.bytes);
    return writer.take();
}

std::vector<ObjectId> decode_tree_holdings_request(std::span<const uint8_t> encoded) {
    Reader reader(encoded);
    const auto count = reader.u32();
    if (count > tree_holdings_max)
        throw DecodeError("too many tree nodes in one holdings question");
    std::vector<ObjectId> nodes;
    nodes.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        nodes.push_back(ObjectId{reader.fixed<32>()});
    reader.finish();
    return nodes;
}

Bytes encode_tree_holdings_reply(std::span<const NodeHoldings> answers) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(answers.size()));
    for (const auto& answer : answers) {
        writer.u8(answer.known ? 1 : 0);
        writer.u64(answer.holding.extents);
        writer.u64(answer.holding.held);
        writer.u32(static_cast<uint32_t>(answer.children.size()));
        uint8_t byte = 0;
        for (size_t c = 0; c < answer.children.size(); ++c) {
            if (answer.children[c])
                byte |= static_cast<uint8_t>(1U << (c % 8));
            if (c % 8 == 7 || c + 1 == answer.children.size()) {
                writer.u8(byte);
                byte = 0;
            }
        }
    }
    return writer.take();
}

std::vector<NodeHoldings> decode_tree_holdings_reply(std::span<const uint8_t> encoded) {
    Reader reader(encoded);
    const auto count = reader.u32();
    if (count > tree_holdings_max)
        throw DecodeError("too many answers in one holdings reply");
    std::vector<NodeHoldings> answers(count);
    for (auto& answer : answers) {
        const auto known = reader.u8();
        if (known > 1)
            throw DecodeError("bad holdings answer");
        answer.known = known == 1;
        answer.holding.extents = reader.u64();
        answer.holding.held = reader.u64();
        if (answer.holding.held > answer.holding.extents)
            throw DecodeError("holdings answer holds more than it references");
        const auto children = reader.u32();
        const size_t bytes = (static_cast<size_t>(children) + 7) / 8;
        if (bytes > reader.remaining())
            throw DecodeError("holdings answer child flags truncated");
        const auto flags = reader.view(bytes);
        answer.children.resize(children);
        for (uint32_t c = 0; c < children; ++c)
            answer.children[c] = (flags[c / 8] >> (c % 8)) & 1U;
    }
    reader.finish();
    return answers;
}

bool AvailabilitySurvey::is_unavailable(const ObjectId& id) const {
    return std::binary_search(unavailable.begin(), unavailable.end(), id);
}

bool AvailabilitySurvey::is_unknown(const ObjectId& id) const {
    return std::binary_search(unknown.begin(), unknown.end(), id);
}

AvailabilitySurvey survey_availability(const HoldingsRollup& local,
                                       const NamespaceNodeStore& store, const HeldFn& held,
                                       std::span<PeerHoldings* const> peers) {
    AvailabilitySurvey survey;
    survey.peers_asked = peers.size();
    std::vector<PeerHoldings*> asking(peers.begin(), peers.end());

    std::vector<ObjectId> frontier;
    if (!local.total().complete())
        frontier.push_back(local.root());

    while (!frontier.empty()) {
        ++survey.rounds;
        survey.nodes_asked += frontier.size();
        std::vector<std::vector<NodeHoldings>> answers;
        for (auto peer = asking.begin(); peer != asking.end();) {
            try {
                auto answer = (*peer)->ask(frontier);
                if (answer.size() != frontier.size())
                    throw std::runtime_error("peer answered a different number of tree nodes");
                answers.push_back(std::move(answer));
                ++peer;
            } catch (const std::exception&) {
                ++survey.peers_failed;
                peer = asking.erase(peer);
            }
        }

        std::set<ObjectId> next;
        for (size_t i = 0; i < frontier.size(); ++i) {
            const auto whole = [&](const std::vector<NodeHoldings>& answer) {
                return answer[i].known && answer[i].holding.complete();
            };
            if (std::any_of(answers.begin(), answers.end(), whole))
                continue;
            const auto children = namespace_tree_children(read_node(store, frontier[i]));
            // A peer describes this node only if it answered one flag per child.
            std::vector<const NodeHoldings*> describing;
            for (const auto& answer : answers)
                if (answer[i].children.size() == children.size())
                    describing.push_back(&answer[i]);
            // Every asked peer described it, so what none holds, nobody holds.
            const bool described = describing.size() == peers.size();
            for (size_t c = 0; c < children.size(); ++c) {
                const auto& child = children[c];
                const bool a_peer_has =
                    std::any_of(describing.begin(), describing.end(),
                                [&](const NodeHoldings* answer) { return answer->children[c]; });
                if (a_peer_has)
                    continue;
                if (child.extent) {
                    if (held(child.id))
                        continue;
                    (described ? survey.unavailable : survey.unknown).push_back(child.id);
                } else {
                    const auto here = local.find(child.id);
                    if (here && here->complete())
                        continue;
                    next.insert(child.id);
                }
            }
        }
        frontier.assign(next.begin(), next.end());
    }

    sort_unique(survey.unavailable);
    sort_unique(survey.unknown);
    // Holding is by id: one reference every peer answered for decides the
    // extent, whatever another reference could not.
    std::erase_if(survey.unknown, [&](const ObjectId& id) { return survey.is_unavailable(id); });
    return survey;
}

} // namespace macha
