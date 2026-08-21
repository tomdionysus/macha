// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#ifdef MACHA_HAVE_LIBTORRENT
#include <libtorrent/add_torrent_params.hpp>
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
#endif

namespace macha {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string read_secret(const std::optional<std::filesystem::path>& path) {
    if (!path) return {};
    std::ifstream in(*path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read torrent provider API key file: " + path->string());
    std::ostringstream out;
    out << in.rdbuf();
    return trim(out.str());
}

std::string url_decode(std::string_view value) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hi = nibble(value[i + 1]);
            const auto lo = nibble(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

bool safe_tracker_url(std::string_view value) {
    return value.starts_with("http://") || value.starts_with("https://") || value.starts_with("udp://");
}

std::string sanitize_text(std::string value, size_t limit = 1024) {
    std::erase_if(value, [](unsigned char c) { return c < 0x20 && c != '\t'; });
    if (value.size() > limit) value.resize(limit);
    return trim(std::move(value));
}

std::string xml_decode(std::string value) {
    struct Entity { const char* encoded; const char* decoded; };
    static constexpr Entity entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    for (const auto& entity : entities) {
        size_t pos = 0;
        while ((pos = value.find(entity.encoded, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(entity.encoded), entity.decoded);
            pos += std::strlen(entity.decoded);
        }
    }
    return value;
}

std::optional<std::string> xml_tag(std::string_view item, std::string_view tag) {
    const auto open = "<" + std::string(tag);
    auto start = item.find(open);
    if (start == std::string_view::npos) return {};
    start = item.find('>', start + open.size());
    if (start == std::string_view::npos) return {};
    ++start;
    const auto close = "</" + std::string(tag) + ">";
    const auto end = item.find(close, start);
    if (end == std::string_view::npos) return {};
    auto value = std::string(item.substr(start, end - start));
    if (value.starts_with("<![CDATA[") && value.ends_with("]]>") && value.size() >= 12)
        value = value.substr(9, value.size() - 12);
    return sanitize_text(xml_decode(std::move(value)), 4096);
}

std::optional<std::string> xml_attribute(std::string_view element, std::string_view name) {
    const auto needle = std::string(name) + "=";
    auto pos = element.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    if (pos >= element.size() || (element[pos] != '\'' && element[pos] != '"')) return {};
    const char quote = element[pos++];
    const auto end = element.find(quote, pos);
    if (end == std::string_view::npos) return {};
    return xml_decode(std::string(element.substr(pos, end - pos)));
}

std::optional<std::string> torznab_attr(std::string_view item, std::string_view wanted) {
    size_t pos = 0;
    while ((pos = item.find("<torznab:attr", pos)) != std::string_view::npos) {
        const auto end = item.find('>', pos);
        if (end == std::string_view::npos) break;
        const auto element = item.substr(pos, end - pos + 1);
        auto name = xml_attribute(element, "name");
        if (name && *name == wanted) return xml_attribute(element, "value");
        pos = end + 1;
    }
    return {};
}

std::optional<uint64_t> parse_u64(std::string_view value) {
    uint64_t out{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    auto [at, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || at != end) return {};
    return out;
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
    if (const auto* v = value.find("ingest_job_id"); v && !v->isNull()) job.ingest_job_id = v->asString();
    if (const auto* v = value.find("created_unix_ms")) job.created_unix_ms = v->asUInt64();
    if (const auto* v = value.find("updated_unix_ms")) job.updated_unix_ms = v->asUInt64();
    if (const auto* v = value.find("error")) job.error = v->asString();
    return job;
}

#ifdef MACHA_HAVE_LIBTORRENT
namespace lt = libtorrent;

lt::session_params make_session_params(const TorrentConfig& config) {
    lt::session_params params;
    auto& settings = params.settings;
    settings.set_int(lt::settings_pack::active_downloads, static_cast<int>(config.max_active));
    settings.set_int(lt::settings_pack::active_limit, static_cast<int>(config.max_active + 4));
    settings.set_int(lt::settings_pack::active_seeds, 0);
    settings.set_bool(lt::settings_pack::enable_dht, config.dht);
    settings.set_bool(lt::settings_pack::enable_lsd, config.lsd);
    if (config.max_download_rate)
        settings.set_int(lt::settings_pack::download_rate_limit,
                         static_cast<int>(std::min<uint64_t>(config.max_download_rate, INT_MAX)));
    if (config.max_upload_rate)
        settings.set_int(lt::settings_pack::upload_rate_limit,
                         static_cast<int>(std::min<uint64_t>(config.max_upload_rate, INT_MAX)));
    settings.set_str(lt::settings_pack::user_agent, "Macha/0.13 libtorrent/" + std::string(lt::version()));
    return params;
}

void harden_add_params(lt::add_torrent_params& atp, const TorrentConfig& config) {
    // A torrent may contain arbitrary HTTP web seeds and DHT bootstrap nodes.
    // Macha acquisition deliberately accepts peers/trackers only; never turn a
    // torrent metainfo file into a general-purpose server-side HTTP fetcher.
    atp.url_seeds.clear();
    atp.dht_nodes.clear();
    atp.flags |= lt::torrent_flags::override_web_seeds;
    std::erase_if(atp.trackers, [](const std::string& tracker) { return !safe_tracker_url(tracker); });
    atp.tracker_tiers.assign(atp.trackers.size(), 0);
    if (!config.dht) atp.flags |= lt::torrent_flags::disable_dht;
    if (!config.lsd) atp.flags |= lt::torrent_flags::disable_lsd;
    if (!config.pex) atp.flags |= lt::torrent_flags::disable_pex;
}
#endif

} // namespace

std::string torrent_job_state_name(TorrentJobState state) {
    switch (state) {
    case TorrentJobState::queued: return "queued";
    case TorrentJobState::metadata: return "metadata";
    case TorrentJobState::downloading: return "downloading";
    case TorrentJobState::verifying: return "verifying";
    case TorrentJobState::downloaded: return "downloaded";
    case TorrentJobState::importing: return "importing";
    case TorrentJobState::paused: return "paused";
    case TorrentJobState::blocked: return "blocked";
    case TorrentJobState::completed: return "completed";
    case TorrentJobState::cancelled: return "cancelled";
    case TorrentJobState::failed: return "failed";
    }
    return "failed";
}

std::optional<TorrentJobState> parse_torrent_job_state(std::string_view state) {
    if (state == "queued") return TorrentJobState::queued;
    if (state == "metadata") return TorrentJobState::metadata;
    if (state == "downloading") return TorrentJobState::downloading;
    if (state == "verifying") return TorrentJobState::verifying;
    if (state == "downloaded") return TorrentJobState::downloaded;
    if (state == "importing") return TorrentJobState::importing;
    if (state == "paused") return TorrentJobState::paused;
    if (state == "blocked") return TorrentJobState::blocked;
    if (state == "completed") return TorrentJobState::completed;
    if (state == "cancelled") return TorrentJobState::cancelled;
    if (state == "failed") return TorrentJobState::failed;
    return {};
}

std::optional<std::string> http_origin(std::string_view value) {
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos) return {};
    const auto scheme = value.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https") return {};
    const auto authority_start = scheme_end + 3;
    auto authority_end = value.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos) authority_end = value.size();
    if (authority_end == authority_start) return {};
    return std::string(value.substr(0, authority_end));
}

std::optional<std::string> provider_download_url(std::string_view base, std::string_view candidate) {
    const auto origin = http_origin(base);
    if (!origin) return {};
    std::string absolute;
    if (candidate.starts_with("/")) {
        absolute = *origin + std::string(candidate);
    } else {
        const auto candidate_origin = http_origin(candidate);
        if (!candidate_origin || *candidate_origin != *origin) return {};
        absolute = std::string(candidate);
    }
    return absolute;
}

bool safe_torrent_fetch_url(std::string_view value) {
    return http_origin(value).has_value();
}

std::optional<std::string> sanitize_magnet_uri(std::string_view uri) {
    if (!uri.starts_with("magnet:?")) return {};
    std::vector<std::string> xts;
    std::optional<std::string> dn;
    std::vector<std::string> trackers;
    auto query = uri.substr(8);
    size_t pos = 0;
    while (pos <= query.size()) {
        const auto amp = query.find('&', pos);
        const auto field = query.substr(pos, amp == std::string_view::npos ? query.size() - pos : amp - pos);
        const auto equal = field.find('=');
        if (equal != std::string_view::npos) {
            const auto key = field.substr(0, equal);
            auto value = url_decode(field.substr(equal + 1));
            if (key == "xt" && value.size() <= 256 &&
                (value.starts_with("urn:btih:") || value.starts_with("urn:btmh:"))) {
                xts.push_back(std::move(value));
            } else if (key == "dn" && !dn) {
                dn = sanitize_text(std::move(value), 512);
            } else if (key == "tr" && trackers.size() < 32 && value.size() <= 2048 && safe_tracker_url(value)) {
                trackers.push_back(std::move(value));
            }
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    if (xts.empty()) return {};
    std::string out = "magnet:?";
    bool first = true;
    auto add = [&](std::string_view key, std::string_view value) {
        if (!first) out += '&';
        first = false;
        out += key;
        out += '=';
        out += url_encode(value);
    };
    for (const auto& value : xts) add("xt", value);
    if (dn && !dn->empty()) add("dn", *dn);
    for (const auto& value : trackers) add("tr", value);
    return out;
}

TorznabSearchProvider::TorznabSearchProvider(TorrentSearchProviderConfig config,
                                             std::unique_ptr<HttpClient> http)
    : config_(std::move(config)), http_(std::move(http)), api_key_(read_secret(config_.api_key_file)) {
    if (!http_) http_ = std::make_unique<CurlHttpClient>();
}

std::vector<TorrentSearchResult> TorznabSearchProvider::search(std::string_view query) {
    if (query.empty()) return {};
    std::string url = config_.url;
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += "t=search&q=" + url_encode(query) + "&limit=" + std::to_string(config_.max_results);
    if (!api_key_.empty()) url += "&apikey=" + url_encode(api_key_);
    auto response = http_->get(url, {}, 4 * 1024 * 1024);
    if (response.status < 200 || response.status >= 300)
        throw std::runtime_error("Torznab returned HTTP " + std::to_string(response.status));
    std::string xml(reinterpret_cast<const char*>(response.body.data()), response.body.size());
    auto prefix = trim(xml.substr(0, std::min<size_t>(xml.size(), 256)));
    if (prefix.starts_with("<!DOCTYPE html") || prefix.starts_with("<html") || prefix.starts_with("<HTML"))
        throw std::runtime_error("Torznab provider returned HTML instead of XML");

    std::vector<TorrentSearchResult> out;
    size_t pos = 0;
    while (out.size() < config_.max_results && (pos = xml.find("<item", pos)) != std::string::npos) {
        const auto start = xml.find('>', pos);
        if (start == std::string::npos) break;
        const auto end = xml.find("</item>", start + 1);
        if (end == std::string::npos) break;
        const auto item = std::string_view(xml).substr(start + 1, end - start - 1);
        TorrentSearchResult result;
        result.provider = config_.name;
        if (auto title = xml_tag(item, "title")) result.title = sanitize_text(*title, 1024);
        if (auto published = xml_tag(item, "pubDate")) result.published = sanitize_text(*published, 256);
        if (auto size = xml_tag(item, "size")) result.size_bytes = parse_u64(*size);
        if (!result.size_bytes) {
            if (auto size = torznab_attr(item, "size")) result.size_bytes = parse_u64(*size);
        }
        if (auto seeds = torznab_attr(item, "seeders")) result.seeders = parse_u64(*seeds);
        if (auto leeches = torznab_attr(item, "peers")) result.leechers = parse_u64(*leeches);
        if (auto magnet = torznab_attr(item, "magneturl")) result.magnet_uri = sanitize_magnet_uri(*magnet);
        if (!result.magnet_uri) {
            if (auto link = xml_tag(item, "link")) {
                if (link->starts_with("magnet:?")) result.magnet_uri = sanitize_magnet_uri(*link);
                else if (auto safe = provider_download_url(config_.url, *link)) result.torrent_url = *safe;
            }
        }
        if (!result.torrent_url) {
            const auto enclosure = item.find("<enclosure");
            if (enclosure != std::string_view::npos) {
                const auto enclosure_end = item.find('>', enclosure);
                if (enclosure_end != std::string_view::npos) {
                    auto element = item.substr(enclosure, enclosure_end - enclosure + 1);
                    if (auto value = xml_attribute(element, "url"))
                        if (auto safe = provider_download_url(config_.url, *value)) result.torrent_url = *safe;
                }
            }
        }
        if (!result.title.empty() && (result.magnet_uri || result.torrent_url)) out.push_back(std::move(result));
        pos = end + 7;
    }
    return out;
}

TorrentSearchManager::TorrentSearchManager(const TorrentConfig& config) {
    for (const auto& provider : config.search_providers) {
        if (!provider.enabled) continue;
        if (provider.type == "torznab") providers_.push_back(std::make_unique<TorznabSearchProvider>(provider));
    }
}

TorrentSearchResponse TorrentSearchManager::search(std::string_view query) {
    TorrentSearchResponse response;
    for (auto& provider : providers_) {
        try {
            auto results = provider->search(query);
            response.results.insert(response.results.end(),
                                    std::make_move_iterator(results.begin()),
                                    std::make_move_iterator(results.end()));
        } catch (const std::exception& e) {
            response.errors[std::string(provider->name())] = e.what();
        }
    }
    std::stable_sort(response.results.begin(), response.results.end(), [](const auto& a, const auto& b) {
        return a.seeders.value_or(0) > b.seeders.value_or(0);
    });

    const auto expires = unix_ms() + 30ULL * 60 * 1000;
    std::lock_guard lock(mutex_);
    for (auto it = acquisitions_.begin(); it != acquisitions_.end();) {
        if (it->second.expires_unix_ms <= unix_ms()) it = acquisitions_.erase(it);
        else ++it;
    }
    for (auto& result : response.results) {
        auto uri = result.magnet_uri ? result.magnet_uri : result.torrent_url;
        if (!uri) continue;
        result.acquisition_ref = to_string(random_node_id());
        acquisitions_[result.acquisition_ref] = {*uri, expires};
    }
    return response;
}

std::optional<std::string> TorrentSearchManager::resolve(std::string_view acquisition_ref) {
    std::lock_guard lock(mutex_);
    auto it = acquisitions_.find(std::string(acquisition_ref));
    if (it == acquisitions_.end()) return {};
    if (it->second.expires_unix_ms <= unix_ms()) {
        acquisitions_.erase(it);
        return {};
    }
    return it->second.uri;
}

#ifdef MACHA_HAVE_LIBTORRENT
struct TorrentManager::Impl {
    libtorrent::session session;
    std::map<std::string, libtorrent::torrent_handle, std::less<>> handles;
    CurlHttpClient http;

    explicit Impl(const TorrentConfig& config) : session(make_session_params(config)) {}
};
#else
struct TorrentManager::Impl {};
#endif

TorrentManager::TorrentManager(IngestManager& ingest, TorrentConfig config,
                               const std::filesystem::path& state_path)
    : ingest_(ingest), config_(std::move(config)), state_file_(state_path / "torrent" / "jobs.json") {
    if (!config_.enabled) return;
    std::filesystem::create_directories(state_file_.parent_path());
#ifdef MACHA_HAVE_LIBTORRENT
    impl_ = std::make_unique<Impl>(config_);
#endif
    load_state();
}

TorrentManager::~TorrentManager() { stop(); }

bool TorrentManager::build_available() noexcept {
#ifdef MACHA_HAVE_LIBTORRENT
    return true;
#else
    return false;
#endif
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
    const auto text = Json(std::move(root)).dump();
    const auto temp = state_file_.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write torrent state");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
    }
    std::error_code ec;
    std::filesystem::permissions(temp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    ec.clear();
    std::filesystem::rename(temp, state_file_, ec);
    if (ec) {
        std::filesystem::remove(state_file_, ec);
        ec.clear();
        std::filesystem::rename(temp, state_file_, ec);
    }
    if (ec) throw std::runtime_error("cannot replace torrent state: " + ec.message());
}

void TorrentManager::start() {
    if (!config_.enabled || worker_.joinable()) return;
    restore_jobs();
    worker_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void TorrentManager::request_stop() {
    if (worker_.joinable()) worker_.request_stop();
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
}

void TorrentManager::restore_jobs() {
#ifdef MACHA_HAVE_LIBTORRENT
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
#endif
}

std::string TorrentManager::add_impl(std::string uri, bool allow_fetch) {
    if (!config_.enabled) throw std::runtime_error("torrent support is disabled");
#ifndef MACHA_HAVE_LIBTORRENT
    (void)uri;
    (void)allow_fetch;
    throw std::runtime_error("torrent support was not built");
#else
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
        lt::error_code ec;
        atp = lt::load_torrent_buffer(lt::span<char const>(buffer.data(), buffer.size()), ec, {});
        if (ec) throw std::runtime_error("invalid .torrent file: " + ec.message());
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
        save_state_locked();
    }
    return job.id;
#endif
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
#ifdef MACHA_HAVE_LIBTORRENT
    if (!it->second.ingest_job_id)
        if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) h->second.pause();
#endif
    it->second.state = TorrentJobState::paused;
    it->second.download_rate = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
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
#ifdef MACHA_HAVE_LIBTORRENT
    else if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) h->second.resume();
#endif
    it->second.state = it->second.ingest_job_id ? TorrentJobState::importing : TorrentJobState::queued;
    it->second.error.clear();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    return true;
}

bool TorrentManager::cancel(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == TorrentJobState::completed || it->second.state == TorrentJobState::cancelled) return false;
    if (it->second.ingest_job_id) (void)ingest_.cancel(*it->second.ingest_job_id);
#ifdef MACHA_HAVE_LIBTORRENT
    if (auto h = impl_->handles.find(it->first); h != impl_->handles.end()) {
        impl_->session.remove_torrent(h->second, lt::session::delete_files | lt::session::delete_partfile);
        impl_->handles.erase(h);
    }
#endif
    ingest_.staging().release(it->first);
    std::error_code ec;
    std::filesystem::remove_all(it->second.save_path, ec);
    it->second.state = TorrentJobState::cancelled;
    it->second.download_rate = it->second.upload_rate = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = unix_ms();
    save_state_locked();
    return true;
}

void TorrentManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        update_jobs();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void TorrentManager::update_jobs() {
#ifdef MACHA_HAVE_LIBTORRENT
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (auto& [id, job] : jobs_) {
        if (job.state == TorrentJobState::cancelled || job.state == TorrentJobState::completed ||
            job.state == TorrentJobState::failed)
            continue;

        if (job.ingest_job_id) {
            auto ingest_job = ingest_.job(*job.ingest_job_id);
            if (!ingest_job) {
                job.state = TorrentJobState::failed;
                job.error = "associated ingest job disappeared";
            } else if (ingest_job->state == IngestJobState::completed) {
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
                else
                    job.state = TorrentJobState::importing;
                job.bytes_total = ingest_job->bytes_total;
                job.bytes_completed = ingest_job->bytes_completed;
                job.download_rate = ingest_job->rate_bytes_per_second;
                job.eta_seconds = ingest_job->eta_seconds;
                job.error = ingest_job->error;
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

        switch (status.state) {
        case lt::torrent_status::checking_files:
        case lt::torrent_status::checking_resume_data:
            job.state = TorrentJobState::verifying;
            break;
        case lt::torrent_status::downloading_metadata:
            job.state = TorrentJobState::metadata;
            break;
        case lt::torrent_status::downloading:
            job.state = TorrentJobState::downloading;
            break;
        case lt::torrent_status::finished:
        case lt::torrent_status::seeding:
            job.state = TorrentJobState::downloaded;
            break;
        }

        if (job.state == TorrentJobState::downloaded) {
            hit->second.pause();
            try {
                const auto ingest_id = ingest_.submit_path(job.save_path, "torrent", job.id,
                                                           job.name, true, true);
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
#endif
}

} // namespace macha
