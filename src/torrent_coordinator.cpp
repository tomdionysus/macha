// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent_coordinator.hpp"

#include "cluster.hpp"
#include "durable_file.hpp"
#include "http.hpp"
#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace macha {
namespace {

// Thrown inside a metadata mutation that finds, on the current snapshot, that
// it has nothing to do.
struct Unchanged {};

void put(MetadataSnapshot& snapshot, MetadataDelta& delta, const TorrentRequest& request) {
    snapshot.torrent_requests[request.id] = request;
    delta.upsert_torrent_requests[request.id] = request;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool all_hex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::optional<std::string> base32_to_hex(std::string_view value) {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string bytes;
    uint64_t buffer = 0;
    int bits = 0;
    for (char c : value) {
        const auto at = alphabet.find(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (at == std::string_view::npos) return std::nullopt;
        buffer = (buffer << 5) | at;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char b : bytes) {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 15]);
    }
    return hex;
}

// A magnet's values for one key, URL-decoded: sanitize_magnet_uri encodes
// every value it writes (urn%3Abtih%3A...).
std::vector<std::string> magnet_params(std::string_view magnet, std::string_view key) {
    std::vector<std::string> out;
    const auto query = magnet.find('?');
    if (query == std::string_view::npos) return out;
    auto rest = magnet.substr(query + 1);
    while (!rest.empty()) {
        const auto amp = rest.find('&');
        const auto pair = rest.substr(0, amp);
        if (const auto eq = pair.find('='); eq != std::string_view::npos && pair.substr(0, eq) == key)
            out.push_back(http_url_decode(pair.substr(eq + 1)));
        if (amp == std::string_view::npos) break;
        rest.remove_prefix(amp + 1);
    }
    return out;
}

// Rendezvous weight of a node for a request: the same on every node, so
// healthy nodes agree which of them should claim first.
uint64_t rendezvous(std::string_view id, const NodeId& node) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](unsigned char c) {
        h ^= c;
        h *= 1099511628211ull;
    };
    for (char c : id) mix(static_cast<unsigned char>(c));
    for (auto b : node.bytes) mix(b);
    return h;
}

Json::Object intent_request(std::string_view id, TorrentDesired desired, uint64_t changed) {
    Json::Object out;
    out["job_id"] = std::string(id);
    out["desired"] = std::string(torrent_desired_name(desired));
    out["changed_unix_ms"] = changed;
    return out;
}

std::optional<TorrentDesired> parse_desired(std::string_view name) {
    for (auto d : {TorrentDesired::active, TorrentDesired::paused, TorrentDesired::cancelled})
        if (torrent_desired_name(d) == name) return d;
    return std::nullopt;
}

} // namespace

TorrentPhase torrent_phase_of(TorrentJobState state) {
    switch (state) {
    case TorrentJobState::importing:
    case TorrentJobState::cataloguing:
        return TorrentPhase::importing;
    case TorrentJobState::completed:
        return TorrentPhase::completed;
    case TorrentJobState::cancelled:
        return TorrentPhase::cancelled;
    case TorrentJobState::failed:
        return TorrentPhase::failed;
    default:
        return TorrentPhase::downloading;
    }
}

std::string magnet_info_hash(std::string_view magnet) {
    std::string v2;
    for (const auto& param : magnet_params(magnet, "xt")) {
        const std::string_view xt = param;
        if (xt.starts_with("urn:btih:")) {
            const auto value = xt.substr(9);
            if (value.size() == 40 && all_hex(value)) return lower(std::string(value));
            if (value.size() == 32)
                if (auto hex = base32_to_hex(value); hex && hex->size() == 40) return *hex;
        } else if (xt.starts_with("urn:btmh:1220")) {
            const auto value = xt.substr(13);
            if (value.size() == 64 && all_hex(value)) v2 = lower(std::string(value));
        }
    }
    return v2;
}

std::string magnet_display_name(std::string_view magnet) {
    const auto names = magnet_params(magnet, "dn");
    if (names.empty()) return {};
    auto decoded = names.front();
    std::replace(decoded.begin(), decoded.end(), '+', ' ');
    return decoded.substr(0, 1024);
}

TorrentCoordinator::TorrentCoordinator(NodeRuntime& node, MetadataManager& metadata, SubsystemRegistry& registry,
                                       ClusterJobView& view, const std::filesystem::path& state_path,
                                       std::chrono::milliseconds claim_lease)
    : node_(node), metadata_(metadata), registry_(registry), view_(view), claim_lease_(claim_lease),
      intents_path_(state_path / "torrent" / "intents.json") {
    load_intents();
    node_.set_torrent_intent_handler([this](std::span<const uint8_t> payload) { return handle_intent(payload); });
}

TorrentCoordinator::~TorrentCoordinator() {
    node_.set_torrent_intent_handler({});
    stop();
}

void TorrentCoordinator::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("torrent-coordinator", stop, [this, stop] { loop(stop); });
    });
}

void TorrentCoordinator::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
}

void TorrentCoordinator::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        pass_now();
        std::mutex wait_mutex;
        std::unique_lock lock(wait_mutex);
        wake_.wait_for(lock, stop, pass_interval, [] { return false; });
    }
}

// The current snapshot, including this node's own latest commit: an add's
// 202 promises the job is listed at once (read-your-writes). snapshot_view()
// refreshes a stale cache; the cached view is the fallback when metadata
// cannot be read right now.
std::optional<MetadataSnapshotView> TorrentCoordinator::current_view() const {
    try {
        return metadata_.snapshot_view();
    } catch (const std::exception&) {
        return metadata_.available_snapshot_view();
    }
}

// The availability flag is computed by the node's metadata refresh; until it
// has been computed once there is no answer, and the write itself is tried
// (its failure is still a 503).
bool TorrentCoordinator::write_available() const {
    const auto status = metadata_.cluster_status();
    return status.observed_unix_ms == 0 || status.write_available;
}

std::optional<std::chrono::milliseconds> TorrentCoordinator::default_remove_after() const {
    return view_.default_remove_after();
}

// ---- API ----------------------------------------------------------------------

TorrentCoordinator::Outcome TorrentCoordinator::add(std::string_view uri, bool search_result, std::optional<NodeId> pin,
                                                   std::optional<std::optional<uint64_t>> remove_after) {
    Outcome out;
    const auto self = node_.node_id();
    if (pin && *pin == NodeId{}) pin.reset();
    if (pin) {
        const bool member = std::any_of(node_.membership().active().begin(), node_.membership().active().end(),
                                        [&](const NodeInfo& n) { return n.id == *pin; }) ||
                            *pin == self;
        if (!member) {
            out.status = 409;
            out.code = "placement_failed";
            out.reason = "node_not_member";
            out.message = "node " + to_string(*pin) + " is not an active member of this cluster";
            return out;
        }
        const auto nodes = view_.torrent_nodes();
        if (std::none_of(nodes.begin(), nodes.end(), [&](const auto& n) { return n.node_id == *pin; })) {
            out.status = 409;
            out.code = "placement_failed";
            out.reason = "node_not_torrent_capable";
            out.message = "node " + to_string(*pin) + " does not run the torrent subsystem";
            return out;
        }
    }

    // The canonical magnet and info hash. Core reads a magnet itself; a
    // .torrent needs a node that runs the plugin.
    std::string magnet, info_hash, name;
    if (auto sanitized = sanitize_magnet_uri(uri); sanitized && !search_result) {
        magnet = *sanitized;
        info_hash = magnet_info_hash(magnet);
        name = magnet_display_name(magnet);
    } else if (auto sanitized_result = sanitize_magnet_uri(uri)) {
        magnet = *sanitized_result;
        info_hash = magnet_info_hash(magnet);
        name = magnet_display_name(magnet);
    } else if (!search_result) {
        out.status = 409;
        out.code = "placement_failed";
        out.reason = "add_failed";
        out.message = "torrent job requires a magnet URI";
        return out;
    } else {
        std::string error;
        if (auto local = registry_.torrent()) {
            try {
                const auto resolved = local->resolve(uri, true);
                magnet = resolved.magnet;
                info_hash = resolved.info_hash;
                name = resolved.name;
            } catch (const std::exception& e) {
                error = e.what();
            }
        } else if (auto remote = resolve_remote(uri, true, error)) {
            magnet = remote->first;
            info_hash = magnet_info_hash(magnet);
            name = remote->second;
        }
        if (magnet.empty()) {
            out.status = 409;
            out.code = "placement_failed";
            out.reason = "add_failed";
            out.message = error.empty() ? "no torrent-capable node could read the torrent" : error;
            return out;
        }
    }
    if (info_hash.empty()) {
        out.status = 409;
        out.code = "placement_failed";
        out.reason = "add_failed";
        out.message = "the magnet names no info hash";
        return out;
    }
    if (!write_available()) {
        out.status = 503;
        out.code = "metadata_unavailable";
        out.message = "cluster metadata is not writable, so no torrent can be added until it is";
        out.cluster_scope = true;
        return out;
    }

    TorrentRequest request;
    request.id = to_string(random_node_id());
    request.info_hash = info_hash;
    request.source = magnet;
    request.name = name;
    request.created_unix_ms = request.progress_unix_ms = unix_ms();
    request.created_by = self;
    request.pinned_node_id = pin;
    if (remove_after)
        request.remove_after_ms = *remove_after;
    else if (auto fallback = default_remove_after())
        request.remove_after_ms = static_cast<uint64_t>(fallback->count());
    request.settings_changed_unix_ms = request.desired_changed_unix_ms = request.created_unix_ms;
    request.settings_changed_by = request.desired_changed_by = self;

    std::optional<TorrentRequest> holder;
    try {
        metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            holder.reset();
            for (const auto& [_, existing] : snapshot.torrent_requests)
                if (!existing.removed_unix_ms && existing.info_hash == info_hash) {
                    holder = existing;
                    throw Unchanged{};
                }
            put(snapshot, delta, request);
        });
    } catch (const Unchanged&) {
    } catch (const std::exception& e) {
        out.status = 503;
        out.code = "metadata_unavailable";
        out.message = std::string("the request could not be recorded: ") + e.what();
        out.cluster_scope = true;
        return out;
    }
    if (holder) {
        out.status = 409;
        out.code = "torrent_already_added";
        out.message = "job " + holder->id + " already holds this torrent";
        out.request = holder;
        if (holder->claim) out.node = holder->claim->node_id;
        return out;
    }
    Log::info("torrent requested id=" + request.id + " info_hash=" + info_hash +
              (pin ? " pinned=" + to_string(*pin) : std::string{}));
    wake_.notify_all();
    out.status = 202;
    out.code = "ok";
    out.request = request;
    return out;
}

std::vector<TorrentRequest> TorrentCoordinator::requests() const {
    std::vector<TorrentRequest> out;
    const auto view = current_view();
    if (!view) return out;
    for (const auto& [_, request] : view->snapshot->torrent_requests)
        if (!request.removed_unix_ms) out.push_back(request);
    return out;
}

std::optional<TorrentRequest> TorrentCoordinator::request(std::string_view id) const {
    const auto view = current_view();
    if (!view) return std::nullopt;
    const auto found = view->snapshot->torrent_requests.find(id);
    if (found == view->snapshot->torrent_requests.end() || found->second.removed_unix_ms) return std::nullopt;
    return found->second;
}

std::optional<ClusterTorrentJob> TorrentCoordinator::live_job(const TorrentRequest& request) const {
    if (!request.claim) return std::nullopt;
    if (request.claim->node_id == node_.node_id()) {
        if (auto local = registry_.torrent())
            if (auto job = local->job(request.id)) return ClusterTorrentJob{node_.node_id(), std::move(*job)};
        return std::nullopt;
    }
    auto found = view_.torrent_job(request.id);
    if (found && found->node_id == request.claim->node_id) return found;
    return std::nullopt;
}

bool TorrentCoordinator::desired_applied(const TorrentRequest& request, const std::optional<TorrentJob>& live) const {
    if (request.desired == TorrentDesired::cancelled)
        return request.phase == TorrentPhase::cancelled || request.removed_unix_ms != 0;
    if (!request.claim) return true; // nothing to apply until a node takes it
    if (torrent_phase_terminal(request.phase)) return true;
    if (!live) return false;
    const bool paused = live->state == TorrentJobState::paused;
    return request.desired == TorrentDesired::paused ? paused : !paused;
}

std::string TorrentCoordinator::desired_blocked_reason(const TorrentRequest& request) const {
    if (torrent_phase_terminal(request.phase) || request.removed_unix_ms) return {};
    const auto nodes = view_.torrent_nodes();
    if (request.claim) {
        if (request.claim->node_id == node_.node_id()) return {};
        const auto owner = std::find_if(nodes.begin(), nodes.end(),
                                        [&](const auto& n) { return n.node_id == request.claim->node_id; });
        if (owner == nodes.end() || !owner->reachable) return "owner_unreachable";
        return {};
    }
    if (request.pinned_node_id) {
        const auto pinned = std::find_if(nodes.begin(), nodes.end(),
                                         [&](const auto& n) { return n.node_id == *request.pinned_node_id; });
        if (pinned == nodes.end() || !pinned->reachable) return "pinned_node_unavailable";
        return {};
    }
    if (request.desired != TorrentDesired::active) return {};
    if (std::none_of(nodes.begin(), nodes.end(),
                     [](const auto& n) { return n.reachable && n.offer.accepting; }))
        return "no_capable_node";
    return {};
}

TorrentCoordinator::Outcome TorrentCoordinator::act(std::string_view id, std::string_view action) {
    Outcome out;
    auto current = request(id);
    if (!current) {
        out.status = 404;
        out.code = "not_found";
        out.message = "torrent job not found";
        return out;
    }
    const auto& r = *current;
    const auto invalid = [&] {
        Outcome refused;
        refused.status = 409;
        refused.code = "invalid_state";
        refused.message = "job cannot perform that action in its current state";
        refused.request = r;
        return refused;
    };

    if (action == "retry") {
        // Resumes a failed ingest on the owner; the owner's next pass writes
        // the phase back under a new epoch.
        if (r.phase != TorrentPhase::failed || !r.claim || r.ingest_job_id.empty()) return invalid();
        bool changed = false;
        if (r.claim->node_id == node_.node_id()) {
            if (auto local = registry_.torrent()) changed = local->retry(id);
        } else {
            const auto result = view_.torrent_action(id, "retry");
            if (result.unreachable) {
                out.status = 503;
                out.code = "node_unreachable";
                out.message = "the node that owns this job cannot be reached from this node";
                return out;
            }
            changed = result.changed;
        }
        if (!changed) return invalid();
        wake_.notify_all();
        out.status = 202;
        out.code = "ok";
        out.request = r;
        return out;
    }

    if (action == "clear") {
        const bool clearable = torrent_phase_terminal(r.phase) || !r.claim;
        if (!clearable) return invalid();
        if (!write_available()) {
            out.status = 503;
            out.code = "metadata_unavailable";
            out.message = "cluster metadata is not writable, so the job cannot be cleared until it is";
            out.cluster_scope = true;
            return out;
        }
        std::optional<TorrentRequest> cleared;
        try {
            metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
                auto found = snapshot.torrent_requests.find(id);
                if (found == snapshot.torrent_requests.end() || found->second.removed_unix_ms) throw Unchanged{};
                auto updated = found->second;
                updated.removed_unix_ms = unix_ms();
                put(snapshot, delta, updated);
                cleared = updated;
            });
        } catch (const Unchanged&) {
        } catch (const std::exception& e) {
            out.status = 503;
            out.code = "metadata_unavailable";
            out.message = std::string("the job could not be cleared: ") + e.what();
            out.cluster_scope = true;
            return out;
        }
        Log::info("torrent cleared id=" + std::string(id));
        wake_.notify_all();
        out.status = 202;
        out.code = "ok";
        return out;
    }

    TorrentDesired desired;
    if (action == "pause") {
        if (torrent_phase_terminal(r.phase) || r.desired == TorrentDesired::cancelled) return invalid();
        desired = TorrentDesired::paused;
    } else if (action == "resume") {
        if (torrent_phase_terminal(r.phase) || r.desired != TorrentDesired::paused) return invalid();
        desired = TorrentDesired::active;
    } else if (action == "cancel") {
        if (r.phase == TorrentPhase::completed || r.phase == TorrentPhase::cancelled) return invalid();
        desired = TorrentDesired::cancelled;
    } else {
        out.status = 404;
        out.code = "not_found";
        out.message = "unknown torrent action";
        return out;
    }
    return write_desired(r, desired);
}

TorrentCoordinator::Outcome TorrentCoordinator::write_desired(const TorrentRequest& r, TorrentDesired desired) {
    Outcome out;
    const auto now = unix_ms();
    if (!write_available()) {
        // The owner applies it now and publishes it later; with no owner there
        // is nothing to apply it to.
        if (!r.claim) {
            out.status = 503;
            out.code = "metadata_unavailable";
            out.message = "cluster metadata is not writable and no node holds this job";
            out.cluster_scope = true;
            return out;
        }
        if (r.claim->node_id == node_.node_id()) return apply_intent_locally(r.id, desired, now);
        const auto peers = node_.membership().active();
        const auto owner = std::find_if(peers.begin(), peers.end(),
                                        [&](const NodeInfo& n) { return n.id == r.claim->node_id; });
        if (owner == peers.end()) {
            out.status = 503;
            out.code = "node_unreachable";
            out.message = "the node that owns this job cannot be reached from this node";
            return out;
        }
        try {
            const auto text = Json(intent_request(r.id, desired, now)).dump();
            auto reply = node_.call(*owner, MessageType::torrent_intent, Bytes(text.begin(), text.end()),
                                    FrameType::control);
            if (reply.message.type != MessageType::torrent_intent_reply) throw std::runtime_error("refused");
            const std::string body(reply.message.payload.begin(), reply.message.payload.end());
            auto parsed = Json::parse(body);
            if (const auto* ok = parsed.find("applied"); !ok || !ok->isBool() || !ok->asBool())
                throw std::runtime_error("not applied");
        } catch (const std::exception&) {
            out.status = 503;
            out.code = "node_unreachable";
            out.message = "the node that owns this job cannot be reached from this node";
            return out;
        }
        auto shown = r;
        shown.desired = desired;
        shown.desired_changed_unix_ms = now;
        out.status = 202;
        out.code = "ok";
        out.request = shown;
        return out;
    }

    std::optional<TorrentRequest> written;
    try {
        metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            auto found = snapshot.torrent_requests.find(r.id);
            if (found == snapshot.torrent_requests.end() || found->second.removed_unix_ms) throw Unchanged{};
            auto updated = found->second;
            updated.desired = desired;
            updated.desired_changed_unix_ms = std::max(now, updated.desired_changed_unix_ms + 1);
            updated.desired_changed_by = node_.node_id();
            // Nothing holds an unclaimed request, so cancelling it is the whole job.
            if (desired == TorrentDesired::cancelled && !updated.claim) {
                updated.phase = TorrentPhase::cancelled;
                updated.progress_unix_ms = updated.desired_changed_unix_ms;
            }
            put(snapshot, delta, updated);
            written = updated;
        });
    } catch (const Unchanged&) {
    } catch (const std::exception& e) {
        out.status = 503;
        out.code = "metadata_unavailable";
        out.message = std::string("the request could not be updated: ") + e.what();
        out.cluster_scope = true;
        return out;
    }
    wake_.notify_all();
    out.status = 202;
    out.code = "ok";
    out.request = written ? written : r;
    return out;
}

TorrentCoordinator::Outcome TorrentCoordinator::patch(std::string_view id,
                                                     std::optional<std::optional<uint64_t>> remove_after,
                                                     std::optional<std::optional<NodeId>> pin) {
    Outcome out;
    auto current = request(id);
    if (!current) {
        out.status = 404;
        out.code = "not_found";
        out.message = "torrent job not found";
        return out;
    }
    if (pin && current->phase != TorrentPhase::awaiting_node) {
        out.status = 409;
        out.code = "invalid_state";
        out.message = "a job can be re-pinned only while it awaits a node";
        out.request = current;
        return out;
    }
    if (pin && *pin && **pin != NodeId{}) {
        const auto nodes = view_.torrent_nodes();
        if (std::none_of(nodes.begin(), nodes.end(), [&](const auto& n) { return n.node_id == **pin; })) {
            out.status = 409;
            out.code = "placement_failed";
            out.reason = "node_not_torrent_capable";
            out.message = "node " + to_string(**pin) + " does not run the torrent subsystem";
            return out;
        }
    }
    if (!write_available()) {
        out.status = 503;
        out.code = "metadata_unavailable";
        out.message = "cluster metadata is not writable";
        out.cluster_scope = true;
        return out;
    }
    std::optional<TorrentRequest> written;
    try {
        metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            auto found = snapshot.torrent_requests.find(id);
            if (found == snapshot.torrent_requests.end() || found->second.removed_unix_ms) throw Unchanged{};
            auto updated = found->second;
            if (remove_after) updated.remove_after_ms = *remove_after;
            if (pin) {
                updated.pinned_node_id = *pin;
                if (updated.pinned_node_id && *updated.pinned_node_id == NodeId{}) updated.pinned_node_id.reset();
            }
            updated.settings_changed_unix_ms = std::max(unix_ms(), updated.settings_changed_unix_ms + 1);
            updated.settings_changed_by = node_.node_id();
            put(snapshot, delta, updated);
            written = updated;
        });
    } catch (const Unchanged&) {
    } catch (const std::exception& e) {
        out.status = 503;
        out.code = "metadata_unavailable";
        out.message = std::string("the request could not be updated: ") + e.what();
        out.cluster_scope = true;
        return out;
    }
    wake_.notify_all();
    out.status = 200;
    out.code = "ok";
    out.request = written ? written : current;
    return out;
}

// ---- intents: actions applied by the owner while metadata is unwritable ------

TorrentCoordinator::Outcome TorrentCoordinator::apply_intent_locally(const std::string& id, TorrentDesired desired,
                                                                    uint64_t changed_unix_ms) {
    Outcome out;
    auto local = registry_.torrent();
    if (!local || !local->job(id)) {
        out.status = 404;
        out.code = "not_found";
        out.message = "this node does not hold the job";
        return out;
    }
    if (desired == TorrentDesired::paused) (void)local->pause(id);
    else if (desired == TorrentDesired::active) (void)local->resume(id);
    else (void)local->cancel(id);
    {
        std::lock_guard lock(mutex_);
        auto& intent = intents_[id];
        if (changed_unix_ms >= intent.changed_unix_ms || desired == TorrentDesired::cancelled) {
            intent.desired = desired;
            intent.changed_unix_ms = changed_unix_ms;
        }
        save_intents_locked();
    }
    Log::info("torrent intent applied while metadata is unwritable id=" + id +
              " desired=" + std::string(torrent_desired_name(desired)));
    auto shown = request(id);
    if (shown) {
        shown->desired = desired;
        shown->desired_changed_unix_ms = changed_unix_ms;
    }
    out.status = 202;
    out.code = "ok";
    out.request = shown;
    return out;
}

Bytes TorrentCoordinator::handle_intent(std::span<const uint8_t> payload) {
    Json::Object reply;
    try {
        const std::string text(payload.begin(), payload.end());
        auto request = Json::parse(text);
        const auto* id = request.find("job_id");
        const auto* desired = request.find("desired");
        const auto* changed = request.find("changed_unix_ms");
        if (!id || !id->isString() || !desired || !desired->isString() || !changed)
            throw std::runtime_error("malformed intent");
        const auto parsed = parse_desired(desired->asString());
        if (!parsed) throw std::runtime_error("unknown desired state");
        const auto outcome = apply_intent_locally(id->asString(), *parsed, changed->asUInt64());
        reply["applied"] = outcome.status == 202;
    } catch (const std::exception& e) {
        reply["applied"] = false;
        reply["error"] = std::string(e.what());
    }
    const auto text = Json(std::move(reply)).dump();
    return Bytes(text.begin(), text.end());
}

TorrentDesired TorrentCoordinator::effective_desired(const TorrentRequest& request) const {
    std::lock_guard lock(mutex_);
    const auto found = intents_.find(request.id);
    if (found == intents_.end()) return request.desired;
    if (request.desired == TorrentDesired::cancelled) return request.desired;
    if (found->second.desired == TorrentDesired::cancelled ||
        found->second.changed_unix_ms > request.desired_changed_unix_ms)
        return found->second.desired;
    return request.desired;
}

void TorrentCoordinator::publish_intents() {
    std::map<std::string, Intent, std::less<>> pending;
    {
        std::lock_guard lock(mutex_);
        pending = intents_;
    }
    if (pending.empty() || !write_available()) return;
    try {
        metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            bool any = false;
            for (const auto& [id, intent] : pending) {
                auto found = snapshot.torrent_requests.find(id);
                if (found == snapshot.torrent_requests.end()) continue;
                auto updated = found->second;
                updated.desired = intent.desired;
                updated.desired_changed_unix_ms = intent.changed_unix_ms;
                updated.desired_changed_by = node_.node_id();
                // The join decides: a later write elsewhere still wins.
                updated = merge_torrent_request(found->second, updated);
                if (updated == found->second) continue;
                put(snapshot, delta, updated);
                any = true;
            }
            if (!any) throw Unchanged{};
        });
    } catch (const Unchanged&) {
    } catch (const std::exception& e) {
        Log::debug("torrent intents not published yet: " + std::string(e.what()));
        return;
    }
    std::lock_guard lock(mutex_);
    for (const auto& [id, intent] : pending) {
        const auto found = intents_.find(id);
        if (found != intents_.end() && found->second.changed_unix_ms == intent.changed_unix_ms &&
            found->second.desired == intent.desired)
            intents_.erase(found);
    }
    save_intents_locked();
    Log::info("torrent intents published: " + std::to_string(pending.size()));
}

void TorrentCoordinator::load_intents() {
    std::ifstream in(intents_path_, std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    try {
        auto parsed = Json::parse(text.str());
        std::lock_guard lock(mutex_);
        for (const auto& [id, value] : parsed.asObject()) {
            const auto* desired = value.find("desired");
            const auto* changed = value.find("changed_unix_ms");
            if (!desired || !desired->isString() || !changed) continue;
            if (auto d = parse_desired(desired->asString())) intents_[id] = Intent{*d, changed->asUInt64()};
        }
    } catch (const std::exception& e) {
        Log::warn("torrent intent journal unreadable, ignored: " + std::string(e.what()));
    }
}

void TorrentCoordinator::save_intents_locked() const {
    Json::Object root;
    for (const auto& [id, intent] : intents_) {
        Json::Object item;
        item["desired"] = std::string(torrent_desired_name(intent.desired));
        item["changed_unix_ms"] = intent.changed_unix_ms;
        root[id] = std::move(item);
    }
    try {
        std::filesystem::create_directories(intents_path_.parent_path());
        durable_replace_file(intents_path_, Json(std::move(root)).dump());
    } catch (const std::exception& e) {
        Log::warn("torrent intent journal not written: " + std::string(e.what()));
    }
}

std::optional<std::pair<std::string, std::string>>
TorrentCoordinator::resolve_remote(std::string_view uri, bool search_result, std::string& error) {
    const auto peers = node_.membership().active();
    for (const auto& node : view_.torrent_nodes()) {
        if (node.local || !node.reachable) continue;
        const auto peer = std::find_if(peers.begin(), peers.end(), [&](const NodeInfo& p) { return p.id == node.node_id; });
        if (peer == peers.end()) continue;
        Json::Object request;
        request["action"] = std::string("resolve");
        request["uri"] = std::string(uri);
        request["search_result"] = search_result;
        try {
            const auto text = Json(std::move(request)).dump();
            auto reply = node_.call(*peer, MessageType::torrent_job_action, Bytes(text.begin(), text.end()),
                                    FrameType::control);
            if (reply.message.type != MessageType::torrent_job_action_reply) continue;
            const std::string body(reply.message.payload.begin(), reply.message.payload.end());
            auto parsed = Json::parse(body);
            const auto* resolved = parsed.find("resolved");
            if (resolved && resolved->isBool() && resolved->asBool()) {
                const auto* magnet = parsed.find("magnet");
                const auto* name = parsed.find("name");
                if (magnet && magnet->isString())
                    return std::pair{magnet->asString(), name && name->isString() ? name->asString() : std::string{}};
            }
            if (const auto* e = parsed.find("error"); e && e->isString()) error = e->asString();
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    return std::nullopt;
}

// ---- the scheduler --------------------------------------------------------------

void TorrentCoordinator::pass_now() {
    std::lock_guard serial(pass_mutex_);
    try {
        pass();
    } catch (const std::exception& e) {
        // One bad pass is the next pass's problem, not the thread's.
        Log::warn("torrent coordinator pass failed: " + std::string(e.what()));
    }
}

void TorrentCoordinator::pass() {
    const auto view = current_view();
    if (!view) return;
    publish_intents();
    const auto& requests = view->snapshot->torrent_requests;
    const auto self = node_.node_id();
    const auto now = unix_ms();
    const auto local = registry_.torrent();

    // Membership absence, for claim leases.
    const auto active = node_.membership().active();
    const auto is_active = [&](const NodeId& id) {
        return id == self || std::any_of(active.begin(), active.end(), [&](const NodeInfo& n) { return n.id == id; });
    };
    {
        std::lock_guard lock(mutex_);
        std::set<NodeId> claimants;
        for (const auto& [_, r] : requests)
            if (r.claim) claimants.insert(r.claim->node_id);
        for (const auto& node : claimants) {
            if (is_active(node)) absent_since_.erase(node);
            else absent_since_.try_emplace(node, now);
        }
    }
    const auto lapsed = [&](const TorrentRequest& r) {
        if (!r.claim) return false;
        std::lock_guard lock(mutex_);
        const auto found = absent_since_.find(r.claim->node_id);
        return found != absent_since_.end() &&
               now - found->second >= static_cast<uint64_t>(claim_lease_.count());
    };

    std::vector<TorrentRequest> updates;

    // Jobs this node held before torrents were cluster-wide become requests
    // it has already claimed, once each.
    if (local) {
        std::set<std::string> held_hashes;
        for (const auto& [_, r] : requests)
            if (!r.removed_unix_ms) held_hashes.insert(r.info_hash);
        for (const auto& job : local->jobs()) {
            if (requests.contains(job.id)) continue;
            {
                std::lock_guard lock(mutex_);
                if (migrated_.contains(job.id)) continue;
            }
            if (job.info_hash.empty() || held_hashes.contains(job.info_hash)) continue;
            TorrentRequest r;
            r.id = job.id;
            r.info_hash = job.info_hash;
            r.source = job.source_uri;
            r.created_unix_ms = job.created_unix_ms ? job.created_unix_ms : now;
            r.created_by = self;
            r.settings_changed_unix_ms = r.desired_changed_unix_ms = now;
            r.settings_changed_by = r.desired_changed_by = self;
            r.desired = job.state == TorrentJobState::paused      ? TorrentDesired::paused
                        : job.state == TorrentJobState::cancelled ? TorrentDesired::cancelled
                                                                  : TorrentDesired::active;
            r.claim = TorrentClaim{self, 1, now};
            r.phase = torrent_phase_of(job.state);
            r.phase_epoch = 1;
            r.progress_unix_ms = now;
            r.name = job.name;
            r.bytes_total = job.bytes_total;
            r.ingest_job_id = job.ingest_job_id.value_or("");
            r.error_code = job.error_code;
            r.error = job.error;
            if (r.phase == TorrentPhase::completed) r.completed_unix_ms = job.updated_unix_ms ? job.updated_unix_ms : now;
            held_hashes.insert(r.info_hash);
            updates.push_back(std::move(r));
            std::lock_guard lock(mutex_);
            migrated_.insert(job.id);
        }
    }

    std::vector<const TorrentRequest*> claimable;
    for (const auto& [id, r] : requests) {
        std::optional<TorrentJob> job = local ? local->job(id) : std::nullopt;
        const bool mine = local && r.claim && r.claim->node_id == self;

        if (r.removed_unix_ms) {
            if (job && local) {
                if (!torrent_phase_terminal(torrent_phase_of(job->state))) (void)local->cancel(id);
                (void)local->clear(id);
            }
            continue;
        }

        if (!mine) {
            // Claimed by another node: anything left here is superseded.
            if (job && local && r.claim && r.claim->node_id != self) {
                Log::info("torrent claim superseded here id=" + id + " owner=" + to_string(r.claim->node_id));
                if (!torrent_phase_terminal(torrent_phase_of(job->state))) (void)local->cancel(id);
                (void)local->clear(id);
            }
            if (local && r.desired == TorrentDesired::active &&
                (r.phase == TorrentPhase::awaiting_node || (!torrent_phase_terminal(r.phase) && lapsed(r))) &&
                (!r.pinned_node_id || *r.pinned_node_id == self))
                claimable.push_back(&r);
            else {
                std::lock_guard lock(mutex_);
                claimable_since_.erase(id);
            }
            continue;
        }

        const auto desired = effective_desired(r);
        if (!job) {
            if (!torrent_phase_terminal(r.phase) && desired != TorrentDesired::cancelled) {
                try {
                    (void)local->adopt(id, r.source, desired == TorrentDesired::paused);
                    job = local->job(id);
                } catch (const std::exception& e) {
                    auto failed = r;
                    failed.phase = TorrentPhase::failed;
                    failed.phase_epoch = r.claim->epoch;
                    failed.progress_unix_ms = now;
                    failed.error_code = "adopt_failed";
                    failed.error = e.what();
                    updates.push_back(failed);
                    continue;
                }
            } else if (desired == TorrentDesired::cancelled && !torrent_phase_terminal(r.phase)) {
                auto cancelled = r;
                cancelled.phase = TorrentPhase::cancelled;
                cancelled.phase_epoch = r.claim->epoch;
                cancelled.progress_unix_ms = now;
                updates.push_back(cancelled);
                continue;
            }
        }
        if (!job) continue;

        const auto state_phase = torrent_phase_of(job->state);
        if (!torrent_phase_terminal(state_phase)) {
            if (desired == TorrentDesired::paused && job->state != TorrentJobState::paused) (void)local->pause(id);
            else if (desired == TorrentDesired::active && job->state == TorrentJobState::paused) (void)local->resume(id);
            else if (desired == TorrentDesired::cancelled) (void)local->cancel(id);
            job = local->job(id);
            if (!job) continue;
        }

        auto updated = r;
        const auto phase = torrent_phase_of(job->state);
        updated.name = job->name.empty() ? r.name : job->name;
        updated.bytes_total = job->bytes_total;
        updated.ingest_job_id = job->ingest_job_id.value_or("");
        updated.error_code = job->error_code;
        updated.error = job->error;
        if (phase != r.phase) {
            // Backwards (a retried or revived import) needs a new epoch: the
            // join never moves a phase back within one.
            if (static_cast<uint8_t>(phase) < static_cast<uint8_t>(r.phase) ||
                (torrent_phase_terminal(r.phase) && !torrent_phase_terminal(phase))) {
                updated.claim = TorrentClaim{self, r.claim->epoch + 1, now};
            }
            updated.phase = phase;
            updated.phase_epoch = updated.claim->epoch;
            if (phase == TorrentPhase::completed && !updated.completed_unix_ms) updated.completed_unix_ms = now;
        }
        if (r.phase == TorrentPhase::completed && r.remove_after_ms &&
            now >= r.completed_unix_ms + *r.remove_after_ms) {
            if (local->clear(id)) {
                updated.removed_unix_ms = now;
                Log::info("torrent removed after completion id=" + id);
            }
        }
        if (updated != r) {
            updated.progress_unix_ms = std::max(now, r.progress_unix_ms + 1);
            updates.push_back(std::move(updated));
        }
    }

    // Claim what this node has room for, preferred node first.
    if (local && !claimable.empty()) {
        auto offer = local->offer();
        const auto nodes = view_.torrent_nodes();
        std::sort(claimable.begin(), claimable.end(), [](const TorrentRequest* a, const TorrentRequest* b) {
            return std::tie(a->created_unix_ms, a->id) < std::tie(b->created_unix_ms, b->id);
        });
        for (const auto* r : claimable) {
            if (!offer.accepting) break;
            std::vector<std::pair<uint64_t, NodeId>> ranked;
            for (const auto& node : nodes)
                if (node.reachable && node.offer.accepting &&
                    (!r->pinned_node_id || *r->pinned_node_id == node.node_id))
                    ranked.emplace_back(rendezvous(r->id, node.node_id), node.node_id);
            std::sort(ranked.rbegin(), ranked.rend());
            size_t rank = 0;
            while (rank < ranked.size() && ranked[rank].second != self) ++rank;
            uint64_t since;
            {
                std::lock_guard lock(mutex_);
                since = claimable_since_.try_emplace(r->id, now).first->second;
            }
            if (now - since < rank * static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              claim_rank_step).count()))
                continue;
            auto claimed = *r;
            const auto epoch = r->claim ? r->claim->epoch + 1 : 1;
            claimed.claim = TorrentClaim{self, epoch, now};
            claimed.phase = TorrentPhase::downloading;
            claimed.phase_epoch = epoch;
            claimed.progress_unix_ms = std::max(now, r->progress_unix_ms + 1);
            claimed.error_code.clear();
            claimed.error.clear();
            updates.push_back(std::move(claimed));
            ++offer.active_jobs;
            if (offer.max_active && offer.active_jobs >= offer.max_active) offer.accepting = false;
            std::lock_guard lock(mutex_);
            claimable_since_.erase(r->id);
        }
    }

    // Erase tombstones past their grace: one node does it, the lowest id.
    std::vector<std::string> expired;
    const bool sweeper = std::none_of(active.begin(), active.end(), [&](const NodeInfo& n) { return n.id < self; });
    if (sweeper) {
        const auto grace = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(tombstone_grace).count());
        for (const auto& [id, r] : requests)
            if (r.removed_unix_ms && now - r.removed_unix_ms > grace) expired.push_back(id);
    }

    if ((updates.empty() && expired.empty()) || !write_available()) return;
    try {
        metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            bool any = false;
            for (const auto& update : updates) {
                auto found = snapshot.torrent_requests.find(update.id);
                if (found == snapshot.torrent_requests.end()) {
                    // A migrated job: new to metadata, unless its torrent arrived meanwhile.
                    const bool taken = std::any_of(snapshot.torrent_requests.begin(), snapshot.torrent_requests.end(),
                                                   [&](const auto& kv) {
                                                       return !kv.second.removed_unix_ms &&
                                                              kv.second.info_hash == update.info_hash;
                                                   });
                    if (taken) continue;
                    put(snapshot, delta, update);
                    any = true;
                    continue;
                }
                // Join onto what is there now: an operator's newer intent,
                // or another node's newer claim, is kept.
                auto merged = merge_torrent_request(found->second, update);
                if (merged == found->second) continue;
                put(snapshot, delta, merged);
                any = true;
            }
            for (const auto& id : expired)
                if (snapshot.torrent_requests.erase(id)) {
                    delta.erase_torrent_requests.push_back(id);
                    any = true;
                }
            if (!any) throw Unchanged{};
        });
    } catch (const Unchanged&) {
        return;
    } catch (const std::exception& e) {
        Log::debug("torrent coordinator writes deferred: " + std::string(e.what()));
        return;
    }

    // Adopt what was just claimed, now that the claim stands.
    if (!local) return;
    const auto after = current_view();
    if (!after) return;
    for (const auto& update : updates) {
        const auto found = after->snapshot->torrent_requests.find(update.id);
        if (found == after->snapshot->torrent_requests.end()) continue;
        const auto& r = found->second;
        if (!r.claim || r.claim->node_id != self || torrent_phase_terminal(r.phase) || r.removed_unix_ms) continue;
        if (local->job(r.id)) continue;
        try {
            (void)local->adopt(r.id, r.source, effective_desired(r) == TorrentDesired::paused);
            Log::info("torrent claimed id=" + r.id + " epoch=" + std::to_string(r.claim->epoch));
        } catch (const std::exception& e) {
            Log::warn("torrent claimed but not started id=" + r.id + ": " + e.what());
        }
    }
}

} // namespace macha
