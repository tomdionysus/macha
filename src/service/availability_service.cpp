// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/availability_service.hpp"

#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/local_state.hpp"
#include "codec.hpp"
#include "durable_file.hpp"
#include "filesystem/filesystem.hpp"
#include "log.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "observation.hpp"
#include "storage/local_store.hpp"

#include <algorithm>
#include <set>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <tuple>

namespace macha {

namespace {

// The first release whose transport accepts tree_holdings.
constexpr std::tuple<unsigned, unsigned, unsigned> first_version{0, 82, 0};
// The first release that answers trie_diff.
constexpr std::tuple<unsigned, unsigned, unsigned> first_trie_version{0, 90, 72};

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
            if (reply.message.type != MessageType::tree_holdings_reply) {
                std::string reason = "no reason given";
                if (reply.message.type == MessageType::error) {
                    try {
                        Reader why(reply.message.payload);
                        reason = why.string();
                    } catch (const std::exception&) {
                    }
                }
                throw std::runtime_error("peer refused tree_holdings: " + reason);
            }
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

// A peer's held trie asked over RPC, every round of a diff from the snapshot
// its root question pinned.
class RemoteTrie final : public TrieSource {
  public:
    RemoteTrie(NodeRuntime& node, NodeInfo peer) : node_(node), peer_(std::move(peer)) {}

    ObjectTrie::Summary root() override {
        const auto summary = decode_trie_root(ask(question(TrieQuestionKind::root, {})));
        at_ = summary.hash;
        return summary;
    }
    std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override {
        std::vector<ObjectTrie::Children> out;
        for (size_t from = 0; from < p.size(); from += trie_question_max) {
            const auto part = p.subspan(from, std::min(trie_question_max, p.size() - from));
            auto answers = decode_trie_children(ask(question(TrieQuestionKind::children, part)));
            if (answers.size() != part.size())
                throw std::runtime_error("peer answered a different number of prefixes");
            std::move(answers.begin(), answers.end(), std::back_inserter(out));
        }
        return out;
    }
    std::vector<std::vector<ObjectTrie::Record>>
    records(std::span<const ObjectTrie::Prefix> p) override {
        std::vector<std::vector<ObjectTrie::Record>> out;
        for (size_t from = 0; from < p.size(); from += trie_question_max) {
            const auto part = p.subspan(from, std::min(trie_question_max, p.size() - from));
            auto answers = decode_trie_records(ask(question(TrieQuestionKind::records, part)));
            if (answers.size() != part.size())
                throw std::runtime_error("peer answered a different number of prefixes");
            std::move(answers.begin(), answers.end(), std::back_inserter(out));
        }
        return out;
    }

    TrieQuestion question(TrieQuestionKind kind, std::span<const ObjectTrie::Prefix> p) const {
        TrieQuestion out;
        out.kind = kind;
        out.at = at_;
        out.prefixes.assign(p.begin(), p.end());
        return out;
    }
    // What asking cost: questions sent, reply bytes, and time waiting for
    // the peer's answers.
    struct Spent {
        uint64_t questions{};
        uint64_t bytes{};
        Clock::duration waited{};
    };
    const Spent& spent() const noexcept { return spent_; }

  private:
    Bytes ask(const TrieQuestion& question) {
        const auto started = Clock::now();
        const auto reply = node_.call(peer_, MessageType::trie_diff,
                                      encode_trie_question(question), FrameType::speculative);
        ++spent_.questions;
        spent_.bytes += reply.message.payload.size();
        spent_.waited += Clock::now() - started;
        if (reply.message.type != MessageType::trie_diff_reply) {
            std::string reason = "no reason given";
            if (reply.message.type == MessageType::error) {
                try {
                    Reader why(reply.message.payload);
                    reason = why.string();
                } catch (const std::exception&) {
                }
            }
            throw std::runtime_error("peer refused trie_diff: " + reason);
        }
        return reply.message.payload;
    }

    NodeRuntime& node_;
    NodeInfo peer_;
    Hash256 at_{};
    Spent spent_;
};

// "/a/b/c" -> "/a/b"; "/a" -> "/"; "/" has no parent.
std::string_view parent_of(std::string_view path) {
    if (path.size() <= 1)
        return {};
    const auto slash = path.rfind('/');
    return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

// A file's own facts against a survey.
PathAvailability file_facts(const FsEntry& entry, const AvailabilitySurvey& survey,
                            const HeldFn& held, const AvailabilitySnapshot* last_known) {
    PathAvailability facts;
    {
        facts.size = entry.size;
        facts.hash = file_media_id(entry);
        for (const auto& extent : entry.extents) {
            if (extent.hole)
                continue;
            ++facts.extents;
            if (held(extent.id))
                ++facts.extents_local;
            else if (survey.is_unavailable(extent.id))
                ++facts.extents_unavailable;
            else if (survey.is_unknown(extent.id))
                ++facts.extents_unknown;
        }
        if (facts.extents_unknown && last_known)
            if (const auto known = last_known->by_hash.find(facts.hash);
                known != last_known->by_hash.end())
                if (const auto was = last_known->paths.find(known->second);
                    was != last_known->paths.end() && was->second.hash == facts.hash &&
                    was->second.extents == facts.extents && !was->second.extents_unknown) {
                    facts.extents_unavailable =
                        std::max(facts.extents_unavailable,
                                 std::min(was->second.extents_unavailable,
                                          facts.extents - facts.extents_local));
                    facts.extents_unknown = 0;
                }
    }
    return facts;
}

void add_file(AvailabilitySnapshot& out, const std::string& path, const PathAvailability& file) {
    out.paths[path] = file;
    out.by_hash.emplace(file.hash, path);
    for (auto parent = parent_of(path); !parent.empty(); parent = parent_of(parent)) {
        auto& above = out.paths[std::string(parent)];
        above.directory = true;
        above.size += file.size;
        above.extents += file.extents;
        above.extents_local += file.extents_local;
        above.extents_unavailable += file.extents_unavailable;
        above.extents_unknown += file.extents_unknown;
    }
}

} // namespace

void fill_path_table(AvailabilitySnapshot& out, const MetadataSnapshot& snapshot,
                     const NamespaceNodeStore& store, const HeldFn& held,
                     const AvailabilitySnapshot* last_known, const std::function<void()>& pause) {
    for_each_namespace_entry(snapshot, &store, [&](const std::string& path, const FsEntry& entry) {
        if (pause)
            pause();
        if (entry.type != EntryType::file)
            out.paths[path].directory = true;
        else
            add_file(out, path, file_facts(entry, out.survey, held, last_known));
    });
}

void update_path_table(AvailabilitySnapshot& out, const AvailabilitySnapshot& previous,
                       const NamespaceDifferences& changes, const MetadataSnapshot& snapshot,
                       const NamespaceNodeStore& store, const HeldFn& held) {
    out.paths = previous.paths;
    out.by_hash = previous.by_hash;
    // Directories that may have lost their reason to be listed.
    std::set<std::string, std::greater<>> emptied;
    for (const auto& [path, change] : changes) {
        if (!change.before)
            continue;
        if (change.before->type != EntryType::file) {
            if (!change.after)
                emptied.insert(path);
            continue;
        }
        const auto found = out.paths.find(path);
        if (found == out.paths.end())
            continue;
        const auto file = found->second;
        out.paths.erase(found);
        for (auto [at, end] = out.by_hash.equal_range(file.hash); at != end; ++at)
            if (at->second == path) {
                out.by_hash.erase(at);
                break;
            }
        for (auto parent = parent_of(path); !parent.empty(); parent = parent_of(parent)) {
            const auto above = out.paths.find(parent);
            if (above == out.paths.end())
                continue;
            above->second.size -= file.size;
            above->second.extents -= file.extents;
            above->second.extents_local -= file.extents_local;
            above->second.extents_unavailable -= file.extents_unavailable;
            above->second.extents_unknown -= file.extents_unknown;
            emptied.insert(std::string(parent));
        }
    }
    for (const auto& [path, change] : changes) {
        if (!change.after)
            continue;
        if (change.after->type != EntryType::file)
            out.paths[path].directory = true;
        else
            add_file(out, path, file_facts(*change.after, out.survey, held, nullptr));
    }
    // Deepest first. A directory is listed for its own entry or for a file
    // beneath it.
    for (const auto& path : emptied) {
        const auto found = out.paths.find(path);
        if (found == out.paths.end() || !found->second.directory ||
            namespace_contains(snapshot, &store, path))
            continue;
        const auto prefix = path == "/" ? path : path + "/";
        bool file_beneath = false;
        for (auto at = std::next(found);
             at != out.paths.end() && at->first.starts_with(prefix) && !file_beneath; ++at)
            file_beneath = !at->second.directory;
        if (!file_beneath)
            out.paths.erase(found);
    }
}

namespace {

constexpr uint32_t persisted_schema = 1;

} // namespace

Bytes encode_availability_paths(const AvailabilitySnapshot& snapshot) {
    Writer out;
    out.u32(persisted_schema);
    out.u64(snapshot.generation);
    out.u64(snapshot.surveyed_unix_ms);
    out.u64(snapshot.paths.size());
    for (const auto& [path, facts] : snapshot.paths) {
        out.string(path);
        out.u8(facts.directory ? 1 : 0);
        out.u64(facts.size);
        out.u64(facts.extents);
        out.u64(facts.extents_local);
        out.u64(facts.extents_unavailable);
        out.u64(facts.extents_unknown);
        out.string(facts.hash);
    }
    return out.take();
}

AvailabilitySnapshot decode_availability_paths(std::span<const uint8_t> bytes) {
    Reader in(bytes);
    if (in.u32() != persisted_schema)
        throw DecodeError("unknown availability schema");
    AvailabilitySnapshot snapshot;
    snapshot.generation = in.u64();
    snapshot.surveyed_unix_ms = in.u64();
    const auto count = in.u64();
    for (uint64_t i = 0; i < count; ++i) {
        auto path = in.string();
        PathAvailability facts;
        const auto directory = in.u8();
        if (directory > 1)
            throw DecodeError("bad availability path kind");
        facts.directory = directory == 1;
        facts.size = in.u64();
        facts.extents = in.u64();
        facts.extents_local = in.u64();
        facts.extents_unavailable = in.u64();
        facts.extents_unknown = in.u64();
        facts.hash = in.string();
        if (facts.extents_local > facts.extents ||
            facts.extents_unavailable > facts.extents - facts.extents_local ||
            facts.extents_unknown >
                facts.extents - facts.extents_local - facts.extents_unavailable)
            throw DecodeError("availability counts exceed the extents");
        if (!facts.directory)
            snapshot.by_hash.emplace(facts.hash, path);
        if (!snapshot.paths.emplace(std::move(path), std::move(facts)).second)
            throw DecodeError("availability path repeated");
    }
    in.finish();
    return snapshot;
}

AvailabilityService::AvailabilityService(NodeRuntime& node, LocalState& local,
                                         DistributedStore& store, const ObjectLedger& ledger,
                                         const NodeEvents& events, MessageRoutes& routes,
                                         std::filesystem::path persisted)
    : node_(node), local_(local), store_(store), ledger_(ledger), events_(events),
      routes_(routes), persisted_(std::move(persisted)) {
    // No roll-up is kept across a restart: one an earlier release left goes.
    if (!persisted_.empty()) {
        std::error_code ec;
        std::filesystem::remove(persisted_.parent_path() / "holdings.bin", ec);
    }
    if (!persisted_.empty()) {
        std::ifstream in(persisted_, std::ios::binary);
        if (in) {
            const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
            try {
                snapshot_.publish(decode_availability_paths(bytes));
            } catch (const DecodeError& error) {
                Log::warn("availability: ignoring " + persisted_.string() + ": " + error.what());
            }
        }
    }
    routes_.bind(MessageType::tree_holdings,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return answer(request);
                 });
    routes_.bind(MessageType::trie_diff,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return answer_trie(request);
                 });
}

AvailabilityService::~AvailabilityService() {
    routes_.unbind(MessageType::trie_diff);
    routes_.unbind(MessageType::tree_holdings);
}

RpcMessage AvailabilityService::answer_trie(const RpcMessage& request) const {
    const auto question = decode_trie_question(request.payload);
    const auto now = Clock::now();
    if (question.kind == TrieQuestionKind::root) {
        auto view = ledger_.held_view(RetentionClass::data);
        if (!view.identity.complete)
            return error_reply("holdings_seeding");
        if (view.tries.size() != 1)
            return error_reply("several_held_tries");
        auto& trie = view.tries.front();
        const ObjectTrie::Summary summary{trie.size(), trie.root_hash()};
        Lock lock(pinned_mutex_);
        std::erase_if(pinned_, [&](const Pinned& pinned) {
            return pinned.hash == summary.hash || now - pinned.used > pinned_for;
        });
        if (pinned_.size() >= pinned_max)
            pinned_.erase(pinned_.begin());
        pinned_.push_back({summary.hash, std::move(trie), now});
        return {MessageType::trie_diff_reply, encode_trie_root(summary)};
    }
    std::optional<ObjectTrie::Snapshot> trie;
    {
        Lock lock(pinned_mutex_);
        std::erase_if(pinned_,
                      [&](const Pinned& pinned) { return now - pinned.used > pinned_for; });
        for (auto& pinned : pinned_)
            if (pinned.hash == question.at) {
                pinned.used = now;
                trie = pinned.trie;
            }
    }
    if (!trie)
        return error_reply("snapshot_gone");
    if (question.kind == TrieQuestionKind::children)
        return {MessageType::trie_diff_reply,
                encode_trie_children(trie->children(question.prefixes))};
    // Records only where a diff reads them: below a leaf's worth.
    for (const auto& prefix : question.prefixes) {
        if (prefix.length >= 32)
            continue;
        uint64_t below = 0;
        const auto children = trie->children(std::span(&prefix, 1));
        for (const auto& slot : children.front())
            below += slot.count;
        if (below > ObjectTrie::leaf_max)
            return error_reply("prefix_too_large");
    }
    return {MessageType::trie_diff_reply, encode_trie_records(trie->records(question.prefixes))};
}

RpcMessage AvailabilityService::answer(const RpcMessage& request) const {
    const auto holdings = holdings_.handle();
    if (!holdings)
        return error_reply("holdings are not rolled up yet");
    // Something stopped being held since the roll-up: what it calls whole may
    // not be. The pass rolls up again; until then this node cannot answer.
    if (ledger_.held_view(RetentionClass::data).identity.removals !=
        holdings->identity.removals)
        return error_reply("holdings changed since they were rolled up");
    const auto* rollup = &holdings->rollup;
    const auto nodes = decode_tree_holdings_request(request.payload);
    // This node's own copy of each tree node: answering never asks a peer.
    const LocalNamespaceNodeStore stored(local_.control());
    const NamespaceNodeStore& local_nodes =
        holdings && holdings->built ? static_cast<const NamespaceNodeStore&>(*holdings->built)
                                    : stored;
    // The view the roll-up was made from, so the two agree.
    const HeldFn& held = holdings->held;
    std::vector<NodeHoldings> answers;
    answers.reserve(nodes.size());
    for (const auto& node : nodes)
        answers.push_back(describe_holdings(*rollup, local_nodes, held, node));
    return {MessageType::tree_holdings_reply, encode_tree_holdings_reply(answers)};
}

bool AvailabilityService::refresh(const MetadataSnapshotView& head, Clock::time_point now,
                                  uint64_t now_unix_ms, const std::function<void()>& pause) {
    if (!head.snapshot)
        return false;
    const bool tree_backed = head.snapshot->namespace_root.has_value();
    const auto head_key = tree_backed ? *head.snapshot->namespace_root : head.hash;
    const auto topology_events = events_.count(NodeEvent::topology);
    const auto stored = ControlNamespaceNodeStore::for_reading(local_.control(), store_);

    const auto refresh_started = Clock::now();
    auto holdings = holdings_.handle();
    bool rolled = false;
    auto view = ledger_.held_view(RetentionClass::data);
    // Something held at the roll-up may no longer be.
    const bool lost = holdings && view.identity.removals != holdings->identity.removals;
    const bool head_changed = rolled_head_ != head_key;
    const bool holdings_changed = holdings && view.identity.hash != holdings->identity.hash;
    const bool changed = head_changed || holdings_changed || lost;
    const auto rollup_due = rolled_at_ + rolled_cost_ * share;
    due_ = retry_at_;
    // A disk being seeded: what this node holds cannot be named yet.
    if (!view.identity.complete) {
        due_ = due_ ? std::min(*due_, now + seed_retry) : now + seed_retry;
        if (!holdings || changed)
            return false;
    }
    if (view.identity.complete && (!holdings || (changed && now >= rollup_due))) {
        const auto started = Clock::now();
        Holdings next;
        next.identity = view.identity;
        next.held = view.held;
        next.tries = view.tries;
        if (tree_backed) {
            // With the same holdings as the last roll-up, only the tree has
            // moved: what the two trees share keeps its count.
            const bool carry = holdings && !holdings->built && !holdings_changed && !lost;
            next.rollup = HoldingsRollup::build(head_key, stored, next.held, pause,
                                                carry ? &holdings->rollup : nullptr);
            next.missing = std::make_shared<const std::vector<ObjectId>>(
                missing_extents(next.rollup, stored, next.held, pause));
        } else {
            auto built = std::make_shared<MemoryNamespaceNodeStore>();
            const auto root = build_namespace_tree(head.snapshot->entries, *built);
            next.rollup = HoldingsRollup::build(root, *built, next.held, pause);
            next.missing = std::make_shared<const std::vector<ObjectId>>(
                missing_extents(next.rollup, *built, next.held, pause));
            next.built = std::move(built);
        }
        next.generation = head.generation;
        next.snapshot = head.snapshot;
        holdings_.publish(std::move(next));
        holdings = holdings_.handle();
        rolled_head_ = head_key;
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
    const HeldFn& held = holdings->held;
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
    // The tree is asked about again once it has been rolled up, not while a
    // roll-up of it is still due: until then the survey would be of the tree
    // already surveyed.
    const bool must_ask = !surveyed_ || rollup->root() != surveyed_root_ || lost ||
                          topology_events != surveyed_topology_events_ || !same_peers || shrank;
    const bool missing = surveyed_ && (!previous->survey.unavailable.empty() ||
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

    // Asking nobody proves nothing. With every other host this node knows of
    // out of reach, a survey would call all it lacks unavailable; it waits
    // for one to come back and keeps the last answer. A node that knows no
    // other host surveys alone.
    if (hosts.empty()) {
        const auto known = node_.membership().all();
        const bool others = std::any_of(known.begin(), known.end(), [&](const NodeInfo& peer) {
            return peer.id != node_.node_id() && node_hosts_extents(peer);
        });
        if (others) {
            retry_backoff_ =
                std::clamp<Clock::duration>(retry_backoff_ * 2, retry_floor, retry_ceiling);
            retry_at_ = now + retry_backoff_;
            due_ = due_ ? std::min(*due_, *retry_at_) : *retry_at_;
            return false;
        }
    }

    const auto telemetry = node_.telemetry().all();
    std::vector<RemotePeer> remotes;
    remotes.reserve(hosts.size());
    // Every peer answers a trie diff and this node has one held trie.
    bool diffable = holdings->tries.size() == 1;
    for (const auto& peer : hosts) {
        const auto seen = std::find_if(telemetry.begin(), telemetry.end(),
                                       [&](const auto& item) { return item.node_id == peer.id; });
        const auto version = seen == telemetry.end() ? std::nullopt : parse_version(seen->version);
        remotes.emplace_back(node_, peer, version && *version >= first_version);
        diffable = diffable && version && *version >= first_trie_version;
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
    // What the last survey settled stands while every peer holds what it
    // held and this node has lost nothing.
    const bool remembered = ask && memo_ && surveyed_ && !lost && same_peers && !shrank &&
                            !(grew && missing) && topology_events == surveyed_topology_events_;
    const char* method = "none";
    if (ask) {
        // Each peer's held trie against this node's, when every peer can be
        // asked so; otherwise each peer is asked about the namespace tree.
        PeerLacks lacks;
        std::optional<AvailabilitySurvey> diffed;
        const auto inventory = ledger_.inventory();
        if (!diffable)
            method = holdings->tries.size() == 1 ? "tree:peer_version" : "tree:held_tries";
        else if (!inventory)
            method = "tree:no_inventory";
        if (diffable && inventory && holdings->missing) {
            std::vector<RemoteTrie> tries;
            tries.reserve(hosts.size());
            std::vector<TrieSource*> sources;
            for (const auto& host : hosts)
                sources.push_back(&tries.emplace_back(node_, host));
            std::vector<std::vector<ObjectId>> found;
            const auto diff_started = Clock::now();
            const auto report = [&](const char* outcome) {
                const auto elapsed = Clock::now() - diff_started;
                observations().record("availability.diff_us",
                                      static_cast<uint64_t>(std::chrono::duration_cast<
                                          std::chrono::microseconds>(elapsed).count()));
                for (size_t i = 0; i < tries.size(); ++i) {
                    const auto& spent = tries[i].spent();
                    Log::debug("availability diff " + std::string(outcome) +
                               " peer=" + to_string(hosts[i].id).substr(0, 12) +
                               " questions=" + std::to_string(spent.questions) +
                               " reply_bytes=" + std::to_string(spent.bytes) +
                               " waited_ms=" + std::to_string(std::chrono::duration_cast<
                                   std::chrono::milliseconds>(spent.waited).count()) +
                               " elapsed_ms=" + std::to_string(std::chrono::duration_cast<
                                   std::chrono::milliseconds>(elapsed).count()));
                }
            };
            try {
                diffed = survey_by_diff(holdings->tries.front(), *holdings->missing,
                                        inventory->lookup(RetentionClass::data), sources, found,
                                        pause);
                lacks.generation = inventory->generation();
                for (size_t i = 0; i < hosts.size(); ++i)
                    lacks.lacks.emplace(hosts[i].id, std::make_shared<const std::vector<ObjectId>>(
                                                         std::move(found[i])));
                report("done");
            } catch (const std::exception& error) {
                report("failed");
                diffed.reset();
                lacks = {};
                method = "tree:diff_failed";
                Log::debug(std::string("availability: a trie diff failed, asking about the tree: ") +
                           error.what());
            }
        }
        if (diffed) {
            next.survey = std::move(*diffed);
            memo_.reset();
            method = "diff";
        } else {
            if (std::string_view(method) == "none")
                method = "tree";
            SurveyMemo made;
            next.survey = survey_availability(*rollup, nodes, held, asking,
                                              remembered ? &*memo_ : nullptr, &made);
            if (next.survey.peers_failed)
                memo_.reset();
            else
                memo_ = std::move(made);
            // What each peer lacks of what this node holds, from the peer's
            // own account of its holdings.
            lacks.generation = holdings->generation;
            for (size_t i = 0; i < remotes.size(); ++i)
                if (auto found = extents_peer_lacks(*rollup, nodes, held, remotes[i]))
                    lacks.lacks.emplace(hosts[i].id, std::make_shared<const std::vector<ObjectId>>(
                                                         std::move(*found)));
        }
        peer_lacks_.publish(std::move(lacks));
        surveyed_at_ = now;
    } else {
        // Rolled up after a gain alone: what was missing is still missing,
        // less what this node now holds.
        next.survey = previous->survey;
        std::erase_if(next.survey.unavailable, held);
        std::erase_if(next.survey.unknown, held);
    }
    bool table_changed = false;
    const char* table = "kept";
    if (!rolled && surveyed_ && previous->survey.unavailable == next.survey.unavailable &&
        previous->survey.unknown == next.survey.unknown) {
        next.paths = previous->paths;
        next.by_hash = previous->by_hash;
    } else {
        const auto& root = holdings->snapshot->namespace_root;
        // Only the tree moved: the table follows what moved in it.
        if (remembered && !holdings->built && root && table_root_ &&
            holdings->identity.hash == table_holdings_ && previous->survey.unknown.empty() &&
            next.survey.unknown.empty()) {
            update_path_table(next, *previous, diff_namespace_trees(*table_root_, *root, stored),
                              *holdings->snapshot, stored, held);
            table = "followed";
        } else {
            fill_path_table(next, *holdings->snapshot, stored, held, previous.get(), pause);
            table = "walked";
        }
        table_root_ = holdings->built ? std::nullopt : root;
        table_holdings_ = holdings->identity.hash;
        table_changed = true;
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
               " tree_nodes_asked=" + std::to_string(next.survey.nodes_asked) +
               " remembered=" + (remembered ? "1" : "0") + " table=" + table +
               " method=" + method);
    if (table_changed && !persisted_.empty()) {
        try {
            const auto bytes = encode_availability_paths(next);
            durable_replace_file(persisted_,
                                 std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                  bytes.size()));
        } catch (const std::exception& error) {
            Log::warn(std::string("availability: cannot keep the survey: ") + error.what());
        }
    }
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
    surveyed_ = true;
    surveyed_root_ = rollup->root();
    surveyed_topology_events_ = topology_events;
    surveyed_peers_ = std::move(peers);
    return true;
}

} // namespace macha
