// SPDX-License-Identifier: GPL-3.0-or-later
// The libtorrent-backed download engine, built only into the libmacha-torrent
// plugin. This file only exists at all in a build where libtorrent was found,
// so it has no "not built" branches: an installation without the plugin has no
// torrent capability at runtime, which core reports as `unavailable` rather
// than compiling in a stub. See
// TODO/2026-09-05-subsystem-plugin-isolation-plan.md.
#include "torrent_manager.hpp"

#include "crypto.hpp"
#include "durable_file.hpp"
#include "json.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#include <sys/stat.h>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/span.hpp>

namespace macha {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool safe_tracker_url(std::string_view value) {
    return value.starts_with("http://") || value.starts_with("https://") || value.starts_with("udp://");
}

std::string sanitize_text(std::string value, size_t limit = 1024) {
    std::erase_if(value, [](unsigned char c) { return c < 0x20 && c != '\t'; });
    if (value.size() > limit) value.resize(limit);
    return trim(std::move(value));
}

Json torrent_job_json(const TorrentJob& job) {
    Json::Object o;
    o["id"] = job.id;
    o["name"] = job.name;
    o["source_uri"] = job.source_uri;
    o["info_hash"] = job.info_hash;
    o["save_path"] = job.save_path.string();
    o["state"] = torrent_job_state_name(job.state);
    o["bytes_total"] = job.bytes_total;
    o["bytes_completed"] = job.bytes_completed;
    o["uploaded_total"] = job.uploaded_total;
    o["catalogue_total"] = static_cast<uint64_t>(job.catalogue_total);
    o["catalogue_pending"] = static_cast<uint64_t>(job.catalogue_pending);
    o["catalogue_catalogued"] = static_cast<uint64_t>(job.catalogue_catalogued);
    o["catalogue_no_match"] = static_cast<uint64_t>(job.catalogue_no_match);
    o["catalogue_failed"] = static_cast<uint64_t>(job.catalogue_failed);
    o["ingest_job_id"] = job.ingest_job_id ? Json(*job.ingest_job_id) : Json(nullptr);
    o["created_unix_ms"] = job.created_unix_ms;
    o["updated_unix_ms"] = job.updated_unix_ms;
    o["error"] = job.error;
    return o;
}

TorrentJob parse_torrent_job(const Json& value) {
    TorrentJob job;
    if (const auto* v = value.find("id")) job.id = v->asString();
    if (const auto* v = value.find("name")) job.name = v->asString();
    if (const auto* v = value.find("source_uri")) job.source_uri = v->asString();
    if (const auto* v = value.find("info_hash")) job.info_hash = v->asString();
    if (const auto* v = value.find("save_path")) job.save_path = v->asString();
    if (const auto* v = value.find("state")) {
        if (auto state = parse_torrent_job_state(v->asString())) job.state = *state;
    }
    if (const auto* v = value.find("bytes_total")) job.bytes_total = v->asUInt64();
    if (const auto* v = value.find("bytes_completed")) job.bytes_completed = v->asUInt64();
    if (const auto* v = value.find("uploaded_total")) job.uploaded_total = v->asUInt64();
    if (const auto* v = value.find("catalogue_total")) job.catalogue_total = static_cast<size_t>(v->asUInt64());
    if (const auto* v = value.find("catalogue_pending")) job.catalogue_pending = static_cast<size_t>(v->asUInt64());
    if (const auto* v = value.find("catalogue_catalogued")) job.catalogue_catalogued = static_cast<size_t>(v->asUInt64());
    if (const auto* v = value.find("catalogue_no_match")) job.catalogue_no_match = static_cast<size_t>(v->asUInt64());
    if (const auto* v = value.find("catalogue_failed")) job.catalogue_failed = static_cast<size_t>(v->asUInt64());
    if (const auto* v = value.find("ingest_job_id"); v && !v->isNull()) job.ingest_job_id = v->asString();
    if (const auto* v = value.find("created_unix_ms")) job.created_unix_ms = v->asUInt64();
    if (const auto* v = value.find("updated_unix_ms")) job.updated_unix_ms = v->asUInt64();
    if (const auto* v = value.find("error")) job.error = v->asString();
    return job;
}

namespace lt = libtorrent;

// Which alert categories the session subscribes to for a given
// torrent.log_level. error/status/port_mapping/dht are always on: the drain
// loop reads listen, bootstrap and port-mapping outcomes from them, and a
// session that reported nothing sat dead for hours once. ALL adds the
// categories libtorrent itself calls logs, which are opt-in for a reason.
lt::alert_category_t alert_mask_for(LogLevel level) {
    // piece_progress carries piece_finished_alert, which is how the disk
    // backend learns an extent's pieces have verified and it can publish it.
    auto mask = lt::alert_category::error | lt::alert_category::status |
                lt::alert_category::port_mapping | lt::alert_category::dht |
                lt::alert_category::piece_progress;
    if (level == LogLevel::all)
        mask |= lt::alert_category::tracker | lt::alert_category::peer |
                lt::alert_category::session_log | lt::alert_category::torrent_log |
                lt::alert_category::dht_log;
    return mask;
}

lt::session_params make_session_params(const TorrentConfig& config, std::string_view advertise,
                                       TorrentDiskHooks disk_hooks) {
    lt::session_params params;
    params.disk_io_constructor = macha_disk_io_constructor(std::move(disk_hooks));
    auto& settings = params.settings;
    settings.set_str(lt::settings_pack::listen_interfaces,
                     torrent_listen_interfaces(config, advertise));
    // Nothing consumed alerts at all before this, so a session that bound
    // nothing usable, failed to bootstrap DHT or was refused by every tracker
    // reported exactly nothing: two torrents sat dead for hours with an empty
    // error field and one "plugin loaded" line in the journal.
    settings.set_int(lt::settings_pack::alert_mask, alert_mask_for(config.log_level));
    settings.set_int(lt::settings_pack::active_downloads, static_cast<int>(config.max_active));
    settings.set_int(lt::settings_pack::active_limit, static_cast<int>(config.max_active + 4));
    settings.set_int(lt::settings_pack::active_seeds, 0);
    settings.set_bool(lt::settings_pack::enable_dht, config.dht);
    settings.set_bool(lt::settings_pack::enable_lsd, config.lsd);
    // Stated rather than inherited from libtorrent's defaults. See
    // TorrentConfig::upnp for why these are separate from network.upnp.
    settings.set_bool(lt::settings_pack::enable_upnp, config.upnp);
    settings.set_bool(lt::settings_pack::enable_natpmp, config.natpmp);
    if (config.max_download_rate)
        settings.set_int(lt::settings_pack::download_rate_limit,
                         static_cast<int>(std::min<uint64_t>(config.max_download_rate, INT_MAX)));
    if (config.max_upload_rate)
        settings.set_int(lt::settings_pack::upload_rate_limit,
                         static_cast<int>(std::min<uint64_t>(config.max_upload_rate, INT_MAX)));
    settings.set_str(lt::settings_pack::user_agent, "Macha/" + std::string(kServerVersion) +
                     " libtorrent/" + std::string(lt::version()));
    return params;
}

void harden_add_params(lt::add_torrent_params& atp, const TorrentConfig& config) {
    // A torrent may contain arbitrary HTTP web seeds and DHT bootstrap nodes.
    // Macha acquisition deliberately accepts peers/trackers only; never turn a
    // torrent metainfo file into a general-purpose server-side HTTP fetcher.
    atp.url_seeds.clear();
    atp.dht_nodes.clear();
    std::erase_if(atp.trackers, [](const std::string& tracker) { return !safe_tracker_url(tracker); });
    atp.tracker_tiers.assign(atp.trackers.size(), 0);
    if (!config.dht) atp.flags |= lt::torrent_flags::disable_dht;
    if (!config.lsd) atp.flags |= lt::torrent_flags::disable_lsd;
    if (!config.pex) atp.flags |= lt::torrent_flags::disable_pex;
    // In order, so extents complete in order and are published while their
    // bytes are still in page cache, and so a file is watchable soonest.
    atp.flags |= lt::torrent_flags::sequential_download;
}

} // namespace

struct TorrentManager::Impl {
    libtorrent::session session;
    std::map<std::string, libtorrent::torrent_handle, std::less<>> handles;
    CurlHttpClient http;

    Impl(const TorrentConfig& config, std::string_view advertise, TorrentDiskHooks disk_hooks)
        : session(make_session_params(config, advertise, std::move(disk_hooks))) {}
};

TorrentManager::TorrentManager(NodeRuntime& node, IngestManager& ingest, TorrentConfig config,
                               const std::filesystem::path& state_path)
    : node_(node), ingest_(ingest), config_(std::move(config)),
      state_file_(state_path / "torrent" / "jobs.json") {
    node_.set_torrent_bridge(
        [this](std::span<const uint8_t> payload) { return handle_jobs_query(payload); },
        [this](std::span<const uint8_t> payload) { return handle_job_action(payload); });
    if (!config_.enabled) return;
    std::filesystem::create_directories(state_file_.parent_path());
    impl_ = std::make_unique<Impl>(config_, node_.config().advertise_host, disk_hooks());
    // Alerts arrive on libtorrent's own thread; this only wakes the worker,
    // which does the draining. Without it a settled manager blocks on the
    // condition and never reads the queue.
    impl_->session.set_alert_notify([this] {
        alerts_pending_.store(true, std::memory_order_release);
        cv_.notify_all();
    });
    load_state();
}

TorrentManager::~TorrentManager() {
    // The bridge holds lambdas bound to `this`. As a supervised subsystem
    // this object is destroyed and reconstructed on fault, not just at
    // process exit, so leaving them installed would dispatch a peer's survey
    // into freed memory. Clear before stopping so no new call is admitted
    // while the worker is winding down.
    node_.set_torrent_bridge({}, {});
    stop();
}

namespace {
// Wire shape for the cluster RPC survey: the persistence shape
// (torrent_job_json/parse_torrent_job) plus the transient fields it
// deliberately never persists (download_rate, upload_rate, peers, seeds,
// eta_seconds -- resetting those across a local restart is intentional; a
// remote peer answering a live survey should still report its own current
// values).
Json torrent_job_wire_json(const TorrentJob& job) {
    auto out = torrent_job_json(job);
    out["download_rate"] = job.download_rate;
    out["upload_rate"] = job.upload_rate;
    out["peers"] = static_cast<uint64_t>(job.peers);
    out["seeds"] = static_cast<uint64_t>(job.seeds);
    out["eta_seconds"] = optional_u64(job.eta_seconds);
    return out;
}

TorrentJob parse_torrent_job_wire(const Json& value) {
    auto job = parse_torrent_job(value);
    if (const auto* v = value.find("download_rate")) job.download_rate = v->asUInt64();
    if (const auto* v = value.find("upload_rate")) job.upload_rate = v->asUInt64();
    if (const auto* v = value.find("peers")) job.peers = static_cast<unsigned>(v->asUInt64());
    if (const auto* v = value.find("seeds")) job.seeds = static_cast<unsigned>(v->asUInt64());
    if (const auto* v = value.find("eta_seconds"); v && !v->isNull()) job.eta_seconds = v->asUInt64();
    return job;
}
} // namespace

Bytes TorrentManager::handle_jobs_query(std::span<const uint8_t> request_payload) const {
    std::string job_id;
    if (!request_payload.empty()) {
        try {
            const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                                   request_payload.size());
            auto request = Json::parse(text);
            if (const auto* id = request.find("job_id"); id && id->isString())
                job_id = id->asString();
        } catch (const std::exception&) {
            // Malformed survey request: answer as "list all" rather than fail
            // the whole peer.
        }
    }
    Json::Array out_jobs;
    if (job_id.empty()) {
        for (const auto& job : jobs()) out_jobs.push_back(torrent_job_wire_json(job));
    } else if (auto found = job(job_id)) {
        out_jobs.push_back(torrent_job_wire_json(*found));
    }
    Json::Object out;
    out["jobs"] = std::move(out_jobs);
    const auto text = Json(std::move(out)).dump();
    return Bytes(text.begin(), text.end());
}

Bytes TorrentManager::handle_job_action(std::span<const uint8_t> request_payload) {
    std::string job_id, action;
    try {
        const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                               request_payload.size());
        auto request = Json::parse(text);
        if (const auto* id = request.find("job_id"); id && id->isString()) job_id = id->asString();
        if (const auto* act = request.find("action"); act && act->isString())
            action = act->asString();
    } catch (const std::exception&) {
    }
    // A targeted add arrives as an action with no job id: the sender chose this
    // node, so this is where the job is created.
    if (action == "add") {
        std::string uri;
        try {
            const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                                   request_payload.size());
            auto request = Json::parse(text);
            if (const auto* u = request.find("uri"); u && u->isString()) uri = u->asString();
        } catch (const std::exception&) {
        }
        Json::Object added;
        if (uri.empty()) {
            added["exists"] = false;
            added["error"] = std::string("a magnet or torrent uri is required");
        } else {
            try {
                added["job_id"] = add(uri);
                added["exists"] = true;
            } catch (const std::exception& error) {
                added["exists"] = false;
                added["error"] = std::string(error.what());
            }
        }
        const auto text = Json(std::move(added)).dump();
        return Bytes(text.begin(), text.end());
    }

    Json::Object out;
    const bool exists = job(job_id).has_value();
    out["exists"] = exists;
    bool changed = false;
    if (exists) {
        if (action == "pause") changed = pause(job_id);
        else if (action == "resume") changed = resume(job_id);
        else if (action == "retry") changed = retry(job_id);
        else if (action == "cancel") changed = cancel(job_id);
        else if (action == "clear") changed = clear(job_id);
    }
    out["changed"] = changed;
    if (auto updated = job(job_id))
        out["job"] = torrent_job_wire_json(*updated);
    else
        out["job"] = Json(nullptr);
    const auto text = Json(std::move(out)).dump();
    return Bytes(text.begin(), text.end());
}

TorrentService::Placement TorrentManager::add_on(const NodeId& node,
                                                std::string_view magnet_or_uri) {
    Placement placement;
    placement.node_id = node;
    if (node == NodeId{} || node == node_.node_id()) {
        placement.node_id = node_.node_id();
        try {
            placement.job_id = add(std::string(magnet_or_uri));
            placement.placed = true;
        } catch (const std::exception& error) {
            placement.error = error.what();
        }
        return placement;
    }

    // A named node that is not in the active set is an error, not a reason to
    // download it here: silently placing the job somewhere the operator did not
    // ask for is exactly what this call exists to stop.
    const auto peers = node_.membership().active();
    const auto peer = std::find_if(peers.begin(), peers.end(),
                                   [&](const NodeInfo& info) { return info.id == node; });
    if (peer == peers.end()) {
        placement.error = "node " + to_string(node) + " is not an active member of this cluster";
        return placement;
    }

    Json::Object request;
    request["action"] = std::string("add");
    request["uri"] = std::string(magnet_or_uri);
    const auto request_text = Json(std::move(request)).dump();
    const Bytes request_bytes(request_text.begin(), request_text.end());
    try {
        auto reply = node_.call(*peer, MessageType::torrent_job_action, request_bytes,
                                FrameType::control);
        if (reply.message.type != MessageType::torrent_job_action_reply) {
            placement.error = "node " + to_string(node) + " refused the request";
            return placement;
        }
        const std::string text(reinterpret_cast<const char*>(reply.message.payload.data()),
                               reply.message.payload.size());
        auto parsed = Json::parse(text);
        if (const auto* placed = parsed.find("exists"); placed && placed->isBool() &&
                                                        placed->asBool()) {
            if (const auto* id = parsed.find("job_id"); id && id->isString()) {
                placement.job_id = id->asString();
                placement.placed = true;
                return placement;
            }
        }
        if (const auto* error = parsed.find("error"); error && error->isString())
            placement.error = error->asString();
        else
            placement.error = "node " + to_string(node) + " did not start the job";
    } catch (const std::exception& error) {
        placement.error = std::string("node ") + to_string(node) + " is unreachable: " +
                          error.what();
    }
    return placement;
}

std::vector<ClusterTorrentJob> TorrentManager::jobs_cluster_wide() const {
    std::vector<ClusterTorrentJob> out;
    for (auto& job : jobs()) out.push_back({node_.node_id(), std::move(job)});
    for (const auto& peer : node_.membership().active()) {
        if (peer.id == node_.node_id()) continue;
        try {
            auto reply = node_.call(peer, MessageType::get_torrent_jobs, {}, FrameType::control);
            if (reply.message.type != MessageType::torrent_jobs_reply) continue;
            const std::string text(reinterpret_cast<const char*>(reply.message.payload.data()),
                                   reply.message.payload.size());
            auto parsed = Json::parse(text);
            const auto* peer_jobs = parsed.find("jobs");
            if (!peer_jobs) continue;
            for (const auto& value : peer_jobs->asArray())
                out.push_back({peer.id, parse_torrent_job_wire(value)});
        } catch (const std::exception& error) {
            Log::debug("torrent job survey " + peer.host + ": " + error.what());
        }
    }
    return out;
}

std::optional<ClusterTorrentJob> TorrentManager::job_cluster_wide(std::string_view id) const {
    if (auto local = job(id))
        return ClusterTorrentJob{node_.node_id(), std::move(*local)};
    Json::Object request;
    request["job_id"] = std::string(id);
    const auto request_text = Json(std::move(request)).dump();
    const Bytes request_bytes(request_text.begin(), request_text.end());
    for (const auto& peer : node_.membership().active()) {
        if (peer.id == node_.node_id()) continue;
        try {
            auto reply = node_.call(peer, MessageType::get_torrent_jobs, request_bytes,
                                    FrameType::control);
            if (reply.message.type != MessageType::torrent_jobs_reply) continue;
            const std::string text(reinterpret_cast<const char*>(reply.message.payload.data()),
                                   reply.message.payload.size());
            auto parsed = Json::parse(text);
            const auto* peer_jobs = parsed.find("jobs");
            if (!peer_jobs || peer_jobs->asArray().empty()) continue;
            return ClusterTorrentJob{peer.id, parse_torrent_job_wire(peer_jobs->asArray().front())};
        } catch (const std::exception& error) {
            Log::debug("torrent job survey " + peer.host + ": " + error.what());
        }
    }
    return std::nullopt;
}

TorrentActionResult TorrentManager::dispatch_action_cluster_wide(std::string_view id,
                                                                 std::string_view action) {
    TorrentActionResult result;
    if (auto local = job(id)) {
        result.exists = true;
        if (action == "pause") result.changed = pause(id);
        else if (action == "resume") result.changed = resume(id);
        else if (action == "retry") result.changed = retry(id);
        else if (action == "cancel") result.changed = cancel(id);
        else if (action == "clear") result.changed = clear(id);
        if (auto updated = job(id))
            result.updated = ClusterTorrentJob{node_.node_id(), std::move(*updated)};
        return result;
    }
    Json::Object request;
    request["job_id"] = std::string(id);
    request["action"] = std::string(action);
    const auto request_text = Json(std::move(request)).dump();
    const Bytes request_bytes(request_text.begin(), request_text.end());
    for (const auto& peer : node_.membership().active()) {
        if (peer.id == node_.node_id()) continue;
        try {
            auto reply = node_.call(peer, MessageType::torrent_job_action, request_bytes,
                                    FrameType::control);
            if (reply.message.type != MessageType::torrent_job_action_reply) continue;
            const std::string text(reinterpret_cast<const char*>(reply.message.payload.data()),
                                   reply.message.payload.size());
            auto parsed = Json::parse(text);
            const auto* exists = parsed.find("exists");
            if (!exists || !exists->isBool() || !exists->asBool()) continue;
            result.exists = true;
            if (const auto* changed = parsed.find("changed"); changed && changed->isBool())
                result.changed = changed->asBool();
            if (const auto* updated = parsed.find("job"); updated && !updated->isNull())
                result.updated = ClusterTorrentJob{peer.id, parse_torrent_job_wire(*updated)};
            return result;
        } catch (const std::exception& error) {
            Log::debug("torrent job action survey " + peer.host + ": " + error.what());
        }
    }
    return result;
}

TorrentActionResult TorrentManager::pause_cluster_wide(std::string_view id) {
    return dispatch_action_cluster_wide(id, "pause");
}

TorrentActionResult TorrentManager::resume_cluster_wide(std::string_view id) {
    return dispatch_action_cluster_wide(id, "resume");
}

TorrentActionResult TorrentManager::retry_cluster_wide(std::string_view id) {
    return dispatch_action_cluster_wide(id, "retry");
}

TorrentActionResult TorrentManager::cancel_cluster_wide(std::string_view id) {
    return dispatch_action_cluster_wide(id, "cancel");
}

TorrentActionResult TorrentManager::clear_cluster_wide(std::string_view id) {
    return dispatch_action_cluster_wide(id, "clear");
}

void TorrentManager::load_state() {
    std::lock_guard lock(mutex_);
    std::ifstream in(state_file_, std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    try {
        const auto root = Json::parse(text.str());
        const auto* jobs = root.find("jobs");
        if (!jobs) return;
        for (const auto& value : jobs->asArray()) {
            auto job = parse_torrent_job(value);
            if (job.id.empty()) continue;
            if (job.state == TorrentJobState::metadata || job.state == TorrentJobState::downloading ||
                job.state == TorrentJobState::verifying || job.state == TorrentJobState::downloaded)
                job.state = TorrentJobState::queued;
            jobs_[job.id] = std::move(job);
        }
    } catch (const std::exception& e) {
        Log::warn("torrent state ignored: " + std::string(e.what()));
    }
}

void TorrentManager::save_state_locked() const {
    if (!config_.enabled) return;
    Json::Array jobs;
    for (const auto& [_, job] : jobs_) jobs.push_back(torrent_job_json(job));
    Json::Object root;
    root["version"] = static_cast<uint64_t>(1);
    root["jobs"] = std::move(jobs);
    durable_replace_file(state_file_, Json(std::move(root)).dump());
}

void TorrentManager::start() {
    if (!config_.enabled || worker_.joinable()) return;
    restore_jobs();
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised("torrent", [this, stop] { loop(stop); });
    });
}

void TorrentManager::request_stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        cv_.notify_all();
    }
}

void TorrentManager::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
}

void TorrentManager::reconfigure(TorrentConfig config) {
    std::lock_guard lock(mutex_);
    if (config.enabled != config_.enabled) Log::warn("torrent.enabled changes require restart");
    config_.max_active = config.max_active;
    config_.max_download_rate = config.max_download_rate;
    config_.max_upload_rate = config.max_upload_rate;
    if (config.log_level != config_.log_level) {
        config_.log_level = config.log_level;
        alert_log_level_.store(config.log_level, std::memory_order_relaxed);
        if (impl_) {
            lt::settings_pack settings;
            settings.set_int(lt::settings_pack::alert_mask, alert_mask_for(config.log_level));
            impl_->session.apply_settings(std::move(settings));
        }
    }
    cv_.notify_all();
}

void TorrentManager::restore_jobs() {
    std::vector<TorrentJob> restore;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, job] : jobs_) {
            if (job.state == TorrentJobState::completed || job.state == TorrentJobState::cancelled ||
                job.state == TorrentJobState::failed || job.state == TorrentJobState::importing)
                continue;
            restore.push_back(job);
        }
    }
    for (const auto& job : restore) {
        try {
            auto sanitized = sanitize_magnet_uri(job.source_uri);
            if (!sanitized) throw std::runtime_error("invalid persisted torrent magnet");
            auto atp = lt::parse_magnet_uri(*sanitized);
            atp.save_path = job.save_path.string();
            harden_add_params(atp, config_);
            auto handle = impl_->session.add_torrent(std::move(atp));
            if (job.state == TorrentJobState::paused) handle.pause();
            impl_->handles[job.id] = std::move(handle);
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            auto& mutable_job = jobs_[job.id];
            mutable_job.state = TorrentJobState::failed;
            mutable_job.error = e.what();
            mutable_job.updated_unix_ms = unix_ms();
            save_state_locked();
        }
    }
}

std::string TorrentManager::add_impl(std::string uri, bool allow_fetch) {
    if (!config_.enabled) throw std::runtime_error("torrent support is disabled");
    TorrentJob job;
    job.id = to_string(random_node_id());
    job.created_unix_ms = job.updated_unix_ms = unix_ms();
    job.save_path = ingest_.staging().path() / "torrents" / job.id;
    std::filesystem::create_directories(job.save_path);

    lt::add_torrent_params atp;
    if (auto magnet = sanitize_magnet_uri(uri)) {
        job.source_uri = *magnet;
        atp = lt::parse_magnet_uri(*magnet);
    } else if (allow_fetch && safe_torrent_fetch_url(uri)) {
        auto fetched = impl_->http.get(uri, {}, 4 * 1024 * 1024);
        if (fetched.status < 200 || fetched.status >= 300)
            throw std::runtime_error("torrent URL returned HTTP " + std::to_string(fetched.status));
        std::vector<char> buffer(fetched.body.begin(), fetched.body.end());
        try {
            // Use the throwing overload shared by libtorrent 2.0 and 2.1.
            // Some 2.1 distro builds no longer expose the deprecated
            // error_code/limits overload used by older builds.
            atp = lt::load_torrent_buffer(lt::span<char const>(buffer.data(), buffer.size()));
        } catch (const std::exception& e) {
            throw std::runtime_error("invalid .torrent file: " + std::string(e.what()));
        }
        // Do not persist a potentially credential-bearing ephemeral download URL
        // as the only restart source. Persist a canonical magnet constructed from
        // the parsed metainfo instead.
        job.source_uri = lt::make_magnet_uri(atp);
        if (auto sanitized = sanitize_magnet_uri(job.source_uri)) job.source_uri = *sanitized;
    } else {
        throw std::runtime_error(allow_fetch
                                     ? "torrent acquisition must be a magnet or trusted http(s) .torrent URL"
                                     : "torrent job requires a magnet URI");
    }
    atp.save_path = job.save_path.string();
    harden_add_params(atp, config_);
    auto handle = impl_->session.add_torrent(std::move(atp));
    auto status = handle.status(lt::torrent_handle::query_name);
    job.name = sanitize_text(status.name, 1024);
    job.state = status.state == lt::torrent_status::downloading_metadata ? TorrentJobState::metadata : TorrentJobState::queued;
    {
        std::lock_guard lock(mutex_);
        jobs_[job.id] = job;
        impl_->handles[job.id] = std::move(handle);
        try {
            save_state_locked();
        } catch (...) {
            // The API must not report a failed admission while libtorrent keeps
            // unacknowledged work running. Roll the live handle and in-memory
            // record back before propagating the persistence failure.
            if (auto h = impl_->handles.find(job.id); h != impl_->handles.end()) {
                try {
                    impl_->session.remove_torrent(
                        h->second, lt::session::delete_files | lt::session::delete_partfile);
                } catch (...) {
                    // Preserve the original durable-state error. remove_torrent
                    // is best-effort cleanup here; erase the manager handle so
                    // this failed request cannot later be driven by our worker.
                }
                impl_->handles.erase(h);
            }
            jobs_.erase(job.id);
            std::error_code ec;
            std::filesystem::remove_all(job.save_path, ec);
            throw;
        }
    }
    cv_.notify_all();
    return job.id;
}

std::string TorrentManager::add(std::string magnet_uri) {
    return add_impl(std::move(magnet_uri), false);
}

std::string TorrentManager::add_search_result(std::string acquisition_uri) {
    return add_impl(std::move(acquisition_uri), true);
}

std::vector<TorrentJob> TorrentManager::jobs() const {
    std::lock_guard lock(mutex_);
    std::vector<TorrentJob> out;
    for (const auto& [_, job] : jobs_) out.push_back(job);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.created_unix_ms > b.created_unix_ms; });
    return out;
}

std::optional<TorrentJob> TorrentManager::job(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return {};
    return it->second;
}

bool TorrentManager::pause(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == TorrentJobState::completed || it->second.state == TorrentJobState::cancelled ||
        it->second.state == TorrentJobState::failed) return false;
    if (it->second.ingest_job_id && !ingest_.pause(*it->second.ingest_job_id)) return false;
    if (!it->second.ingest_job_id)
        if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) h->second.pause();
    it->second.state = TorrentJobState::paused;
    it->second.download_rate = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool TorrentManager::resume(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state != TorrentJobState::paused && it->second.state != TorrentJobState::blocked) return false;
    if (it->second.ingest_job_id) {
        if (!ingest_.resume(*it->second.ingest_job_id)) return false;
    }
    else if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) h->second.resume();
    it->second.state = it->second.ingest_job_id ? TorrentJobState::importing : TorrentJobState::queued;
    it->second.error.clear();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool TorrentManager::retry(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    auto& job = it->second;
    if (job.state != TorrentJobState::failed || !job.ingest_job_id) return false;

    const auto linked = ingest_.job(*job.ingest_job_id);
    if (!linked || linked->state != IngestJobState::failed) return false;

    const auto previous = job;
    job.state = TorrentJobState::importing;
    job.download_rate = 0;
    job.upload_rate = 0;
    job.eta_seconds.reset();
    job.error.clear();
    job.updated_unix_ms = unix_ms();
    try {
        // Persist the wrapper first. If the daemon exits before ingest.resume(),
        // update_jobs() will observe the still-failed linked ingest after restart
        // and converge this wrapper back to failed without touching the payload.
        save_state_locked();
    } catch (...) {
        job = previous;
        throw;
    }

    try {
        if (!ingest_.resume(*job.ingest_job_id)) {
            job = previous;
            save_state_locked();
            return false;
        }
    } catch (...) {
        job = previous;
        save_state_locked();
        throw;
    }

    cv_.notify_all();
    return true;
}

bool TorrentManager::cancel(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == TorrentJobState::completed || it->second.state == TorrentJobState::cancelled) return false;
    if (it->second.ingest_job_id) (void)ingest_.cancel(*it->second.ingest_job_id);
    const bool delete_payload = ingest_.delete_owned_source_on_cancel();
    if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) {
        if (delete_payload)
            impl_->session.remove_torrent(h->second, lt::session::delete_files | lt::session::delete_partfile);
        else
            impl_->session.remove_torrent(h->second);
        impl_->handles.erase(h);
    }
    ingest_.staging().release(it->first);
    if (delete_payload) {
        std::error_code ec;
        std::filesystem::remove_all(it->second.save_path, ec);
        if (ec) Log::warn("torrent cancel staging cleanup failed id=" + it->first + ": " + ec.message());
    }
    it->second.state = TorrentJobState::cancelled;
    it->second.download_rate = it->second.upload_rate = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool TorrentManager::clear(std::string_view id) {
    TorrentJob terminal_job;
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(std::string(id));
        if (it == jobs_.end()) return false;
        if (it->second.state != TorrentJobState::completed &&
            it->second.state != TorrentJobState::cancelled &&
            it->second.state != TorrentJobState::failed)
            return false;
        terminal_job = it->second;
    }

    bool linked_cleared = false;
    if (terminal_job.ingest_job_id) {
        if (auto linked = ingest_.job(*terminal_job.ingest_job_id)) {
            if (!ingest_.clear(*terminal_job.ingest_job_id)) return false;
            linked_cleared = true;
        }
    }
    if (!linked_cleared && ingest_.delete_owned_source_on_clear()) {
        std::error_code ec;
        std::filesystem::remove_all(terminal_job.save_path, ec);
        if (ec) throw std::runtime_error("cannot clear torrent staging payload: " + ec.message());
    }
    ingest_.staging().release(terminal_job.id);

    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state != terminal_job.state) return false;
    jobs_.erase(it);
    save_state_locked();
    Log::info("torrent cleared id=" + terminal_job.id);
    cv_.notify_all();
    return true;
}

bool TorrentManager::has_active_jobs_locked() const {
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& pair) {
        const auto state = pair.second.state;
        return state != TorrentJobState::completed && state != TorrentJobState::cancelled &&
               state != TorrentJobState::failed && state != TorrentJobState::paused;
    });
}

void TorrentManager::drain_alerts() {
    if (!impl_) return;
    alerts_pending_.store(false, std::memory_order_release);
    std::vector<lt::alert*> alerts;
    impl_->session.pop_alerts(&alerts);
    for (const auto* alert : alerts) {
        if (const auto* failed = lt::alert_cast<lt::listen_failed_alert>(alert)) {
            Log::warn("torrent listen failed interface=" + std::string(failed->listen_interface()) +
                      " " + failed->message());
            continue;
        }
        if (const auto* ok = lt::alert_cast<lt::listen_succeeded_alert>(alert)) {
            const auto address = ok->address;
            if (!address.is_loopback() && !address.is_unspecified())
                ++routable_listen_endpoints_;
            Log::info("torrent listening on " + address.to_string() + ":" +
                      std::to_string(ok->port));
            continue;
        }
        if (lt::alert_cast<lt::dht_bootstrap_alert>(alert)) {
            Log::info("torrent DHT bootstrapped");
            continue;
        }
        if (const auto* finished = lt::alert_cast<lt::piece_finished_alert>(alert)) {
            if (const auto path = save_path_of(finished->handle))
                verifications_->piece_verified(*path, static_cast<int>(finished->piece_index));
            continue;
        }
        // A check (a resume, or a recheck) reports no per-piece alerts, and
        // piece alerts can be dropped (below), so at these two points every
        // piece the torrent holds is reported from its own bitfield.
        if (const auto* checked = lt::alert_cast<lt::torrent_checked_alert>(alert)) {
            report_held_pieces(checked->handle);
            continue;
        }
        if (const auto* finished = lt::alert_cast<lt::torrent_finished_alert>(alert)) {
            report_held_pieces(finished->handle);
            continue;
        }
        // libtorrent's alert queue is bounded and drops on overflow. On
        // 2026-09-24 Trainspotting's publication stopped at 162 of 436
        // extents with the publisher idle and nothing logged; a lost
        // piece_finished_alert is the likely cause, and until this line a
        // drop was invisible below debug.
        if (const auto* dropped = lt::alert_cast<lt::alerts_dropped_alert>(alert)) {
            Log::warn("torrent alert queue overflowed: " + std::to_string(dropped->dropped_alerts.count()) +
                      " alert types lost; verified pieces are also taken from each torrent's own "
                      "bitfield, so extent publication does not depend on them");
            continue;
        }
        // Having no inbound port is an operational fact, not churn: the node
        // can still reach peers it dials, but nothing can dial it, so peer
        // counts stay low and it can never seed. At debug that is invisible in
        // normal running -- gbni-1 ran for hours with "no router found" and
        // nothing above debug said so. Once per session, like the bind
        // warnings above, so a router that simply has no UPnP does not become
        // a recurring complaint.
        if (const auto* mapped = lt::alert_cast<lt::portmap_alert>(alert)) {
            if (!logged_portmap_) {
                logged_portmap_ = true;
                Log::info("torrent port mapped on the router: " + mapped->message());
            }
            continue;
        }
        if (const auto* map_failed = lt::alert_cast<lt::portmap_error_alert>(alert)) {
            if (!warned_portmap_failed_) {
                warned_portmap_failed_ = true;
                Log::warn("torrent port mapping failed (" + map_failed->message() +
                          "): this node has no inbound port, so it can dial peers but "
                          "none can dial it -- expect low peer counts and no seeding. "
                          "Forward " + std::to_string(config_.listen_port) +
                          " TCP+UDP by hand, or set torrent.upnp/torrent.natpmp false "
                          "to stop trying.");
            }
            continue;
        }
        // Everything else is libtorrent's own chatter -- tracker churn, DHT
        // traffic, peer errors -- and goes through torrent.log_level, not the
        // process level: like the ffmpeg bridge it applies its own threshold
        // and then emits past the process filter, so a node can run at INFO
        // and still turn this on, and a node at DEBUG no longer has its
        // journal eaten by it (gbni-1, 2026-09-23: 99,088 of 99,187 lines).
        if (alert_log_level_.load(std::memory_order_relaxed) <= LogLevel::debug)
            Log::emit(LogLevel::debug,
                      std::string("torrent alert ") + alert->what() + ": " + alert->message());
    }

    // A session holding only loopback sockets can reach no peer at all. That
    // is a configuration fault, not a transient, so say it once and plainly
    // rather than leaving every magnet stuck in `metadata` with no error.
    if (!warned_loopback_only_ && !routable_listen_endpoints_ && !alerts.empty()) {
        bool any_listen = std::any_of(alerts.begin(), alerts.end(), [](const lt::alert* a) {
            return lt::alert_cast<lt::listen_succeeded_alert>(a) ||
                   lt::alert_cast<lt::listen_failed_alert>(a);
        });
        if (any_listen) {
            warned_loopback_only_ = true;
            Log::warn("torrent session bound no routable interface (loopback only): no peer or "
                      "DHT traffic is possible. Set torrent.listen_interfaces, or check that "
                      "network.advertise names a live link on this node.");
        }
    }
}

void TorrentManager::report_held_pieces(const lt::torrent_handle& handle) {
    const auto path = save_path_of(handle);
    if (!path) return;
    const auto held = handle.status(lt::torrent_handle::query_pieces).pieces;
    for (int piece = 0; piece < held.size(); ++piece)
        if (held.get_bit(lt::piece_index_t(piece))) verifications_->piece_verified(*path, piece);
}

std::optional<std::string> TorrentManager::save_path_of(const lt::torrent_handle& handle) const {
    if (!impl_) return std::nullopt;
    for (const auto& [id, candidate] : impl_->handles) {
        if (candidate != handle) continue;
        std::lock_guard lock(mutex_);
        const auto job = jobs_.find(id);
        if (job == jobs_.end()) return std::nullopt;
        return job->second.save_path.string();
    }
    return std::nullopt;
}

TorrentDiskHooks TorrentManager::disk_hooks() const {
    TorrentDiskHooks hooks;
    hooks.threads = config_.disk_threads;
    NodeRuntime* node = &node_;
    hooks.admit = loader_admission(node_.data_resources());
    // Stage 2: every verified extent is published into the store the ingest
    // commits into, at loader class, durably, and recorded in the job's extent
    // journal; the ingest then commits the file by naming its extents.
    hooks.extent_size = node_.config().extent_size;
    hooks.verifications = verifications_;
    FileSystem* fs = &ingest_.filesystem();
    hooks.publish = [fs](std::span<const uint8_t> bytes) -> std::optional<ObjectId> {
        try {
            return fs->store().put(bytes, FrameType::loader);
        } catch (const std::exception& e) {
            Log::debug(std::string("torrent extent put failed: ") + e.what());
            return std::nullopt;
        }
    };
    // The DATA device's monitor hears the torrent's I/O only when staging
    // lives on a DATA backend's device. On every node today it does
    // (/mnt/diskB/ingest beside /mnt/diskB), and that is why the monitor's
    // verdict on 2026-09-23 described the torrent's load and blamed macha's.
    struct stat staging_stat {};
    bool shared = false;
    if (::stat(ingest_.staging().path().c_str(), &staging_stat) == 0) {
        for (const auto& backend : node_.config().storage_backends) {
            struct stat backend_stat {};
            if (::stat(backend.path.c_str(), &backend_stat) == 0 &&
                backend_stat.st_dev == staging_stat.st_dev)
                shared = true;
        }
    }
    if (shared) {
        hooks.observe = [node](std::chrono::nanoseconds elapsed, uint64_t bytes) {
            try {
                node->local_store().service_monitor().note(elapsed, bytes);
            } catch (const std::exception&) {
                // No DATA pool yet (early start-up) or none at all (edge node).
            }
        };
    }
    Log::info("torrent disk backend threads=" + std::to_string(hooks.threads) +
              " admission=loader monitor=" + (shared ? "data-device" : "none (staging on another device)"));
    return hooks;
}

void TorrentManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        drain_alerts();
        // Piece alerts are a hint; the bitfield is the record. Every few
        // seconds every torrent's held pieces are reported again, so a lost
        // alert delays an extent's publication by at most this interval.
        if (impl_ && Clock::now() - last_held_pieces_report_ >= held_pieces_report_interval) {
            last_held_pieces_report_ = Clock::now();
            for (const auto& [_, handle] : impl_->handles) report_held_pieces(handle);
        }
        update_jobs();
        std::unique_lock lock(mutex_);
        if (has_active_jobs_locked() || alerts_pending_.load(std::memory_order_acquire)) {
            // libtorrent and linked ingest jobs are external progress sources, so
            // active jobs still receive a modest status sample cadence. A fully
            // settled/paused manager blocks until an API operation wakes it.
            cv_.wait_for(lock, stop, std::chrono::milliseconds(500), [this] {
                return !has_active_jobs_locked() || alerts_pending_.load(std::memory_order_acquire);
            });
        } else {
            cv_.wait(lock, stop, [this] {
                return has_active_jobs_locked() || alerts_pending_.load(std::memory_order_acquire);
            });
        }
    }
}

void TorrentManager::update_jobs() {
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (auto& [id, job] : jobs_) {
        if (job.state == TorrentJobState::cancelled || job.state == TorrentJobState::completed ||
            job.state == TorrentJobState::failed)
            continue;

        // Macha's explicit pause is operator intent. libtorrent applies
        // pause asynchronously and status() may briefly report the pre-pause
        // download state; never let that stale observation resume the job in
        // Macha. Linked ingest jobs are still sampled because their own state
        // is authoritative once the torrent payload has been handed over.
        if (job.state == TorrentJobState::paused && !job.ingest_job_id)
            continue;

        if (job.ingest_job_id) {
            auto ingest_job = ingest_.job(*job.ingest_job_id);
            if (!ingest_job) {
                job.state = TorrentJobState::failed;
                job.error = "associated ingest job disappeared";
            } else {
                job.catalogue_total = ingest_job->catalogue_total;
                job.catalogue_pending = ingest_job->catalogue_pending;
                job.catalogue_catalogued = ingest_job->catalogue_catalogued;
                job.catalogue_no_match = ingest_job->catalogue_no_match;
                job.catalogue_failed = ingest_job->catalogue_failed;
                if (ingest_job->state == IngestJobState::completed) {
                    job.state = TorrentJobState::completed;
                    job.error.clear();
                    ingest_.staging().release(id);
                } else if (ingest_job->state == IngestJobState::failed ||
                           ingest_job->state == IngestJobState::cancelled) {
                    job.state = TorrentJobState::failed;
                    job.error = "ingest " + ingest_job_state_name(ingest_job->state) +
                                (ingest_job->error.empty() ? std::string{} : ": " + ingest_job->error);
                } else {
                    if (ingest_job->state == IngestJobState::paused)
                        job.state = TorrentJobState::paused;
                    else if (ingest_job->state == IngestJobState::blocked)
                        job.state = TorrentJobState::blocked;
                    else if (ingest_job->state == IngestJobState::cataloguing)
                        job.state = TorrentJobState::cataloguing;
                    else
                        job.state = TorrentJobState::importing;
                    job.bytes_total = ingest_job->bytes_total;
                    job.bytes_completed = ingest_job->bytes_completed;
                    job.download_rate = ingest_job->rate_bytes_per_second;
                    job.eta_seconds = ingest_job->eta_seconds;
                    job.error = ingest_job->error;
                }
            }
            job.updated_unix_ms = unix_ms();
            changed = true;
            continue;
        }

        auto hit = impl_->handles.find(id);
        if (hit == impl_->handles.end()) continue;
        auto status = hit->second.status(lt::torrent_handle::query_name | lt::torrent_handle::query_accurate_download_counters);
        job.name = sanitize_text(status.name, 1024);
        job.bytes_total = status.total_wanted > 0 ? static_cast<uint64_t>(status.total_wanted) : 0;
        job.bytes_completed = status.total_wanted_done > 0 ? static_cast<uint64_t>(status.total_wanted_done) : 0;
        job.download_rate = status.download_rate > 0 ? static_cast<uint64_t>(status.download_rate) : 0;
        job.upload_rate = status.upload_rate > 0 ? static_cast<uint64_t>(status.upload_rate) : 0;
        job.uploaded_total = status.all_time_upload > 0 ? static_cast<uint64_t>(status.all_time_upload) : 0;
        job.peers = status.num_peers > 0 ? static_cast<unsigned>(status.num_peers) : 0;
        job.seeds = status.num_seeds > 0 ? static_cast<unsigned>(status.num_seeds) : 0;
        if (job.download_rate && job.bytes_total >= job.bytes_completed)
            job.eta_seconds = (job.bytes_total - job.bytes_completed + job.download_rate - 1) / job.download_rate;
        else
            job.eta_seconds.reset();

        if (status.errc) {
            job.state = TorrentJobState::failed;
            job.error = status.errc.message();
            ingest_.staging().release(id);
            changed = true;
            continue;
        }

        if (job.bytes_total) {
            const auto remaining = job.bytes_total > job.bytes_completed ? job.bytes_total - job.bytes_completed : 0;
            if (!ingest_.staging().reserve(id, remaining)) {
                hit->second.pause();
                job.state = TorrentJobState::blocked;
                job.error = "staging size limit reached";
                job.download_rate = 0;
                job.eta_seconds.reset();
                job.updated_unix_ms = unix_ms();
                changed = true;
                continue;
            } else if (job.state == TorrentJobState::blocked && job.error == "staging size limit reached") {
                hit->second.resume();
                job.error.clear();
            }
        }

        // Do not switch exhaustively on libtorrent's state enum. 2.1 adds
        // queued_for_checking/allocating in configurations where older builds
        // do not expose those names, and -Wswitch then turns the otherwise
        // harmless API difference into a build failure. Unknown/pre-download
        // states remain queued until they enter one of the stable states below.
        if (status.state == lt::torrent_status::checking_files ||
            status.state == lt::torrent_status::checking_resume_data) {
            job.state = TorrentJobState::verifying;
        } else if (status.state == lt::torrent_status::downloading_metadata) {
            job.state = TorrentJobState::metadata;
        } else if (status.state == lt::torrent_status::downloading) {
            job.state = TorrentJobState::downloading;
        } else if (status.state == lt::torrent_status::finished ||
                   status.state == lt::torrent_status::seeding) {
            job.state = TorrentJobState::downloaded;
        } else {
            job.state = TorrentJobState::queued;
        }

        if (job.state == TorrentJobState::downloaded) {
            hit->second.pause();
            try {
                const auto ingest_id = ingest_.submit_path(job.save_path, "torrent", job.id,
                                                           job.name, std::nullopt, true, true);
                job.ingest_job_id = ingest_id;
                job.state = TorrentJobState::importing;
                impl_->session.remove_torrent(hit->second);
                impl_->handles.erase(hit);
            } catch (const std::exception& e) {
                job.state = TorrentJobState::failed;
                job.error = "cannot submit completed torrent to ingest: " + std::string(e.what());
            }
        }
        job.updated_unix_ms = unix_ms();
        changed = true;
    }
    if (changed) save_state_locked();
}

} // namespace macha