// SPDX-License-Identifier: GPL-3.0-or-later
// The half of BitTorrent acquisition that stays in macha_core: the job value
// types, the API/wire JSON shapes, the URI sanitisers and the Torznab search
// client, none of which touch libtorrent. The download engine itself
// (TorrentManager) lives in the libmacha-torrent plugin -- see
// TODO/2026-09-05-subsystem-plugin-isolation-plan.md and torrent_manager.cpp.
#include "torrent.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "crypto.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>

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
} // namespace

std::string torrent_job_state_name(TorrentJobState state) {
    switch (state) {
    case TorrentJobState::queued: return "queued";
    case TorrentJobState::metadata: return "metadata";
    case TorrentJobState::downloading: return "downloading";
    case TorrentJobState::verifying: return "verifying";
    case TorrentJobState::downloaded: return "downloaded";
    case TorrentJobState::importing: return "importing";
    case TorrentJobState::cataloguing: return "cataloguing";
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
    if (state == "cataloguing") return TorrentJobState::cataloguing;
    if (state == "paused") return TorrentJobState::paused;
    if (state == "blocked") return TorrentJobState::blocked;
    if (state == "completed") return TorrentJobState::completed;
    if (state == "cancelled") return TorrentJobState::cancelled;
    if (state == "failed") return TorrentJobState::failed;
    return {};
}

Json torrent_job_api_json(const TorrentJob& job) {
    Json::Object out;
    out["id"] = job.id;
    out["name"] = job.name;
    out["info_hash"] = job.info_hash.empty() ? Json(nullptr) : Json(job.info_hash);
    out["state"] = torrent_job_state_name(job.state);
    out["bytes_total"] = job.bytes_total;
    out["bytes_completed"] = job.bytes_completed;
    out["download_rate"] = job.download_rate;
    out["upload_rate"] = job.upload_rate;
    out["uploaded_total"] = job.uploaded_total;
    out["peers"] = static_cast<uint64_t>(job.peers);
    out["seeds"] = static_cast<uint64_t>(job.seeds);
    Json::Object catalogue;
    catalogue["total"] = static_cast<uint64_t>(job.catalogue_total);
    catalogue["pending"] = static_cast<uint64_t>(job.catalogue_pending);
    catalogue["catalogued"] = static_cast<uint64_t>(job.catalogue_catalogued);
    catalogue["no_match"] = static_cast<uint64_t>(job.catalogue_no_match);
    catalogue["failed"] = static_cast<uint64_t>(job.catalogue_failed);
    if (job.catalogue_pending)
        catalogue["state"] = "processing";
    else if (job.catalogue_total && (job.catalogue_failed || job.catalogue_no_match))
        catalogue["state"] = "completed_with_issues";
    else if (job.catalogue_total)
        catalogue["state"] = "completed";
    else
        catalogue["state"] = "waiting";
    out["catalogue"] = std::move(catalogue);
    out["eta_seconds"] = optional_u64(job.eta_seconds);
    out["progress"] = job.bytes_total
                          ? Json(std::min(1.0, static_cast<double>(job.bytes_completed) /
                                                   static_cast<double>(job.bytes_total)))
                          : Json(nullptr);
    out["ingest_job_id"] = job.ingest_job_id ? Json(*job.ingest_job_id) : Json(nullptr);
    out["created_unix_ms"] = job.created_unix_ms;
    out["updated_unix_ms"] = job.updated_unix_ms;
    out["error_code"] = job.error_code.empty() ? Json(nullptr) : Json(job.error_code);
    out["error"] = job.error.empty() ? Json(nullptr) : Json(job.error);
    return Json(std::move(out));
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
        while (acquisitions_.size() >= max_acquisitions_) {
            // References are opaque and equivalent except for expiry. Retire
            // the one with the least remaining lifetime before admitting a new
            // owner so repeated searches cannot grow the process indefinitely.
            auto victim = std::min_element(
                acquisitions_.begin(), acquisitions_.end(), [](const auto& a, const auto& b) {
                    return a.second.expires_unix_ms < b.second.expires_unix_ms;
                });
            acquisitions_.erase(victim);
        }
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

bool advertise_is_ip_literal(std::string_view advertise) {
    // libtorrent's listen_interfaces takes an IP literal or a device name --
    // never a hostname. A name reaches its device enumeration, matches no
    // device, and binds nothing at all, silently. Only an address can be used
    // here, so only an address is accepted.
    if (advertise.find(':') != std::string_view::npos) {
        in6_addr v6{};
        return inet_pton(AF_INET6, std::string(advertise).c_str(), &v6) == 1;
    }
    in_addr v4{};
    return inet_pton(AF_INET, std::string(advertise).c_str(), &v4) == 1;
}

std::string torrent_listen_interfaces(const TorrentConfig& config, std::string_view advertise) {
    if (!config.listen_interfaces.empty())
        return config.listen_interfaces;
    const auto port = ":" + std::to_string(config.listen_port);
    const auto wildcard = "0.0.0.0" + port + ",[::]" + port;
    // No usable advertised address: fall back to libtorrent's own default and
    // accept whatever its device enumeration produces.
    if (advertise.empty() || advertise == "0.0.0.0" || advertise == "::")
        return wildcard;
    // An advertised address that is not an IP literal cannot be bound. This is
    // the ordinary case once a node advertises a DNS name -- and worse, that
    // name usually resolves to a public address the node does not hold at all,
    // because it is behind NAT. Binding every interface is the only honest
    // answer: peer traffic then leaves by whichever route the kernel picks,
    // exactly as it did before 0.37.2 tried to be more specific.
    //
    // Observed 2026-09-12: three nodes moved to public DNS advertise values
    // and every one of them bound nothing on 6881, leaving torrents in
    // dl-metadata for ever with no error anywhere.
    if (!advertise_is_ip_literal(advertise)) {
        Log::info("torrent listen: advertised address '" + std::string(advertise) +
                  "' is not an IP literal, binding all interfaces instead" + port +
                  " (set torrent.listen_interfaces to choose a device)");
        return wildcard;
    }
    if (advertise.find(':') != std::string_view::npos) // literal IPv6
        return "[" + std::string(advertise) + "]" + port;
    return std::string(advertise) + port;
}

} // namespace macha

