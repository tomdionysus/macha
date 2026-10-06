// SPDX-License-Identifier: GPL-3.0-or-later
// The libtorrent download engine, built only into the libmacha-torrent plugin.
#include "torrent/torrent_manager.hpp"

#include "crypto.hpp"
#include "durable_file.hpp"
#include "json.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "supervised.hpp"
#include "torrent/torrent_session_policy.hpp"

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

// An add of a torrent some job already holds. Never leaves this file: callers
// see a Placement or RPC reply with the code and the holder's id.
struct TorrentAlreadyAdded : std::runtime_error {
    std::string job_id;
    explicit TorrentAlreadyAdded(std::string holder)
        : std::runtime_error("job " + holder + " already holds this torrent"), job_id(std::move(holder)) {}
};

std::string info_hash_hex(const lt::info_hash_t& hashes) { return torrent_info_hash_hex(hashes); }

std::string sanitize_text(std::string value, size_t limit = 1024) {
    std::erase_if(value, [](unsigned char c) { return c < 0x20 && c != '\t'; });
    if (value.size() > limit) value.resize(limit);
    return trim(std::move(value));
}

namespace lt = libtorrent;

// The session's alert categories for torrent.log_level. error, status,
// port_mapping and dht are always on: the drain reads listen, bootstrap and
// port-mapping outcomes from them. ALL adds libtorrent's log categories.
lt::alert_category_t alert_mask_for(LogLevel level) {
    // piece_progress carries piece_finished_alert, which tells the disk
    // backend an extent's pieces verified.
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
    settings.set_int(lt::settings_pack::alert_mask, alert_mask_for(config.log_level));
    settings.set_int(lt::settings_pack::active_downloads, static_cast<int>(config.max_active));
    settings.set_int(lt::settings_pack::active_limit, static_cast<int>(config.max_active + 4));
    settings.set_int(lt::settings_pack::active_seeds, 0);
    settings.set_bool(lt::settings_pack::enable_dht, config.dht);
    settings.set_bool(lt::settings_pack::enable_lsd, config.lsd);
    // Explicit, not libtorrent's defaults; see TorrentConfig::upnp.
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
    // Peers and trackers only: a torrent's web seeds and DHT nodes would make
    // its metainfo a server-side HTTP fetcher.
    atp.url_seeds.clear();
    atp.dht_nodes.clear();
    std::erase_if(atp.trackers, [](const std::string& tracker) { return !safe_tracker_url(tracker); });
    atp.tracker_tiers.assign(atp.trackers.size(), 0);
    if (!config.dht) atp.flags |= lt::torrent_flags::disable_dht;
    if (!config.lsd) atp.flags |= lt::torrent_flags::disable_lsd;
    if (!config.pex) atp.flags |= lt::torrent_flags::disable_pex;
    // Sequential, so extents publish while still in page cache and a file is
    // watchable soonest.
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

TorrentManager::TorrentManager(NodeRuntime& node, LocalState& local,
                               DataResourceArbiter& data_resources,
                               IngestManager& ingest, TorrentConfig config,
                               const std::filesystem::path& state_path, const TimeSource& time)
    : node_(node), local_(local), data_resources_(data_resources), ingest_(ingest), time_(time),
      config_(std::move(config)),
      enabled_(config_.enabled), state_file_(state_path / "torrent" / "jobs.json"),
      resume_dir_(state_path / "torrent" / "resume") {
    if (!config_.enabled) return;
    // The only signal that a failed job's ingest runs again; a settled manager
    // otherwise sleeps.
    ingest_.set_resume_listener([this](std::string_view) { cv_.notify_all(); });
    std::filesystem::create_directories(state_file_.parent_path());
    impl_ = std::make_unique<Impl>(config_, node_.config().advertise_host, disk_hooks());
    // Called on libtorrent's thread: only wakes the worker, which drains.
    impl_->session.set_alert_notify([this] {
        alerts_pending_.store(true, std::memory_order_release);
        cv_.notify_all();
    });
    load_state();
}

TorrentManager::~TorrentManager() {
    ingest_.set_resume_listener({});
    stop();
}


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
            // A malformed request lists all rather than failing the peer.
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
    // What this node offers for new jobs; peers build their node list from it.
    const auto offered = offer();
    Json::Object node;
    node["accepting"] = offered.accepting;
    node["not_accepting_reason"] =
        offered.not_accepting_reason.empty() ? Json(nullptr) : Json(offered.not_accepting_reason);
    node["max_active"] = static_cast<uint64_t>(offered.max_active);
    node["active_jobs"] = static_cast<uint64_t>(offered.active_jobs);
    node["staging"] = staging_capacity_json(ingest_.staging().status());
    out["node"] = std::move(node);
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
    // For a node without the plugin: a .torrent's canonical magnet and info hash.
    if (action == "resolve") {
        Json::Object out;
        try {
            const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                                   request_payload.size());
            auto request = Json::parse(text);
            std::string uri;
            bool search_result = false;
            if (const auto* u = request.find("uri"); u && u->isString()) uri = u->asString();
            if (const auto* v = request.find("search_result"); v && v->isBool()) search_result = v->asBool();
            const auto resolved = resolve(uri, search_result);
            out["resolved"] = true;
            out["magnet"] = resolved.magnet;
            out["info_hash"] = resolved.info_hash;
            out["name"] = resolved.name;
        } catch (const std::exception& error) {
            out["resolved"] = false;
            out["error_code"] = std::string("add_failed");
            out["error"] = std::string(error.what());
        }
        const auto text = Json(std::move(out)).dump();
        return Bytes(text.begin(), text.end());
    }
    // A targeted add is an action with no job id: the job is created here.
    if (action == "add") {
        std::string uri;
        bool search_result = false;
        try {
            const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                                   request_payload.size());
            auto request = Json::parse(text);
            if (const auto* u = request.find("uri"); u && u->isString()) uri = u->asString();
            if (const auto* s = request.find("search_result"); s && s->isBool()) search_result = s->asBool();
        } catch (const std::exception&) {
        }
        Json::Object added;
        if (uri.empty()) {
            added["exists"] = false;
            added["error_code"] = std::string("missing_uri");
            added["error"] = std::string("a magnet or torrent uri is required");
        } else {
            const auto placement = place(uri, search_result);
            added["exists"] = placement.placed;
            if (!placement.job_id.empty()) added["job_id"] = placement.job_id;
            if (placement.job) added["job"] = torrent_job_wire_json(*placement.job);
            if (!placement.placed) {
                added["error_code"] = placement.reason;
                added["error"] = placement.error;
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

// A search result's URI may be a .torrent URL, which only add_search_result fetches.
TorrentService::Placement TorrentManager::place(std::string_view magnet_or_uri, bool search_result) {
    Placement placement;
    placement.node_id = node_.node_id();
    bool accepting;
    {
        Lock lock(mutex_);
        accepting = config_.accept_new_jobs;
    }
    if (!accepting) {
        placement.reason = "node_not_torrent_capable";
        placement.error = "this node is draining (torrent.accept_new_jobs is false)";
        return placement;
    }
    try {
        placement.job_id = search_result ? add_search_result(std::string(magnet_or_uri))
                                         : add(std::string(magnet_or_uri));
        placement.placed = true;
        placement.job = job(placement.job_id);
    } catch (const TorrentAlreadyAdded& held) {
        placement.reason = "torrent_already_added";
        placement.error = held.what();
        placement.job_id = held.job_id;
        placement.job = job(held.job_id);
    } catch (const std::exception& error) {
        placement.reason = "add_failed";
        placement.error = error.what();
    }
    return placement;
}

TorrentService::Offer TorrentManager::offer() const {
    Offer out;
    bool accept_new_jobs;
    {
        Lock lock(mutex_);
        out.max_active = config_.max_active;
        accept_new_jobs = config_.accept_new_jobs;
        for (const auto& [_, job] : jobs_) {
            switch (job.state) {
            case TorrentJobState::queued:
            case TorrentJobState::metadata:
            case TorrentJobState::downloading:
            case TorrentJobState::verify_queued:
            case TorrentJobState::verifying:
            case TorrentJobState::downloaded:
                ++out.active_jobs;
                break;
            default:
                break;
            }
        }
    }
    const auto staging = ingest_.staging().status();
    if (!accept_new_jobs)
        out.not_accepting_reason = "draining";
    else if (out.max_active && out.active_jobs >= out.max_active)
        out.not_accepting_reason = "slots_full";
    else if (staging.accounted_bytes >= staging.limit)
        out.not_accepting_reason = "staging_full";
    out.accepting = out.not_accepting_reason.empty();
    return out;
}

void TorrentManager::load_state() {
    Lock lock(mutex_);
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
            if (job.info_hash.empty() && !job.source_uri.empty()) {
                try {
                    job.info_hash = info_hash_hex(lt::parse_magnet_uri(job.source_uri).info_hashes);
                } catch (const std::exception&) {
                }
            }
            if (job.state == TorrentJobState::metadata || job.state == TorrentJobState::downloading ||
                job.state == TorrentJobState::verify_queued || job.state == TorrentJobState::verifying ||
                job.state == TorrentJobState::downloaded)
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

void TorrentManager::set_fault_sink(std::function<void(std::string)> sink) {
    Lock lock(mutex_);
    fault_sink_ = std::move(sink);
}

void TorrentManager::start() {
    if (!enabled_ || worker_.joinable()) return;
    restore_jobs();
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_escalating("torrent", [this, stop] { loop(stop); }, [this](std::string reason) {
            std::function<void(std::string)> sink;
            {
                Lock lock(mutex_);
                sink = fault_sink_;
            }
            if (sink) sink(std::move(reason));
        });
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
    save_all_resume_data();
}

std::filesystem::path TorrentManager::resume_path(std::string_view id) const {
    return resume_dir_ / (std::string(id) + ".resume");
}

void TorrentManager::request_resume_save(const lt::torrent_handle& handle) {
    if (!handle.is_valid()) return;
    // With the info dictionary, so a magnet restarts without refetching metadata.
    handle.save_resume_data(lt::torrent_handle::only_if_modified | lt::torrent_handle::save_info_dict);
}

void TorrentManager::write_resume_alert(const lt::torrent_handle& handle,
                                        const lt::add_torrent_params& params) {
    std::string id;
    {
        Lock lock(mutex_);
        if (!impl_) return;
        for (const auto& [job_id, candidate] : impl_->handles)
            if (candidate == handle) {
                id = job_id;
                break;
            }
    }
    if (id.empty()) return;
    try {
        store_torrent_resume(resume_path(id), params);
    } catch (const std::exception& e) {
        // Losing it costs a re-check at the next start, nothing more.
        Log::warn("torrent resume data not written id=" + id + ": " + e.what());
    }
}

void TorrentManager::save_all_resume_data() {
    if (!impl_) return;
    size_t outstanding = 0;
    {
        Lock lock(mutex_);
        for (const auto& [_, handle] : impl_->handles) {
            if (!handle.is_valid()) continue;
            // Unconditional at stop, so no change is lost to only_if_modified.
            handle.save_resume_data(lt::torrent_handle::save_info_dict);
            ++outstanding;
        }
    }
    // Bounded so stop never hangs; anything unsaved is re-checked at start.
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (outstanding && Clock::now() < deadline) {
        impl_->session.wait_for_alert(std::chrono::milliseconds(200));
        std::vector<lt::alert*> alerts;
        impl_->session.pop_alerts(&alerts);
        for (const auto* alert : alerts) {
            if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {
                write_resume_alert(saved->handle, saved->params);
                --outstanding;
            } else if (lt::alert_cast<lt::save_resume_data_failed_alert>(alert)) {
                --outstanding;
            }
        }
    }
    if (outstanding)
        Log::warn("torrent stop: " + std::to_string(outstanding) +
                  " torrents' resume data not saved in time; they will be re-checked at the next start");
}

void TorrentManager::reconfigure(TorrentConfig config) {
    Lock lock(mutex_);
    if (config.enabled != config_.enabled) Log::warn("torrent.enabled changes require restart");
    config_.max_active = config.max_active;
    config_.max_download_rate = config.max_download_rate;
    config_.max_upload_rate = config.max_upload_rate;
    config_.accept_new_jobs = config.accept_new_jobs;
    config_.remove_on_complete_after = config.remove_on_complete_after;
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
        Lock lock(mutex_);
        for (const auto& [_, job] : jobs_) {
            if (job.state == TorrentJobState::completed || job.state == TorrentJobState::cancelled ||
                job.state == TorrentJobState::failed || job.state == TorrentJobState::importing)
                continue;
            restore.push_back(job);
        }
    }
    // Oldest first: of two jobs recorded for one torrent, the first keeps it.
    std::stable_sort(restore.begin(), restore.end(), [](const TorrentJob& a, const TorrentJob& b) {
        return a.created_unix_ms < b.created_unix_ms;
    });
    std::map<std::string, std::string, std::less<>> restored_by_hash;
    for (const auto& job : restore) {
        if (!job.info_hash.empty()) {
            const auto [holder, first] = restored_by_hash.try_emplace(job.info_hash, job.id);
            if (!first) {
                Lock lock(mutex_);
                auto& mutable_job = jobs_[job.id];
                mutable_job.state = TorrentJobState::failed;
                mutable_job.error_code = "duplicate_torrent";
                mutable_job.error = "job " + holder->second + " holds the same torrent";
                mutable_job.updated_unix_ms = unix_ms();
                save_state_locked();
                Log::warn("torrent job not restored id=" + job.id + ": job " + holder->second +
                          " holds the same torrent info_hash=" + job.info_hash);
                continue;
            }
        }
        try {
            // Resume data names the verified pieces, so nothing is re-hashed.
            std::optional<lt::add_torrent_params> resumed;
            if (!job.info_hash.empty()) resumed = load_torrent_resume(resume_path(job.id), job.info_hash);
            lt::add_torrent_params atp;
            if (resumed) {
                atp = std::move(*resumed);
            } else {
                auto sanitized = sanitize_magnet_uri(job.source_uri);
                if (!sanitized) throw std::runtime_error("invalid persisted torrent magnet");
                atp = lt::parse_magnet_uri(*sanitized);
            }
            atp.save_path = job.save_path.string();
            // Held from add for operator pauses: paused after add, libtorrent's
            // queue would start an auto-managed torrent. Not staging_full jobs:
            // held, a magnet never learns its size, so the staging check that
            // would release it never runs; it re-holds once the size is known.
            set_hold_at_add(atp, job.state == TorrentJobState::paused);
            atp.flags |= lt::torrent_flags::duplicate_is_error;
            Lock lock(mutex_);
            harden_add_params(atp, config_);
            impl_->handles[job.id] = impl_->session.add_torrent(std::move(atp));
        } catch (const std::exception& e) {
            Lock lock(mutex_);
            auto& mutable_job = jobs_[job.id];
            mutable_job.state = TorrentJobState::failed;
            mutable_job.error_code = "restore_failed";
            mutable_job.error = e.what();
            mutable_job.updated_unix_ms = unix_ms();
            save_state_locked();
        }
    }
}

struct TorrentManager::ParsedAdd {
    lt::add_torrent_params params;
    std::string source_uri;
};

void TorrentManager::parse_add_uri(std::string uri, bool allow_fetch, ParsedAdd& parsed) {
    auto& atp = parsed.params;
    if (auto magnet = sanitize_magnet_uri(uri)) {
        parsed.source_uri = *magnet;
        atp = lt::parse_magnet_uri(*magnet);
    } else if (allow_fetch && safe_torrent_fetch_url(uri)) {
        auto fetched = impl_->http.get(uri, {}, 4 * 1024 * 1024);
        if (fetched.status < 200 || fetched.status >= 300)
            throw std::runtime_error("torrent URL returned HTTP " + std::to_string(fetched.status));
        std::vector<char> buffer(fetched.body.begin(), fetched.body.end());
        try {
            // The throwing overload: some 2.1 builds lack the error_code one.
            atp = lt::load_torrent_buffer(lt::span<char const>(buffer.data(), buffer.size()));
        } catch (const std::exception& e) {
            throw std::runtime_error("invalid .torrent file: " + std::string(e.what()));
        }
        // Persist a canonical magnet from the metainfo, never the download URL,
        // which may carry credentials.
        parsed.source_uri = lt::make_magnet_uri(atp);
        if (auto sanitized = sanitize_magnet_uri(parsed.source_uri)) parsed.source_uri = *sanitized;
    } else {
        throw std::runtime_error(allow_fetch
                                     ? "torrent acquisition must be a magnet or trusted http(s) .torrent URL"
                                     : "torrent job requires a magnet URI");
    }
}

TorrentService::Resolved TorrentManager::resolve(std::string_view uri, bool search_result) {
    if (!enabled_) throw std::runtime_error("torrent support is disabled");
    ParsedAdd parsed;
    parse_add_uri(std::string(uri), search_result, parsed);
    Resolved out;
    out.magnet = parsed.source_uri;
    out.info_hash = info_hash_hex(parsed.params.ti ? parsed.params.ti->info_hashes() : parsed.params.info_hashes);
    out.name = sanitize_text(parsed.params.ti ? parsed.params.ti->name() : parsed.params.name, 1024);
    if (out.info_hash.empty()) throw std::runtime_error("the torrent names no info hash");
    return out;
}

std::string TorrentManager::adopt(std::string_view id, std::string_view magnet, bool held) {
    {
        Lock lock(mutex_);
        if (jobs_.contains(id)) return std::string(id);
    }
    ParsedAdd parsed;
    parse_add_uri(std::string(magnet), false, parsed);
    return add_parsed(std::string(id), parsed, held);
}

std::string TorrentManager::add_impl(std::string uri, bool allow_fetch) {
    if (!enabled_) throw std::runtime_error("torrent support is disabled");
    ParsedAdd parsed;
    parse_add_uri(std::move(uri), allow_fetch, parsed);
    return add_parsed(to_string(random_node_id()), parsed, false);
}

std::string TorrentManager::add_parsed(std::string id, ParsedAdd& parsed, bool held) {
    if (!enabled_) throw std::runtime_error("torrent support is disabled");
    TorrentJob job;
    job.id = std::move(id);
    job.created_unix_ms = job.updated_unix_ms = unix_ms();
    job.save_path = ingest_.staging().path() / "torrents" / job.id;
    job.source_uri = std::move(parsed.source_uri);
    auto& atp = parsed.params;
    // Held from add when paused: paused after add, libtorrent's queue would
    // start an auto-managed torrent.
    set_hold_at_add(atp, held);
    atp.save_path = job.save_path.string();
    // Backstop to the check below: libtorrent would answer a second add with
    // the first one's handle, so a hash Macha missed (a v2-only magnet for a
    // hybrid torrent) would make two jobs share one torrent.
    atp.flags |= lt::torrent_flags::duplicate_is_error;
    const auto wanted = atp.ti ? atp.ti->info_hashes() : atp.info_hashes;
    const auto info_hash = info_hash_hex(wanted);

    // Checked and added under one lock, so two concurrent adds of one torrent
    // cannot both pass the check.
    Lock lock(mutex_);
    harden_add_params(atp, config_);
    if (!info_hash.empty())
        if (auto holder = job_holding_locked(info_hash)) {
            Log::info("torrent add refused: job " + *holder + " already holds info_hash=" + info_hash);
            throw TorrentAlreadyAdded(*holder);
        }
    std::filesystem::create_directories(job.save_path);
    lt::torrent_handle handle;
    try {
        handle = impl_->session.add_torrent(std::move(atp));
    } catch (const lt::system_error& e) {
        std::error_code ec;
        std::filesystem::remove_all(job.save_path, ec);
        if (e.code() == lt::errors::duplicate_torrent) {
            // libtorrent matched a hash Macha's comparison did not (a v2-only
            // magnet for a hybrid torrent): name the job whose torrent it is.
            for (const auto& [id, candidate] : impl_->handles) {
                const auto held = candidate.info_hashes();
                if ((wanted.has_v1() && held.has_v1() && wanted.v1 == held.v1) ||
                    (wanted.has_v2() && held.has_v2() && wanted.v2 == held.v2))
                    throw TorrentAlreadyAdded(id);
            }
        }
        throw;
    }
    auto status = handle.status(lt::torrent_handle::query_name);
    job.name = sanitize_text(status.name, 1024);
    job.info_hash = info_hash_hex(status.info_hashes);
    job.state = held ? TorrentJobState::paused
              : status.state == lt::torrent_status::downloading_metadata ? TorrentJobState::metadata
                                                                         : TorrentJobState::queued;
    jobs_[job.id] = job;
    impl_->handles[job.id] = std::move(handle);
    try {
        save_state_locked();
    } catch (...) {
        // A failed admission must leave nothing running: roll back the handle
        // and record before propagating.
        retire_torrent_locked(job.id, true);
        jobs_.erase(job.id);
        std::error_code ec;
        std::filesystem::remove_all(job.save_path, ec);
        throw;
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
    Lock lock(mutex_);
    std::vector<TorrentJob> out;
    for (const auto& [_, job] : jobs_) out.push_back(job);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.created_unix_ms > b.created_unix_ms; });
    return out;
}

std::optional<TorrentJob> TorrentManager::job(std::string_view id) const {
    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return {};
    return it->second;
}

bool TorrentManager::pause(std::string_view id) {
    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == TorrentJobState::completed || it->second.state == TorrentJobState::cancelled ||
        it->second.state == TorrentJobState::failed) return false;
    if (it->second.ingest_job_id && !ingest_.pause(*it->second.ingest_job_id)) return false;
    if (!it->second.ingest_job_id)
        if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) {
            hold_torrent(h->second);
            request_resume_save(h->second);
        }
    it->second.state = TorrentJobState::paused;
    it->second.download_rate = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool TorrentManager::resume(std::string_view id) {
    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state != TorrentJobState::paused && it->second.state != TorrentJobState::blocked) return false;
    if (it->second.ingest_job_id) {
        if (!ingest_.resume(*it->second.ingest_job_id)) return false;
    }
    else if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) release_torrent(h->second);
    it->second.state = it->second.ingest_job_id ? TorrentJobState::importing : TorrentJobState::queued;
    it->second.error.clear();
    it->second.error_code.clear();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool TorrentManager::retry(std::string_view id) {
    Lock lock(mutex_);
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
    job.error_code.clear();
    job.updated_unix_ms = unix_ms();
    try {
        // Persisted first: a crash before ingest.resume() leaves update_jobs()
        // to converge this job back to failed without touching the payload.
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
    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == TorrentJobState::completed || it->second.state == TorrentJobState::cancelled) return false;
    if (it->second.ingest_job_id) (void)ingest_.cancel(*it->second.ingest_job_id);
    const bool delete_payload = ingest_.delete_owned_source_on_cancel();
    retire_torrent_locked(it->first, delete_payload);
    ingest_.staging().release(it->first);
    if (delete_payload && !ingest_.staging().discard(it->second.save_path)) {
        std::error_code ec;
        std::filesystem::remove_all(it->second.save_path, ec);
        if (ec) Log::warn("torrent cancel staging cleanup failed id=" + it->first + ": " + ec.message());
    }
    it->second.state = TorrentJobState::cancelled;
    it->second.download_rate = it->second.upload_rate = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    Log::info("torrent cancelled id=" + it->first + " payload_deleted=" + (delete_payload ? "true" : "false"));
    cv_.notify_all();
    return true;
}

bool TorrentManager::clear(std::string_view id) {
    TorrentJob terminal_job;
    {
        Lock lock(mutex_);
        auto it = jobs_.find(std::string(id));
        if (it == jobs_.end()) return false;
        if (it->second.state != TorrentJobState::completed &&
            it->second.state != TorrentJobState::cancelled &&
            it->second.state != TorrentJobState::failed)
            return false;
        terminal_job = it->second;
        // A job failed in libtorrent still has a torrent: retire it before the
        // payload goes.
        retire_torrent_locked(terminal_job.id, false);
    }

    bool linked_cleared = false;
    if (terminal_job.ingest_job_id) {
        if (auto linked = ingest_.job(*terminal_job.ingest_job_id)) {
            if (!ingest_.clear(*terminal_job.ingest_job_id)) return false;
            linked_cleared = true;
        }
    }
    if (!linked_cleared && ingest_.delete_owned_source_on_clear() &&
        !ingest_.staging().discard(terminal_job.save_path)) {
        std::error_code ec;
        std::filesystem::remove_all(terminal_job.save_path, ec);
        if (ec) throw std::runtime_error("cannot clear torrent staging payload: " + ec.message());
    }
    ingest_.staging().release(terminal_job.id);

    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state != terminal_job.state) return false;
    jobs_.erase(it);
    save_state_locked();
    Log::info("torrent cleared id=" + terminal_job.id);
    cv_.notify_all();
    return true;
}

std::optional<std::string> TorrentManager::job_holding_locked(std::string_view info_hash) const {
    for (const auto& [id, job] : jobs_)
        if (job.info_hash == info_hash) return id;
    return std::nullopt;
}

void TorrentManager::retire_torrent_locked(const std::string& id, bool delete_payload) {
    publication_waits_.erase(id);
    publication_seen_.erase(id);
    check_samples_.erase(id);
    if (impl_) {
        if (auto h = impl_->handles.find(id); h != impl_->handles.end()) {
            try {
                if (delete_payload)
                    impl_->session.remove_torrent(h->second,
                                                  lt::session::delete_files | lt::session::delete_partfile);
                else
                    impl_->session.remove_torrent(h->second);
            } catch (const std::exception& e) {
                // Already gone from the session: what matters is that no
                // handle to it stays behind.
                Log::warn("torrent remove failed id=" + id + ": " + e.what());
            }
            impl_->handles.erase(h);
        }
    }
    std::error_code ec;
    std::filesystem::remove(resume_path(id), ec);
}

void TorrentManager::isolate_fault_locked(const std::string& id, std::string_view what) {
    Log::error("torrent job faulted id=" + id + ": " + std::string(what) +
               "; the job is failed and every other job carries on");
    retire_torrent_locked(id, false);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    auto& job = it->second;
    // A job linked to an ingest keeps following it: update_jobs brings a
    // failed job back while its ingest runs.
    job.state = TorrentJobState::failed;
    job.error_code = "torrent_fault";
    job.error = std::string(what);
    job.download_rate = job.upload_rate = 0;
    job.eta_seconds.reset();
    job.updated_unix_ms = unix_ms();
    if (!job.ingest_job_id) ingest_.staging().release(id);
    try {
        save_state_locked();
    } catch (const std::exception& e) {
        Log::warn("torrent state not saved after fault id=" + id + ": " + e.what());
    }
}

bool TorrentManager::linked_ingest_revived(const TorrentJob& job) const {
    if (job.state != TorrentJobState::failed || !job.ingest_job_id) return false;
    const auto linked = ingest_.job(*job.ingest_job_id);
    return linked && linked->state != IngestJobState::failed &&
           linked->state != IngestJobState::cancelled;
}

bool TorrentManager::has_active_jobs_locked() const {
    return std::any_of(jobs_.begin(), jobs_.end(), [this](const auto& pair) {
        const auto state = pair.second.state;
        if (state == TorrentJobState::failed) return linked_ingest_revived(pair.second);
        return state != TorrentJobState::completed && state != TorrentJobState::cancelled &&
               state != TorrentJobState::paused;
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
        // An alert can outlive its torrent: act only on a handle a job still
        // holds, under the lock that keeps it held.
        if (const auto* finished = lt::alert_cast<lt::piece_finished_alert>(alert)) {
            Lock lock(mutex_);
            if (const auto* job = job_of_locked(finished->handle))
                verifications_->piece_verified(job->save_path.string(),
                                               static_cast<int>(finished->piece_index));
            continue;
        }
        // A check reports no piece alerts and piece alerts can drop, so here
        // every held piece is reported from the bitfield.
        const lt::torrent_handle* settled = nullptr;
        if (const auto* checked = lt::alert_cast<lt::torrent_checked_alert>(alert))
            settled = &checked->handle;
        else if (const auto* finished = lt::alert_cast<lt::torrent_finished_alert>(alert))
            settled = &finished->handle;
        if (settled) {
            Lock lock(mutex_);
            if (auto* job = job_of_locked(*settled)) {
                const auto id = job->id;
                try {
                    report_held_pieces_locked(*job, *settled);
                    // A finished check is exactly what a restart must not repeat.
                    request_resume_save(*settled);
                } catch (const std::exception& e) {
                    isolate_fault_locked(id, e.what());
                }
            }
            continue;
        }
        if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {
            write_resume_alert(saved->handle, saved->params);
            continue;
        }
        if (const auto* unsaved = lt::alert_cast<lt::save_resume_data_failed_alert>(alert)) {
            // only_if_modified's "not modified" arrives this way: debug only.
            Log::debug("torrent resume data not saved: " + unsaved->message());
            continue;
        }
        // The alert queue is bounded and drops on overflow; say so.
        if (const auto* dropped = lt::alert_cast<lt::alerts_dropped_alert>(alert)) {
            Log::warn("torrent alert queue overflowed: " + std::to_string(dropped->dropped_alerts.count()) +
                      " alert types lost; verified pieces are also taken from each torrent's own "
                      "bitfield, so extent publication does not depend on them");
            continue;
        }
        // No inbound port means no peer can dial in and the node never seeds:
        // said once per session above debug.
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
                uint16_t listen_port;
                {
                    Lock lock(mutex_);
                    listen_port = config_.listen_port;
                }
                Log::warn("torrent port mapping failed (" + map_failed->message() +
                          "): this node has no inbound port, so it can dial peers but "
                          "none can dial it -- expect low peer counts and no seeding. "
                          "Forward " + std::to_string(listen_port) +
                          " TCP+UDP by hand, or set torrent.upnp/torrent.natpmp false "
                          "to stop trying.");
            }
            continue;
        }
        // libtorrent's chatter obeys torrent.log_level, not the process level,
        // and then emits past the process filter, like the ffmpeg bridge.
        if (alert_log_level_.load(std::memory_order_relaxed) <= LogLevel::debug)
            Log::emit(LogLevel::debug,
                      std::string("torrent alert ") + alert->what() + ": " + alert->message());
    }

    // Only loopback sockets reach no peer: a configuration fault, said once.
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

void TorrentManager::report_held_pieces_locked(const TorrentJob& job, const lt::torrent_handle& handle) {
    const auto path = job.save_path.string();
    const auto held = handle.status(lt::torrent_handle::query_pieces).pieces;
    for (int piece = 0; piece < held.size(); ++piece)
        if (held.get_bit(lt::piece_index_t(piece))) verifications_->piece_verified(path, piece);
}

TorrentJob* TorrentManager::job_of_locked(const lt::torrent_handle& handle) {
    if (!impl_) return nullptr;
    for (const auto& [id, candidate] : impl_->handles) {
        if (candidate != handle) continue;
        const auto job = jobs_.find(id);
        return job == jobs_.end() ? nullptr : &job->second;
    }
    return nullptr;
}

void TorrentManager::refresh_publication_locked(const std::string& id, TorrentJob& job) {
    const auto progress = verifications_->publication(job.save_path.string());
    if (!progress) {
        job.publication.reset();
        job.waiting_reason.clear();
        publication_seen_.erase(id);
        return;
    }
    const auto now = Clock::now();
    auto [seen, first] = publication_seen_.try_emplace(id, PublicationWait{progress->published, now});
    if (!first && seen->second.published != progress->published) seen->second = {progress->published, now};
    const uint64_t extent = node_.config().extent_size;
    TorrentJob::Publication publication;
    publication.published_extents = progress->published;
    publication.extents = progress->extents;
    publication.bytes = job.bytes_total ? job.bytes_total : progress->extents * extent;
    publication.published_bytes = std::min<uint64_t>(progress->published * extent, publication.bytes);
    publication.progress_age_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - seen->second.advanced).count());
    job.publication = publication;
    job.waiting_reason = job.state == TorrentJobState::downloaded && !progress->complete()
                             ? "extent_publication"
                             : "";
}

bool TorrentManager::publication_settled_locked(const std::string& id, const TorrentJob& job) {
    // Settled when the backend is not publishing or does not hold the torrent.
    const auto progress = verifications_->publication(job.save_path.string());
    if (!progress || progress->complete()) {
        if (publication_waits_.erase(id) && progress)
            Log::info("torrent extents published; importing id=" + id +
                      " extents=" + std::to_string(progress->extents));
        return true;
    }
    const auto now = Clock::now();
    const auto [wait, first] = publication_waits_.try_emplace(id, PublicationWait{progress->published, now});
    if (first) {
        Log::info("torrent downloaded; import waits for extent publication id=" + id +
                  " published=" + std::to_string(progress->published) +
                  " extents=" + std::to_string(progress->extents));
        return false;
    }
    if (progress->published != wait->second.published) {
        wait->second = {progress->published, now};
        return false;
    }
    if (now - wait->second.advanced < publication_stall_limit) return false;
    Log::warn("torrent extent publication made no progress for " +
              std::to_string(std::chrono::duration_cast<std::chrono::minutes>(publication_stall_limit).count()) +
              " min id=" + id + " published=" + std::to_string(progress->published) +
              " extents=" + std::to_string(progress->extents) + "; importing now, the rest is copied");
    publication_waits_.erase(wait);
    return true;
}

TorrentDiskHooks TorrentManager::disk_hooks() const {
    TorrentDiskHooks hooks;
    {
        Lock lock(mutex_);
        hooks.threads = config_.disk_threads;
    }
    hooks.admit = loader_admission(data_resources_);
    // Every verified extent is published durably at loader class into the
    // ingest's store and journalled; the ingest commits files by naming them.
    hooks.extent_size = node_.config().extent_size;
    hooks.verifications = verifications_;
    FileSystem* fs = &ingest_.filesystem();
    hooks.publish = [fs](std::span<const uint8_t> bytes,
                         std::atomic_bool& abort) -> std::optional<ObjectId> {
        try {
            return fs->store().put(bytes, FrameType::loader, &abort);
        } catch (const std::exception& e) {
            if (!abort.load())
                Log::debug(std::string("torrent extent put failed: ") + e.what());
            return std::nullopt;
        }
    };
    // The DATA device's monitor hears the torrent's I/O only when staging
    // shares a DATA backend's device.
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
        hooks.observe = [monitor = &local_.data().service_monitor()](
                            std::chrono::nanoseconds elapsed, uint64_t bytes) {
            monitor->note(elapsed, bytes);
        };
    }
    Log::info("torrent disk backend threads=" + std::to_string(hooks.threads) +
              " admission=loader monitor=" + (shared ? "data-device" : "none (staging on another device)"));
    return hooks;
}

void TorrentManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        drain_alerts();
        // Piece alerts are a hint, the bitfield the record: re-reporting held
        // pieces bounds a lost alert's delay to this interval. impl_->handles
        // changes only under mutex_ and only via retire_torrent_locked, so
        // each handle names a live torrent; a throw fails that job only.
        if (const auto now = time_.now(); impl_ && now - last_held_pieces_report_ >= held_pieces_report_interval) {
            last_held_pieces_report_ = now;
            Lock lock(mutex_);
            std::vector<std::pair<std::string, std::string>> faults;
            for (const auto& [id, handle] : impl_->handles) {
                const auto job = jobs_.find(id);
                if (job == jobs_.end()) continue;
                try {
                    report_held_pieces_locked(job->second, handle);
                } catch (const std::exception& e) {
                    faults.emplace_back(id, e.what());
                }
            }
            for (const auto& [id, what] : faults) isolate_fault_locked(id, what);
        }
        if (const auto now = time_.now(); impl_ && now - last_resume_save_ >= resume_save_interval) {
            last_resume_save_ = now;
            Lock lock(mutex_);
            std::vector<std::pair<std::string, std::string>> faults;
            for (const auto& [id, handle] : impl_->handles) {
                try {
                    request_resume_save(handle);
                } catch (const std::exception& e) {
                    faults.emplace_back(id, e.what());
                }
            }
            for (const auto& [id, what] : faults) isolate_fault_locked(id, what);
        }
        update_jobs();
        Lock lock(mutex_);
        if (has_active_jobs_locked() || alerts_pending_.load(std::memory_order_acquire)) {
            // Active jobs are sampled at a modest cadence; a settled manager
            // blocks until woken.
            cv_.wait_for(lock.native(), stop, std::chrono::milliseconds(500), [this]() MACHA_REQUIRES(mutex_) {
                return !has_active_jobs_locked() || alerts_pending_.load(std::memory_order_acquire);
            });
        } else {
            cv_.wait(lock.native(), stop, [this]() MACHA_REQUIRES(mutex_) {
                return has_active_jobs_locked() || alerts_pending_.load(std::memory_order_acquire);
            });
        }
    }
}

void TorrentManager::update_jobs() {
    Lock lock(mutex_);
    bool changed = false;
    bool progress_moved = false;
    std::vector<std::pair<std::string, std::string>> faults;
    for (auto& entry : jobs_) {
        const auto& id = entry.first;
        auto& job = entry.second;
        // One job's fault is that job's, not the worker's.
        try {
            const auto before = job;
            [&]() MACHA_REQUIRES(mutex_) {
                if (job.state == TorrentJobState::cancelled || job.state == TorrentJobState::completed)
                    return;
                // A failed job whose ingest runs again follows it below.
                if (job.state == TorrentJobState::failed && !linked_ingest_revived(job))
                    return;

                // A pause stands: libtorrent pauses asynchronously and status()
                // may briefly show it downloading. A linked ingest is still
                // sampled, as its state is authoritative after hand-over.
                if (job.state == TorrentJobState::paused && !job.ingest_job_id)
                    return;

                if (job.ingest_job_id) {
                    auto ingest_job = ingest_.job(*job.ingest_job_id);
                    if (!ingest_job) {
                        job.state = TorrentJobState::failed;
                        job.error_code = "ingest_missing";
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
                            job.error_code.clear();
                            ingest_.staging().release(id);
                        } else if (ingest_job->state == IngestJobState::failed ||
                                   ingest_job->state == IngestJobState::cancelled) {
                            job.state = TorrentJobState::failed;
                            job.error_code = ingest_job->state == IngestJobState::cancelled ? "ingest_cancelled"
                                             : !ingest_job->error_code.empty()          ? ingest_job->error_code
                                                                                        : "ingest_failed";
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
                            job.error_code = ingest_job->error_code;
                            job.error = ingest_job->error;
                        }
                    }
                    return;
                }

                auto hit = impl_->handles.find(id);
                if (hit == impl_->handles.end()) return;
                auto status = hit->second.status(lt::torrent_handle::query_name | lt::torrent_handle::query_accurate_download_counters);
                job.name = sanitize_text(status.name, 1024);
                if (job.info_hash.empty()) job.info_hash = info_hash_hex(status.info_hashes);
                job.bytes_total = status.total_wanted > 0 ? static_cast<uint64_t>(status.total_wanted) : 0;
                job.bytes_completed = status.total_wanted_done > 0 ? static_cast<uint64_t>(status.total_wanted_done) : 0;
                job.download_rate = status.download_rate > 0 ? static_cast<uint64_t>(status.download_rate) : 0;
                job.upload_rate = status.upload_rate > 0 ? static_cast<uint64_t>(status.upload_rate) : 0;
                job.uploaded_total = status.all_time_upload > 0 ? static_cast<uint64_t>(status.all_time_upload) : 0;
                job.peers = status.num_peers > 0 ? static_cast<unsigned>(status.num_peers) : 0;
                job.seeds = status.num_seeds > 0 ? static_cast<unsigned>(status.num_seeds) : 0;
                TorrentJob::Swarm swarm;
                if (status.num_complete >= 0) swarm.seeds = static_cast<uint64_t>(status.num_complete);
                if (status.num_incomplete >= 0) swarm.peers = static_cast<uint64_t>(status.num_incomplete);
                swarm.availability = std::max(0.0, static_cast<double>(status.distributed_copies));
                job.swarm = swarm;
                if (job.download_rate && job.bytes_total >= job.bytes_completed)
                    job.eta_seconds = (job.bytes_total - job.bytes_completed + job.download_rate - 1) / job.download_rate;
                else
                    job.eta_seconds.reset();

                if (status.errc) {
                    job.state = TorrentJobState::failed;
                    job.error_code = "torrent_error";
                    job.error = status.errc.message();
                    ingest_.staging().release(id);
                    return;
                }

                if (job.bytes_total) {
                    const auto remaining = job.bytes_total > job.bytes_completed ? job.bytes_total - job.bytes_completed : 0;
                    if (!ingest_.staging().reserve(id, remaining)) {
                        hold_torrent(hit->second);
                        job.state = TorrentJobState::blocked;
                        job.error_code = "staging_full";
                        job.error = "staging size limit reached";
                        job.download_rate = 0;
                        job.eta_seconds.reset();
                        return;
                    } else if (job.state == TorrentJobState::blocked && job.error_code == "staging_full") {
                        release_torrent(hit->second);
                        job.error.clear();
                        job.error_code.clear();
                    }
                }

                // No exhaustive switch: libtorrent versions differ in state names
                // and -Wswitch would fail the build. Unknown states read queued.
                if (const auto phase = torrent_check_phase(status); phase == TorrentCheckPhase::queued) {
                    // Waiting for another torrent's check; libtorrent checks one
                    // at a time.
                    job.state = TorrentJobState::verify_queued;
                    job.eta_seconds.reset();
                    check_samples_.erase(id);
                } else if (phase == TorrentCheckPhase::checking) {
                    job.state = TorrentJobState::verifying;
                    // The check's own rate: how far through the payload it has read,
                    // not the verified bytes (a check of a partial download finds
                    // most pieces absent and still has to read past them).
                    const auto total = status.total_wanted > 0 ? static_cast<uint64_t>(status.total_wanted) : 0;
                    const auto checked = static_cast<uint64_t>(static_cast<double>(total) *
                                                               std::clamp(static_cast<double>(status.progress), 0.0, 1.0));
                    const auto now = Clock::now();
                    auto& sample = check_samples_[id];
                    if (sample.at != Clock::time_point{} && checked >= sample.checked) {
                        const auto seconds = std::chrono::duration<double>(now - sample.at).count();
                        if (seconds >= 1.0) {
                            const double instant = static_cast<double>(checked - sample.checked) / seconds;
                            sample.rate = sample.rate > 0 ? 0.7 * sample.rate + 0.3 * instant : instant;
                            sample.checked = checked;
                            sample.at = now;
                        }
                    } else {
                        sample.checked = checked;
                        sample.at = now;
                    }
                    if (sample.rate > 0 && total > checked)
                        job.eta_seconds = static_cast<uint64_t>(static_cast<double>(total - checked) / sample.rate) + 1;
                    else
                        job.eta_seconds.reset();
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
                    hold_torrent(hit->second);
                    // Kept, paused, until every extent is published, or the
                    // ingest would copy instead of adopting.
                    if (!publication_settled_locked(id, job)) {
                        return;
                    }
                    try {
                        const auto ingest_id = ingest_.submit_path(job.save_path, "torrent", job.id,
                                                                   job.name, std::nullopt, true, true);
                        job.ingest_job_id = ingest_id;
                        job.state = TorrentJobState::importing;
                        retire_torrent_locked(id, false);
                    } catch (const std::exception& e) {
                        job.state = TorrentJobState::failed;
                        job.error_code = "ingest_submit_failed";
                        job.error = "cannot submit completed torrent to ingest: " + std::string(e.what());
                    }
                }
            }();
            refresh_publication_locked(id, job);
            // Saved when the record changed; transfer counters alone at most
            // every progress_save_interval.
            const auto change = torrent_job_change(before, job);
            if (change != TorrentJobChange::none) job.updated_unix_ms = unix_ms();
            if (change == TorrentJobChange::record) changed = true;
            if (change == TorrentJobChange::progress) progress_moved = true;
        } catch (const std::exception& e) {
            faults.emplace_back(id, e.what());
        }
    }
    for (const auto& [id, what] : faults) isolate_fault_locked(id, what);
    const auto now = Clock::now();
    if (progress_moved && now - last_progress_save_ >= progress_save_interval) changed = true;
    if (changed) {
        save_state_locked();
        last_progress_save_ = now;
    }
}

} // namespace macha