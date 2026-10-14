// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/trie_union.hpp"

#include <stdexcept>

namespace macha {

struct TrieUnion::Node {
    // Each part's summary at this prefix.
    std::vector<ObjectTrie::Summary> inputs;
    ObjectTrie::Summary summary;
    // A part whose records below here are the union's.
    std::optional<size_t> whole;
    // The 256 subtrees below an interior node worked out; none for a leaf.
    std::vector<std::shared_ptr<const Node>> below;
};

TrieUnion::TrieUnion(std::vector<ObjectTrie::Snapshot> parts, const TrieUnion* hint)
    : parts_(std::move(parts)) {
    std::vector<ObjectTrie::Summary> inputs;
    inputs.reserve(parts_.size());
    for (const auto& part : parts_)
        inputs.push_back({part.size(), part.root_hash()});
    root_ = merge({}, std::move(inputs), hint ? hint->root_ : nullptr);
}

ObjectTrie::Summary TrieUnion::root() const noexcept {
    return root_ ? root_->summary : ObjectTrie::Summary{};
}

std::optional<Bytes> TrieUnion::get(const ObjectId& id) const {
    for (const auto& part : parts_)
        if (auto value = part.get(id))
            return value;
    return std::nullopt;
}

std::shared_ptr<const TrieUnion::Node>
TrieUnion::merge(const ObjectTrie::Prefix& prefix, std::vector<ObjectTrie::Summary> inputs,
                 const std::shared_ptr<const Node>& hint) const {
    if (hint && hint->inputs == inputs)
        return hint;
    std::optional<size_t> first;
    bool alike = true;
    uint64_t most = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!inputs[i].count)
            continue;
        most += inputs[i].count;
        if (!first)
            first = i;
        else if (inputs[i] != inputs[*first])
            alike = false;
    }
    if (!first)
        return nullptr;
    auto node = std::make_shared<Node>();
    node->inputs = std::move(inputs);
    if (alike) {
        node->summary = node->inputs[*first];
        node->whole = first;
        return node;
    }
    if (most > ObjectTrie::leaf_max) {
        std::vector<ObjectTrie::Children> below(parts_.size());
        for (size_t i = 0; i < parts_.size(); ++i)
            if (node->inputs[i].count)
                below[i] = parts_[i].children(std::span(&prefix, 1)).front();
        const bool hinted = hint && hint->below.size() == 256;
        node->below.resize(256);
        ObjectTrie::Children slots{};
        uint64_t count = 0;
        for (size_t slot = 0; slot < 256; ++slot) {
            std::vector<ObjectTrie::Summary> in(parts_.size());
            for (size_t i = 0; i < parts_.size(); ++i)
                in[i] = below[i][slot];
            node->below[slot] = merge(prefix.child(static_cast<uint8_t>(slot)), std::move(in),
                                      hinted ? hint->below[slot] : nullptr);
            if (node->below[slot]) {
                slots[slot] = node->below[slot]->summary;
                count += slots[slot].count;
            }
        }
        if (count > ObjectTrie::leaf_max) {
            node->summary = ObjectTrie::interior_summary(slots);
            return node;
        }
        node->below.clear();
    }
    node->summary = ObjectTrie::leaf_summary(merged_records(prefix, UINT64_MAX));
    return node;
}

std::vector<ObjectTrie::Record> TrieUnion::merged_records(const ObjectTrie::Prefix& prefix,
                                                          uint64_t most) const {
    std::vector<ObjectTrie::Record> out;
    for (const auto& part : parts_) {
        auto got = std::move(part.records(std::span(&prefix, 1), most).front());
        if (got.empty())
            continue;
        std::vector<ObjectTrie::Record> merged;
        merged.reserve(out.size() + got.size());
        auto a = out.begin();
        auto b = got.begin();
        while (a != out.end() || b != got.end()) {
            if (b == got.end() || (a != out.end() && a->first < b->first)) {
                merged.push_back(std::move(*a++));
            } else if (a == out.end() || b->first < a->first) {
                merged.push_back(std::move(*b++));
            } else {
                merged.push_back(std::move(*a++));
                ++b;
            }
        }
        out = std::move(merged);
        if (out.size() > most)
            throw std::length_error("trie union: more records below a prefix than asked for");
    }
    return out;
}

namespace {

// The worked-out node a prefix falls in, and how deep it sits.
template <typename NodePtr>
NodePtr find_node(NodePtr node, const ObjectTrie::Prefix& prefix) {
    for (uint8_t depth = 0; node && node->below.size() == 256 && depth < prefix.length; ++depth)
        node = node->below[prefix.bytes[depth]];
    return node;
}

} // namespace

std::vector<ObjectTrie::Children>
TrieUnion::children(std::span<const ObjectTrie::Prefix> prefixes) const {
    std::vector<ObjectTrie::Children> out(prefixes.size());
    for (size_t i = 0; i < prefixes.size(); ++i) {
        const auto& prefix = prefixes[i];
        if (prefix.length >= 32)
            throw std::logic_error("trie union: a whole id has no children");
        const auto node = find_node(root_, prefix);
        if (!node)
            continue;
        if (node->whole) {
            out[i] = parts_[*node->whole].children(std::span(&prefix, 1)).front();
            continue;
        }
        if (node->below.size() == 256) {
            for (size_t slot = 0; slot < 256; ++slot)
                if (node->below[slot])
                    out[i][slot] = node->below[slot]->summary;
            continue;
        }
        // Below a leaf every subtree is a leaf of its own records.
        const auto records = merged_records(prefix, UINT64_MAX);
        auto at = records.begin();
        while (at != records.end()) {
            const auto byte = at->first.bytes[prefix.length];
            auto end = at;
            while (end != records.end() && end->first.bytes[prefix.length] == byte)
                ++end;
            out[i][byte] = ObjectTrie::leaf_summary(std::span(at, end));
            at = end;
        }
    }
    return out;
}

std::vector<std::vector<ObjectTrie::Record>>
TrieUnion::records(std::span<const ObjectTrie::Prefix> prefixes, uint64_t most) const {
    std::vector<std::vector<ObjectTrie::Record>> out(prefixes.size());
    for (size_t i = 0; i < prefixes.size(); ++i) {
        const auto& prefix = prefixes[i];
        const auto node = find_node(root_, prefix);
        if (!node)
            continue;
        if (node->whole) {
            out[i] = std::move(parts_[*node->whole].records(std::span(&prefix, 1), most).front());
            continue;
        }
        if (node->below.size() == 256 && node->summary.count > most)
            throw std::length_error("trie union: more records below a prefix than asked for");
        out[i] = merged_records(prefix, most);
    }
    return out;
}

} // namespace macha
