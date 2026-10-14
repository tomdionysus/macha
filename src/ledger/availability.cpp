// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/availability.hpp"
#include "log.hpp"

#include <algorithm>
#include <iterator>
#include <optional>

namespace macha {

namespace {

void sort_unique(std::vector<ObjectId>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

// A source that remembers the root it answered with.
class Recorded final : public TrieSource {
  public:
    explicit Recorded(TrieSource& source) : source_(source) {}
    ObjectTrie::Summary root() override {
        const auto summary = source_.root();
        root_ = summary.hash;
        return summary;
    }
    std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override {
        return source_.children(p);
    }
    std::vector<std::vector<ObjectTrie::Record>>
    records(std::span<const ObjectTrie::Prefix> p) override {
        return source_.records(p);
    }
    const Hash256& root_hash() const noexcept { return root_; }

  private:
    TrieSource& source_;
    Hash256 root_{};
};

} // namespace

bool AvailabilitySurvey::is_unavailable(const ObjectId& id) const {
    return std::binary_search(unavailable.begin(), unavailable.end(), id);
}

bool AvailabilitySurvey::is_unknown(const ObjectId& id) const {
    return std::binary_search(unknown.begin(), unknown.end(), id);
}

void keep_unavailable(AvailabilitySurvey& next, const AvailabilitySurvey& last,
                      const HeldFn& held) {
    std::vector<ObjectId> kept;
    std::copy_if(last.unavailable.begin(), last.unavailable.end(), std::back_inserter(kept),
                 [&](const ObjectId& id) { return !held(id); });
    std::vector<ObjectId> unavailable;
    std::set_union(next.unavailable.begin(), next.unavailable.end(), kept.begin(), kept.end(),
                   std::back_inserter(unavailable));
    std::vector<ObjectId> unknown;
    std::set_difference(next.unknown.begin(), next.unknown.end(), unavailable.begin(),
                        unavailable.end(), std::back_inserter(unknown));
    next.unavailable = std::move(unavailable);
    next.unknown = std::move(unknown);
}

AvailabilitySurvey survey_by_diff(const std::shared_ptr<const TrieUnion>& mine,
                                  const std::vector<ObjectId>& missing_here,
                                  const IdLookup& referenced, std::span<TrieSource* const> sources,
                                  std::vector<PeerDiff>& peers,
                                  const std::function<void()>& pause) {
    AvailabilitySurvey survey;
    std::vector<ObjectId> held_elsewhere;
    peers.assign(sources.size(), {});
    for (size_t i = 0; i < sources.size(); ++i) {
        ++survey.peers_asked;
        UnionSource here(mine);
        Recorded there(*sources[i]);
        std::vector<ObjectId> lacks;
        std::vector<ObjectId> holds;
        try {
            const auto cost = diff_tries(
                here, there,
                [&](const TrieDifference& difference) {
                    if (difference.left && !difference.right) {
                        if (referenced.contains(difference.id))
                            lacks.push_back(difference.id);
                    } else if (!difference.left && difference.right) {
                        holds.push_back(difference.id);
                    }
                },
                pause);
            survey.rounds += cost.rounds;
            survey.nodes_asked += cost.prefixes;
        } catch (const std::exception& error) {
            ++survey.peers_failed;
            Log::debug(std::string("availability: a peer could not finish a trie diff: ") +
                       error.what());
            continue;
        }
        std::sort(lacks.begin(), lacks.end());
        peers[i] = {true, there.root_hash(), std::move(lacks)};
        held_elsewhere.insert(held_elsewhere.end(), holds.begin(), holds.end());
    }
    sort_unique(held_elsewhere);
    std::set_difference(missing_here.begin(), missing_here.end(), held_elsewhere.begin(),
                        held_elsewhere.end(),
                        std::back_inserter(survey.peers_failed ? survey.unknown
                                                               : survey.unavailable));
    return survey;
}

} // namespace macha
