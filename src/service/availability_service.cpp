// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/availability_service.hpp"

#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/local_state.hpp"
#include "codec.hpp"
#include "filesystem/filesystem.hpp"
#include "log.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "observation.hpp"
#include "storage/local_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace macha {

namespace {

// The first release whose transport accepts tree_holdings.
constexpr std::tuple<unsigned, unsigned, unsigned> first_version{0, 82, 0};

// "0.82.1+abc" as (0, 82, 1); none if it is not three numbers.
std::optional<std::tuple<unsigned, unsigned, unsigned>> parse_version(std::string_view text) {
    unsigned parts[3]{};
    size_t at = 0;
    for (auto& part : parts) {
        const size_t start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (part > 100000)
                return {};
            part = part * 10 + static_cast<unsigned>(text[at] - '0');
            ++at;
        }
        if (at == start)
            return {};
        if (&part != &parts[2]) {
            if (at >= text.size() || text[at] != '.')
                return {};
            ++at;
        }
    }
    return std::tuple{parts[0], parts[1], parts[2]};
}

// A peer asked over RPC, in questions of at most tree_holdings_max ids.
class RemotePeer final : public PeerHoldings {
  public:
    RemotePeer(NodeRuntime& node, NodeInfo peer, bool supported)
        : node_(node), peer_(std::move(peer)), supported_(supported) {}

    std::vector<NodeHoldings> ask(std::span<const ObjectId> nodes) override {
        if (!supported_)
            throw std::runtime_error("peer does not answer tree_holdings");
        std::vector<NodeHoldings> answers;
        answers.reserve(nodes.size());
        for (size_t from = 0; from < nodes.size(); from += tree_holdings_max) {
            const auto part = nodes.subspan(from, std::min(tree_holdings_max, nodes.size() - from));
            const auto payload = encode_tree_holdings_request(part);
            const auto reply =
                node_.call(peer_, MessageType::tree_holdings, payload, FrameType::speculative);
            if (reply.message.type != MessageType::tree_holdings_reply)
                throw std::runtime_error("peer refused tree_holdings");
            auto decoded = decode_tree_holdings_reply(reply.message.payload);
            if (decoded.size() != part.size())
                throw std::runtime_error("peer answered a different number of tree nodes");
            std::move(decoded.begin(), decoded.end(), std::back_inserter(answers));
        }
        return answers;
    }

  private:
    NodeRuntime& node_;
    NodeInfo peer_;
    bool supported_;
};

// "/a/b/c" -> "/a/b"; "/a" -> "/"; "/" has no parent.
std::string_view parent_of(std::string_view path) {
    if (path.size() <= 1)
        return {};
    const auto slash = path.rfind('/');
    return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

} // namespace

void fill_path_table(AvailabilitySnapshot& out, const MetadataSnapshot& snapshot,
                     const NamespaceNodeStore& store, const HeldFn& held,
                     const std::function<void()>& pause) {
    for_each_namespace_entry(snapshot, &store, [&](const std::string& path, const FsEntry& entry) {
        if (pause)
            pause();
        auto& facts = out.paths[path];
        if (entry.type != EntryType::file) {
            facts.directory = true;
            return;
        }
        facts.size = entry.size;
        facts.hash = file_media_id(entry);
        for (const auto& extent : entry.extents) {
            if (extent.hole)
                continue;
            ++facts.extents;
            if (held(extent.id))
                ++facts.extents_local;
            else if (out.survey.is_unavailable(extent.id))
                ++facts.extents_unavailable;
            else if (out.survey.is_unknown(extent.id))
                ++facts.extents_unknown;
        }
        out.by_hash.emplace(facts.hash, path);
        const auto file = facts;
        for (auto parent = parent_of(path); !parent.empty(); parent = parent_of(parent)) {
            auto& above = out.paths[std::string(parent)];
            above.directory = true;
            above.size += file.size;
            above.extents += file.extents;
            above.extents_local += file.extents_local;
            above.extents_unavailable += file.extents_unavailable;
            above.extents_unknown += file.extents_unknown;
        }
    });
}

AvailabilityService::AvailabilityService(NodeRuntime& node, LocalState& local,
                                         DistributedStore& store, const ObjectLedger& ledger,
                                         const NodeEvents& events, MessageRoutes& routes)
    : node_(node), local_(local), store_(store), ledger_(ledger), events_(events),
      routes_(routes) {
    routes_.bind(MessageType::tree_holdings,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return answer(request);
                 });
}

AvailabilityService::~AvailabilityService() {
    routes_.unbind(MessageType::tree_holdings);
}

RpcMessage AvailabilityService::answer(const RpcMessage& request) const {
    const auto holdings = holdings_.handle();
    if (!holdings)
        return error_reply("holdings are not rolled up yet");
    // A loss since the roll-up: what it calls whole may not be. The pass
    // rolls up again; until then this node cannot answer.
    if (ledger_.held_losses(RetentionClass::data) != holdings->losses)
        return error_reply("holdings changed since they were rolled up");
    const auto nodes = decode_tree_holdings_request(request.payload);
    // This node's own copy of each tree node: answering never asks a peer.
    const LocalNamespaceNodeStore stored(local_.control());
    const NamespaceNodeStore& local_nodes =
        holdings->built ? static_cast<const NamespaceNodeStore&>(*holdings->built) : stored;
    const HeldFn held = [this](const ObjectId& id) {
        return ledger_.held(RetentionClass::data, id);
    };
    std::vector<NodeHoldings> answers;
    answers.reserve(nodes.size());
    for (const auto& node : nodes)
        answers.push_back(describe_holdings(holdings->rollup, local_nodes, held, node));
    return {MessageType::tree_holdings_reply, encode_tree_holdings_reply(answers)};
}

bool AvailabilityService::refresh(const MetadataSnapshotView& head, Clock::time_point now,
                                  uint64_t now_unix_ms, const std::function<void()>& pause) {
    if (!head.snapshot)
        return false;
    const bool tree_backed = head.snapshot->namespace_root.has_value();
    const auto head_key = tree_backed ? *head.snapshot->namespace_root : head.hash;
    const auto storage_events = events_.count(NodeEvent::storage);
    const auto topology_events = events_.count(NodeEvent::topology);
    const auto stored = ControlNamespaceNodeStore::for_reading(local_.control(), store_);
    const HeldFn held = [this](const ObjectId& id) {
        return ledger_.held(RetentionClass::data, id);
    };

    const auto refresh_started = Clock::now();
    auto holdings = holdings_.handle();
    bool rolled = false;
    const auto losses = ledger_.held_losses(RetentionClass::data);
    const bool lost = holdings && losses != holdings->losses;
    const bool head_changed = rolled_head_ != head_key;
    const bool changed = head_changed || storage_events != rolled_storage_events_ || lost;
    const auto rollup_due = rolled_at_ + rolled_cost_ * share;
    due_ = retry_at_;
    if (!holdings || changed) {
        if (ledger_.held_indexed(RetentionClass::data)) {
            cold_since_.reset();
        } else {
            if (!cold_since_)
                cold_since_ = now;
            if (now - *cold_since_ < cold_patience) {
                due_ = now + cold_retry;
                return false;
            }
        }
    }
    if (!holdings || (changed && now >= rollup_due)) {
        const auto started = Clock::now();
        Holdings next;
        next.losses = losses;
        if (tree_backed) {
            next.rollup = HoldingsRollup::build(head_key, stored, held, pause);
        } else {
            auto built = std::make_shared<MemoryNamespaceNodeStore>();
            const auto root = build_namespace_tree(head.snapshot->entries, *built);
            next.rollup = HoldingsRollup::build(root, *built, held, pause);
            next.built = std::move(built);
        }
        next.generation = head.generation;
        next.snapshot = head.snapshot;
        holdings_.publish(std::move(next));
        holdings = holdings_.handle();
        rolled_head_ = head_key;
        rolled_storage_events_ = storage_events;
        rolled_at_ = now;
        rolled = true;
        observations().record("availability.rollup_us", elapsed_us(started));
    }
    if (changed && !rolled) {
        due_ = due_ ? std::min(*due_, rollup_due) : rollup_due;
        // Overtaken by a loss: a survey from this roll-up could call
        // available what this node no longer holds.
        if (lost)
            return false;
    }
    const NamespaceNodeStore& nodes =
        holdings->built ? static_cast<const NamespaceNodeStore&>(*holdings->built) : stored;
    const auto* rollup = &holdings->rollup;

    std::vector<NodeInfo> hosts;
    std::vector<std::pair<NodeId, uint64_t>> peers;
    for (const auto& peer : node_.membership().active()) {
        if (peer.id == node_.node_id() || !node_hosts_extents(peer))
            continue;
        hosts.push_back(peer);
        peers.emplace_back(peer.id, peer.used);
    }
    std::sort(peers.begin(), peers.end());
    const auto previous = snapshot_.handle();
    const bool retry = retry_at_ && now >= *retry_at_;
    const bool same_peers =
        peers.size() == surveyed_peers_.size() &&
        std::equal(peers.begin(), peers.end(), surveyed_peers_.begin(),
                   [](const auto& a, const auto& b) { return a.first == b.first; });
    bool shrank = false;
    bool grew = false;
    for (size_t i = 0; same_peers && i < peers.size(); ++i) {
        shrank = shrank || peers[i].second < surveyed_peers_[i].second;
        grew = grew || peers[i].second > surveyed_peers_[i].second;
    }
    // What must be asked of the peers again, whatever it costs.
    const bool must_ask = !previous || head_changed || lost ||
                          topology_events != surveyed_topology_events_ || !same_peers || shrank;
    const bool missing = previous && (!previous->survey.unavailable.empty() ||
                                      !previous->survey.unknown.empty());
    // A peer holding more could hold something missing: worth asking, but a
    // peer that is importing grows all the time.
    const auto ask_due = surveyed_at_ + surveyed_cost_ * share;
    bool ask = must_ask || retry;
    if (!ask && grew && missing) {
        if (now >= ask_due)
            ask = true;
        else
            due_ = due_ ? std::min(*due_, ask_due) : ask_due;
    }
    if (!ask && !rolled) {
        if (!(grew && missing))
            surveyed_peers_ = std::move(peers); // compare next against these
        return false;
    }

    const auto telemetry = node_.telemetry().all();
    std::vector<RemotePeer> remotes;
    remotes.reserve(hosts.size());
    for (const auto& peer : hosts) {
        const auto seen = std::find_if(telemetry.begin(), telemetry.end(),
                                       [&](const auto& item) { return item.node_id == peer.id; });
        const auto version = seen == telemetry.end() ? std::nullopt : parse_version(seen->version);
        remotes.emplace_back(node_, peer, version && *version >= first_version);
    }
    std::vector<PeerHoldings*> asking;
    for (auto& remote : remotes)
        asking.push_back(&remote);

    // A peer that could not answer is tried with one tree node before the
    // whole descent is repeated for it.
    if (retry && !must_ask && !rolled) {
        const ObjectId root = rollup->root();
        bool answered = false;
        for (auto* peer : asking) {
            try {
                (void)peer->ask(std::span<const ObjectId>(&root, 1));
                answered = true;
            } catch (const std::exception&) {
            }
        }
        if (!answered) {
            retry_backoff_ =
                std::clamp<Clock::duration>(retry_backoff_ * 2, retry_floor, retry_ceiling);
            retry_at_ = now + retry_backoff_;
            due_ = due_ ? std::min(*due_, *retry_at_) : *retry_at_;
            return false;
        }
    }

    const auto started = Clock::now();
    AvailabilitySnapshot next;
    next.generation = holdings->generation;
    next.surveyed_unix_ms = now_unix_ms;
    if (ask) {
        next.survey = survey_availability(*rollup, nodes, held, asking);
        surveyed_at_ = now;
    } else {
        // Rolled up after a gain alone: what was missing is still missing,
        // less what this node now holds.
        next.survey = previous->survey;
        std::erase_if(next.survey.unavailable, held);
        std::erase_if(next.survey.unknown, held);
    }
    if (!rolled && previous && previous->survey.unavailable == next.survey.unavailable &&
        previous->survey.unknown == next.survey.unknown) {
        next.paths = previous->paths;
        next.by_hash = previous->by_hash;
    } else {
        fill_path_table(next, *holdings->snapshot, stored, held, pause);
    }
    if (ask)
        surveyed_cost_ = Clock::now() - started;
    if (rolled)
        rolled_cost_ = Clock::now() - refresh_started;
    observations().record("availability.survey_us", elapsed_us(started));
    Log::debug("availability surveyed generation=" + std::to_string(next.generation) +
               " extents=" + std::to_string(rollup->total().extents) +
               " local=" + std::to_string(rollup->total().held) +
               " unavailable=" + std::to_string(next.survey.unavailable.size()) +
               " unknown=" + std::to_string(next.survey.unknown.size()) +
               " peers=" + std::to_string(next.survey.peers_asked) +
               " peers_failed=" + std::to_string(next.survey.peers_failed) +
               " rounds=" + std::to_string(next.survey.rounds) +
               " tree_nodes_asked=" + std::to_string(next.survey.nodes_asked));
    if (!ask) {
        snapshot_.publish(std::move(next));
        return false;
    }
    if (next.survey.peers_failed) {
        retry_backoff_ = std::clamp<Clock::duration>(retry_backoff_ * 2, retry_floor, retry_ceiling);
        retry_at_ = now + retry_backoff_;
        due_ = due_ ? std::min(*due_, *retry_at_) : *retry_at_;
    } else {
        retry_backoff_ = {};
        retry_at_.reset();
    }
    snapshot_.publish(std::move(next));
    surveyed_topology_events_ = topology_events;
    surveyed_peers_ = std::move(peers);
    return true;
}

} // namespace macha
