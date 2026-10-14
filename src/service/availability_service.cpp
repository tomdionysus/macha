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

// The first release that answers trie_diff.
constexpr ReleaseVersion first_trie_version{0, 90, 72};

// A peer's held trie asked over RPC, every round of a diff from the snapshot
// its root question pinned.
class RemoteTrie final : public TrieSource {
  public:
    RemoteTrie(NodeRuntime& node, NodeInfo peer, bool supported)
        : node_(node), peer_(std::move(peer)), supported_(supported) {}

    ObjectTrie::Summary root() override {
        if (!supported_)
            throw std::runtime_error("peer does not answer trie_diff");
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
        for (size_t from = 0; from < p.size(); from += trie_records_max) {
            const auto part = p.subspan(from, std::min(trie_records_max, p.size() - from));
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
    bool supported_;
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
    routes_.bind(MessageType::trie_diff,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return answer_trie(request);
                 });
}

AvailabilityService::~AvailabilityService() {
    routes_.unbind(MessageType::trie_diff);
}

RpcMessage AvailabilityService::answer_trie(const RpcMessage& request) const {
    const auto question = decode_trie_question(request.payload);
    const auto now = Clock::now();
    if (question.kind == TrieQuestionKind::root) {
        auto view = ledger_.held_view(RetentionClass::data);
        if (!view.identity.complete || !view.trie)
            return error_reply(503, "holdings_seeding", "this node is still listing what it holds");
        auto& trie = view.trie;
        const auto summary = trie->root();
        Lock lock(pinned_mutex_);
        std::erase_if(pinned_, [&](const Pinned& pinned) {
            return pinned.hash == summary.hash || now - pinned.used > pinned_for;
        });
        if (pinned_.size() >= pinned_max)
            pinned_.erase(pinned_.begin());
        pinned_.push_back({summary.hash, std::move(trie), now});
        return {MessageType::trie_diff_reply, encode_trie_root(summary)};
    }
    std::shared_ptr<const TrieUnion> trie;
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
        return error_reply(410, "snapshot_gone", "the trie snapshot asked about is no longer pinned");
    if (question.kind == TrieQuestionKind::children)
        return {MessageType::trie_diff_reply,
                encode_trie_children(trie->children(question.prefixes))};
    // Records only where a diff reads them: below a leaf's worth.
    try {
        return {MessageType::trie_diff_reply,
                encode_trie_records(trie->records(question.prefixes, ObjectTrie::leaf_max))};
    } catch (const std::length_error&) {
        return error_reply(413, "prefix_too_large", "a prefix asked for holds more than a leaf");
    }
}

bool AvailabilityService::refresh(const MetadataSnapshotView& head, Clock::time_point now,
                                  uint64_t now_unix_ms, const std::function<void()>& pause) {
    if (!head.snapshot)
        return false;
    const auto topology_events = events_.count(NodeEvent::topology);
    const auto inventory = ledger_.inventory();
    auto view = ledger_.held_view(RetentionClass::data);
    due_ = retry_at_;
    // A disk being seeded: what this node holds cannot be named yet. The
    // referenced set is the inventory's: before the first there is nothing
    // to join.
    if (!view.identity.complete || !view.trie || !inventory) {
        due_ = due_ ? std::min(*due_, now + seed_retry) : now + seed_retry;
        return false;
    }
    const auto refresh_started = Clock::now();
    const bool lost = surveyed_ && view.identity.removals != joined_identity_.removals;
    const bool changed = !surveyed_ || inventory != joined_inventory_ ||
                         view.identity.hash != joined_identity_.hash || lost;
    const bool gained_only = surveyed_ && changed && !lost && inventory == joined_inventory_;

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
    const bool must_ask = !surveyed_ || topology_events != surveyed_topology_events_ ||
                          !same_peers || shrank || retry;
    const bool missing = surveyed_ && (!previous->survey.unavailable.empty() ||
                                      !previous->survey.unknown.empty());
    // What the inventory or this node's holdings changed, or a peer holding
    // more (which could hold something missing, but a peer that is importing
    // grows all the time), waits for the duty cycle.
    const auto survey_due = surveyed_at_ + surveyed_cost_ * share;
    const bool paced = changed || (grew && missing);
    if (!must_ask && paced && now < survey_due) {
        due_ = due_ ? std::min(*due_, survey_due) : survey_due;
        return false;
    }
    if (!must_ask && !paced) {
        surveyed_peers_ = std::move(peers); // compare next against these
        return false;
    }
    // A gain alone asks nobody: what this node gained was already available.
    const bool ask = must_ask || !gained_only || (grew && missing);

    const auto started = Clock::now();
    if (changed) {
        const auto joined = Clock::now();
        missing_.publish(
            inventory->not_held(RetentionClass::data, view.trie->parts(), pause));
        joined_inventory_ = inventory;
        joined_identity_ = view.identity;
        observations().record("availability.join_us", elapsed_us(joined));
    }
    const auto missing_here = missing_.handle();
    AvailabilitySnapshot next;
    next.generation = head.generation;
    next.surveyed_unix_ms = now_unix_ms;
    const HeldFn& held = view.held;
    bool failed = false;
    bool retry_needed = false;
    std::map<NodeId, Hash256> roots;
    if (ask) {
        // A host out of reach holds nothing reachable: with none in reach,
        // what this node lacks is unavailable until one comes back.
        const auto telemetry = node_.telemetry().all();
        std::vector<RemoteTrie> tries;
        tries.reserve(hosts.size());
        std::vector<TrieSource*> sources;
        std::vector<bool> supported;
        for (const auto& host : hosts) {
            supported.push_back(runs_at_least(telemetry, host.id, first_trie_version));
            sources.push_back(&tries.emplace_back(node_, host, supported.back()));
        }
        std::vector<PeerDiff> diffs;
        next.survey = survey_by_diff(view.trie, *missing_here,
                                     inventory->lookup(RetentionClass::data), sources, diffs,
                                     pause);
        const auto elapsed = Clock::now() - started;
        observations().record("availability.diff_us",
                              static_cast<uint64_t>(std::chrono::duration_cast<
                                  std::chrono::microseconds>(elapsed).count()));
        PeerLacks lacks;
        lacks.generation = inventory->generation();
        lacks.complete = true;
        for (size_t i = 0; i < hosts.size(); ++i) {
            const auto& spent = tries[i].spent();
            Log::debug("availability diff " +
                       std::string(diffs[i].answered ? "done" : "failed") +
                       " peer=" + to_string(hosts[i].id).substr(0, 12) +
                       " questions=" + std::to_string(spent.questions) +
                       " reply_bytes=" + std::to_string(spent.bytes) +
                       " waited_ms=" +
                       std::to_string(
                           std::chrono::duration_cast<std::chrono::milliseconds>(spent.waited)
                               .count()));
            if (!diffs[i].answered) {
                lacks.complete = false;
                // A peer too old to answer is asked again when the membership
                // changes, not on a timer.
                retry_needed = retry_needed || supported[i];
                continue;
            }
            roots.emplace(hosts[i].id, diffs[i].root);
            lacks.lacks.emplace(hosts[i].id, std::make_shared<const std::vector<ObjectId>>(
                                                 std::move(diffs[i].lacks)));
        }
        failed = next.survey.peers_failed > 0;
        if (failed && surveyed_)
            keep_unavailable(next.survey, previous->survey, held);
        peer_lacks_.publish(std::move(lacks));
        surveyed_roots_ = failed ? std::map<NodeId, Hash256>{} : std::move(roots);
        surveyed_at_ = now;
    } else {
        // Gained alone: what was missing is still missing, less what this
        // node now holds.
        next.survey = previous->survey;
        const auto kept = [&](std::vector<ObjectId>& ids) {
            std::vector<ObjectId> out;
            std::set_intersection(ids.begin(), ids.end(), missing_here->begin(),
                                  missing_here->end(), std::back_inserter(out));
            ids = std::move(out);
        };
        kept(next.survey.unavailable);
        kept(next.survey.unknown);
    }
    const auto stored = ControlNamespaceNodeStore::for_reading(local_.control(), store_);
    const auto& root = head.snapshot->namespace_root;
    const char* table = "kept";
    bool table_changed = false;
    if (!changed && surveyed_ && previous->survey.unavailable == next.survey.unavailable &&
        previous->survey.unknown == next.survey.unknown) {
        next.paths = previous->paths;
        next.by_hash = previous->by_hash;
    } else {
        // Only the tree moved: with the same holdings everywhere, no extent of
        // an unchanged file has changed hands, so the table follows the tree.
        if (surveyed_ && root && table_root_ && view.identity.hash == table_holdings_ &&
            surveyed_roots_.size() == hosts.size() && surveyed_roots_ == table_roots_ &&
            previous->survey.unknown.empty() && next.survey.unknown.empty()) {
            update_path_table(next, *previous, diff_namespace_trees(*table_root_, *root, stored),
                              *head.snapshot, stored, held);
            table = "followed";
        } else {
            fill_path_table(next, *head.snapshot, stored, held, previous.get(), pause);
            table = "walked";
        }
        table_root_ = root;
        table_holdings_ = view.identity.hash;
        table_roots_ = surveyed_roots_;
        table_changed = true;
    }
    if (ask || changed)
        surveyed_cost_ = Clock::now() - refresh_started;
    observations().record("availability.survey_us", elapsed_us(started));
    const auto referenced = inventory->size(RetentionClass::data);
    Log::debug("availability surveyed generation=" + std::to_string(next.generation) +
               " referenced=" + std::to_string(referenced) +
               " local=" + std::to_string(referenced - missing_here->size()) +
               " unavailable=" + std::to_string(next.survey.unavailable.size()) +
               " unknown=" + std::to_string(next.survey.unknown.size()) +
               " peers=" + std::to_string(next.survey.peers_asked) +
               " peers_failed=" + std::to_string(next.survey.peers_failed) +
               " rounds=" + std::to_string(next.survey.rounds) +
               " prefixes_asked=" + std::to_string(next.survey.nodes_asked) +
               " table=" + table + " method=" + (ask ? "diff" : "gained"));
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
    snapshot_.publish(std::move(next));
    if (!ask)
        return false;
    if (retry_needed) {
        retry_backoff_ = std::clamp<Clock::duration>(retry_backoff_ * 2, retry_floor, retry_ceiling);
        retry_at_ = now + retry_backoff_;
        due_ = due_ ? std::min(*due_, *retry_at_) : *retry_at_;
    } else {
        retry_backoff_ = {};
        retry_at_.reset();
    }
    surveyed_ = true;
    surveyed_topology_events_ = topology_events;
    surveyed_peers_ = std::move(peers);
    return true;
}

} // namespace macha
