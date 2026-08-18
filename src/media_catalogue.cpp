// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_catalogue.hpp"
#include "diagnostics.hpp"

#include "crypto.hpp"
#include "log.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace macha {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(std::string value) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && ws(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && ws(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string clean_title(std::string value) {
    for (auto& c : value) {
        if (c == '.' || c == '_' || c == '-') c = ' ';
    }
    std::string out;
    bool space = true;
    for (unsigned char c : value) {
        if (std::isspace(c)) {
            if (!space) out.push_back(' ');
            space = true;
        } else {
            out.push_back(static_cast<char>(c));
            space = false;
        }
    }
    return trim(out);
}

std::string normalized(std::string_view value) {
    std::string out;
    bool space = true;
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            space = false;
        } else if (!space) {
            out.push_back(' ');
            space = true;
        }
    }
    return trim(out);
}

std::string comparable_title(std::string_view value) {
    // Provider titles commonly spell sequel numbers differently from release
    // filenames ("2" vs "II", "12" vs "Twelve"). Canonicalise isolated
    // number tokens for matching only; preserve the parsed/display title.
    static constexpr std::pair<std::string_view, std::string_view> aliases[] = {
        {"zero", "0"}, {"one", "1"}, {"two", "2"}, {"three", "3"},
        {"four", "4"}, {"five", "5"}, {"six", "6"}, {"seven", "7"},
        {"eight", "8"}, {"nine", "9"}, {"ten", "10"}, {"eleven", "11"},
        {"twelve", "12"}, {"thirteen", "13"}, {"fourteen", "14"},
        {"fifteen", "15"}, {"sixteen", "16"}, {"seventeen", "17"},
        {"eighteen", "18"}, {"nineteen", "19"}, {"twenty", "20"},
        {"ii", "2"}, {"iii", "3"}, {"iv", "4"}, {"v", "5"},
        {"vi", "6"}, {"vii", "7"}, {"viii", "8"}, {"ix", "9"},
        {"x", "10"}, {"xi", "11"}, {"xii", "12"}, {"xiii", "13"},
        {"xiv", "14"}, {"xv", "15"}, {"xvi", "16"}, {"xvii", "17"},
        {"xviii", "18"}, {"xix", "19"}, {"xx", "20"},
    };

    std::istringstream in(normalized(value));
    std::string out;
    std::string token;
    while (in >> token) {
        std::string_view canonical = token;
        for (const auto& [from, to] : aliases) {
            if (token == from) {
                canonical = to;
                break;
            }
        }
        if (!out.empty()) out.push_back(' ');
        out.append(canonical);
    }
    return out;
}

std::string extension(std::string_view path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return lower(std::string(path.substr(dot)));
}

std::string stem(std::string_view path) {
    auto slash = path.find_last_of('/');
    auto start = slash == std::string_view::npos ? 0 : slash + 1;
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot < start) dot = path.size();
    return std::string(path.substr(start, dot - start));
}

std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') ++pos;
        if (pos == path.size()) break;
        auto end = path.find('/', pos);
        if (end == std::string_view::npos) end = path.size();
        out.emplace_back(path.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

bool video_extension(std::string_view ext) {
    static const std::set<std::string, std::less<>> exts{
        ".mkv", ".mp4", ".m4v", ".avi", ".mov", ".wmv", ".mpg", ".mpeg", ".ts", ".m2ts", ".webm"};
    return exts.contains(ext);
}

bool audio_extension(std::string_view ext) {
    static const std::set<std::string, std::less<>> exts{
        ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".wav", ".aiff", ".alac", ".wma"};
    return exts.contains(ext);
}

std::optional<int32_t> year_from(std::string_view text) {
    static const std::regex re(R"((?:19|20)[0-9]{2})");
    std::string owned(text);
    std::optional<int32_t> result;
    for (std::sregex_iterator it(owned.begin(), owned.end(), re), end; it != end; ++it) {
        const auto pos = static_cast<size_t>((*it).position());
        const auto len = static_cast<size_t>((*it).length());
        const bool numeric_left = pos && std::isdigit(static_cast<unsigned char>(owned[pos - 1]));
        const bool numeric_right = pos + len < owned.size() &&
                                   std::isdigit(static_cast<unsigned char>(owned[pos + len]));
        if (numeric_left || numeric_right) continue;
        // Dimensions such as 1920x816 are not release years. This is the main
        // reason leading-year names such as 1994.Pulp.Fiction.1920x816 were
        // previously parsed as a 1920 release.
        if (pos + len < owned.size() && (owned[pos + len] == 'x' || owned[pos + len] == 'X') &&
            pos + len + 1 < owned.size() &&
            std::isdigit(static_cast<unsigned char>(owned[pos + len + 1])))
            continue;
        result = std::stoi((*it).str());
    }
    return result;
}

std::string strip_release_noise(std::string value) {
    static const std::regex technical(
        R"((?:^|[ ._\-(\[]+)(?:[0-9]{3,4}x[0-9]{3,4}|2160p|1440p|1080p|720p|576p|480p|360p|uhd|bluray|blu-ray|bdrip|webrip|web-dl|webdl|hdtv|dvdrip|dvd|remux|amzn|nf|x264|x265|h264|h265|h\.264|h\.265|hevc|avc|av1|vp9|aac(?:[0-9.]*)?|eac3|ac3|ddp(?:[0-9.]*)?|dd(?:[0-9.]*)?|dts(?:-hd)?(?:[ .]ma)?|flac|opus|multi-subs|multisubs)\b.*$)",
        std::regex::icase);
    std::smatch match;
    if (std::regex_search(value, match, technical))
        value.resize(static_cast<size_t>(match.position()));
    static const std::regex site_tag(R"([ ._-]*\[[^\]]+\]\s*$)", std::regex::icase);
    value = std::regex_replace(value, site_tag, "");
    return trim(value);
}

std::string remove_year_token(std::string value, int32_t year) {
    const auto text = std::to_string(year);
    auto pos = value.find(text);
    if (pos == std::string::npos) return value;
    size_t begin = pos;
    size_t end = pos + text.size();
    if (begin && value[begin - 1] == '(' && end < value.size() && value[end] == ')') {
        --begin;
        ++end;
    }
    value.replace(begin, end - begin, " ");
    return value;
}

std::string clean_series_name(std::string value) {
    value = strip_release_noise(std::move(value));
    static const std::regex season_suffix(
        R"((?:[ ._-]+)(?:s[0-9]{1,2}(?:[ ._-]*-[ ._-]*s?[0-9]{1,2})?|season[ ._-]*[0-9]{1,2})\s*$)",
        std::regex::icase);
    value = std::regex_replace(value, season_suffix, "");
    if (auto year = year_from(value)) value = remove_year_token(std::move(value), *year);
    return clean_title(value);
}

std::string clean_episode_title(std::string value) {
    value = strip_release_noise(std::move(value));
    return clean_title(value);
}

std::string movie_title_before_year(std::string value, const std::optional<int32_t>& year) {
    if (year) {
        auto pos = value.find(std::to_string(*year));
        if (pos != std::string::npos) {
            if (pos == 0) value.erase(0, 4);
            else value.resize(pos);
        }
    }
    value = strip_release_noise(std::move(value));

    // Zero-padded collection ordinals are common release prefixes, while a
    // genuine title such as "12 Monkeys" must remain intact.
    static const std::regex ordinal(R"(^\s*0[0-9]{1,2}[ ._-]+)");
    value = std::regex_replace(value, ordinal, "");

    // A small but useful release-name convention: numbered collection entries
    // sometimes append a principal actor after " - ". Limit this heuristic to
    // titles whose pre-credit portion itself ends in a digit so ordinary
    // hyphenated titles ("Star Wars - A New Hope") are not damaged.
    static const std::regex numbered_credit(
        R"(^(.+[0-9])\s+-\s+[A-Za-z][A-Za-z' .-]*$)", std::regex::icase);
    std::smatch match;
    if (std::regex_match(value, match, numbered_credit)) value = match[1].str();
    return clean_title(value);
}

std::optional<int32_t> json_i32(const Json* value) {
    if (!value || !value->isNumber()) return {};
    auto n = value->asInt64();
    if (n < INT32_MIN || n > INT32_MAX) return {};
    return static_cast<int32_t>(n);
}

std::string json_string(const Json* value) {
    return value && value->isString() ? value->asString() : std::string{};
}

std::optional<int32_t> json_year(const Json* value) {
    auto date = json_string(value);
    if (date.size() < 4) return {};
    int result = 0;
    auto [end, error] = std::from_chars(date.data(), date.data() + 4, result);
    if (error != std::errc{} || end != date.data() + 4) return {};
    return result;
}

std::string percent_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string lucene_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string query_url(std::string base,
                      const std::vector<std::pair<std::string, std::string>>& query) {
    bool first = base.find('?') == std::string::npos;
    for (const auto& [key, value] : query) {
        base.push_back(first ? '?' : '&');
        first = false;
        base += percent_encode(key);
        base.push_back('=');
        base += percent_encode(value);
    }
    return base;
}

std::string read_secret(const std::optional<std::filesystem::path>& path) {
    if (!path) return {};
    std::ifstream in(*path);
    if (!in) throw std::runtime_error("cannot read provider token file: " + path->string());
    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return trim(value);
}

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* opaque) {
    auto* pair = static_cast<std::pair<Bytes*, size_t>*>(opaque);
    if (nmemb && size > std::numeric_limits<size_t>::max() / nmemb) return 0;
    const auto bytes = size * nmemb;
    if (bytes > pair->second || pair->first->size() > pair->second - bytes) return 0;
    pair->first->insert(pair->first->end(), reinterpret_cast<uint8_t*>(ptr),
                        reinterpret_cast<uint8_t*>(ptr) + bytes);
    return bytes;
}

std::string item_id(std::string_view provider, std::string_view kind, std::string_view id) {
    return std::string(provider) + ":" + std::string(kind) + ":" + std::string(id);
}

void scanner_marker(CatalogueItem& item) { item.external_ids["macha_scanner"] = "1"; }

void add_art(std::vector<RemoteArtwork>& art, const std::string& item, std::string role,
             std::string url) {
    if (!url.empty()) art.push_back({item, std::move(role), std::move(url)});
}

const Json* best_result(const Json& root, std::string_view title, std::string_view title_key,
                        const std::optional<int32_t>& year, std::string_view date_key) {
    auto results = root.find("results");
    if (!results || !results->isArray() || results->asArray().empty()) return nullptr;
    const Json* best = &results->asArray().front();
    int best_score = -1;
    const auto wanted = comparable_title(title);
    for (const auto& candidate : results->asArray()) {
        if (!candidate.isObject()) continue;
        int score = 0;
        auto name = comparable_title(json_string(candidate.find(title_key)));
        if (name == wanted) score += 100;
        else if (name.find(wanted) != std::string::npos || wanted.find(name) != std::string::npos) score += 40;
        if (year) {
            auto found_year = json_year(candidate.find(date_key));
            if (found_year && *found_year == *year) score += 30;
        }
        if (score > best_score) { best_score = score; best = &candidate; }
    }
    const int minimum_score = year ? 70 : 80;
    return best_score >= minimum_score ? best : nullptr;
}

std::string artist_credit_name(const Json* credit) {
    if (!credit || !credit->isArray()) return {};
    std::string out;
    for (const auto& entry : credit->asArray()) {
        if (!entry.isObject()) continue;
        auto name = json_string(entry.find("name"));
        if (name.empty()) {
            if (auto artist = entry.find("artist"); artist && artist->isObject())
                name = json_string(artist->find("name"));
        }
        out += name;
        out += json_string(entry.find("joinphrase"));
    }
    return out;
}

bool has_art_role(const CatalogueItem* item, std::string_view role) {
    if (!item) return false;
    return std::any_of(item->artwork.begin(), item->artwork.end(),
                       [&](const auto& art) { return art.role == role; });
}

} // namespace

std::optional<MediaProbe> probe_media_path(std::string_view path, const FsEntry& entry) {
    if (entry.type != EntryType::file || entry.size == 0) return {};
    auto ext = extension(path);
    auto parts = components(path);
    if (parts.empty()) return {};

    MediaProbe probe;
    probe.path = normalize_path(std::string(path));
    probe.media_id = file_media_id(entry);
    auto file_stem = stem(path);

    if (audio_extension(ext)) {
        probe.kind = MediaProbeKind::track;
        static const std::regex track_re(R"(^\s*(?:(\d{1,2})[-.]\s*)?(\d{1,3})\s*[-_. ]+(.+)$)",
                                         std::regex::icase);
        std::smatch m;
        if (std::regex_match(file_stem, m, track_re)) {
            if (m[1].matched) probe.disc = std::stoi(m[1].str());
            probe.track = std::stoi(m[2].str());
            probe.title = clean_title(m[3].str());
        } else probe.title = clean_title(file_stem);
        size_t album_index = parts.size() >= 2 ? parts.size() - 2 : 0;
        if (parts.size() >= 4) {
            static const std::regex disc_dir(R"(^(?:cd|disc|disk)\s*([0-9]{1,2})$)",
                                             std::regex::icase);
            std::smatch disc_match;
            auto parent = clean_title(parts[parts.size() - 2]);
            if (std::regex_match(parent, disc_match, disc_dir)) {
                if (!probe.disc) probe.disc = std::stoi(disc_match[1].str());
                album_index = parts.size() - 3;
            }
        }
        if (album_index < parts.size()) probe.album = clean_title(parts[album_index]);
        if (album_index > 0) probe.artist = clean_title(parts[album_index - 1]);
        if (probe.artist.empty() || probe.album.empty() || probe.title.empty()) return {};
        return probe;
    }

    if (!video_extension(ext)) return {};

    static const std::regex se_re(R"((.*?)(?:[ ._-]+|^)s(\d{1,2})e(\d{1,3})(?:[ ._-]+(.*))?$)",
                                  std::regex::icase);
    static const std::regex x_re(R"((.*?)(?:[ ._-]+|^)(\d{1,2})x(\d{1,3})(?:[ ._-]+(.*))?$)",
                                 std::regex::icase);
    std::smatch m;
    std::string owned = file_stem;
    if (std::regex_match(owned, m, se_re) || std::regex_match(owned, m, x_re)) {
        probe.kind = MediaProbeKind::episode;
        probe.season = std::stoi(m[2].str());
        probe.episode = std::stoi(m[3].str());
        probe.title = m[4].matched ? clean_episode_title(m[4].str()) : std::string{};
        std::string prefix = clean_series_name(m[1].str());
        if (parts.size() >= 3) {
            static const std::regex season_dir(R"(^season\s*[0-9]{1,2}$)", std::regex::icase);
            auto parent = clean_title(parts[parts.size() - 2]);
            if (std::regex_match(parent, season_dir)) probe.series = clean_series_name(parts[parts.size() - 3]);
        }
        if (probe.series.empty()) probe.series = prefix;
        if (probe.series.empty() && parts.size() >= 2) probe.series = clean_series_name(parts[parts.size() - 2]);
        // Prefer the raw filename/folder year before clean_series_name removes
        // it; provider lookup benefits from a release year when one is present.
        probe.year = year_from(m[1].str());
        if (!probe.year && parts.size() >= 2) probe.year = year_from(parts[parts.size() - 2]);
        if (probe.series.empty()) return {};
        return probe;
    }

    probe.kind = MediaProbeKind::movie;
    probe.year = year_from(file_stem);
    probe.title = movie_title_before_year(file_stem, probe.year);
    if (probe.title.empty()) return {};
    return probe;
}

CurlHttpClient::CurlHttpClient() {
    static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
    if (initialized != CURLE_OK) throw std::runtime_error("curl_global_init failed");
}
CurlHttpClient::~CurlHttpClient() = default;

RemoteHttpResponse CurlHttpClient::get(std::string_view url, const std::vector<std::string>& headers,
                                 size_t maximum_bytes) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    RemoteHttpResponse out;
    std::pair<Bytes*, size_t> sink{&out.body, maximum_bytes};
    const auto owned_url = std::string(url);
    curl_easy_setopt(curl.get(), CURLOPT_URL, owned_url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Macha/0.9.4 (https://github.com/tomdionysus/macha)");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
    struct curl_slist* raw_headers = nullptr;
    for (const auto& header : headers) raw_headers = curl_slist_append(raw_headers, header.c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_guard(raw_headers, curl_slist_free_all);
    if (raw_headers) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, raw_headers);
    auto rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) throw std::runtime_error(std::string("HTTP GET failed: ") + curl_easy_strerror(rc));
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &out.status);
    char* content_type = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &content_type);
    if (content_type) out.content_type = content_type;
    return out;
}

TmdbProvider::TmdbProvider(HttpClient& http, CatalogueTmdbConfig config)
    : http_(http), config_(std::move(config)), token_(read_secret(config_.token_file)) {}

bool TmdbProvider::supports(MediaProbeKind kind) const {
    return config_.enabled && !token_.empty() && kind != MediaProbeKind::track;
}

Json TmdbProvider::api(std::string_view path,
                       const std::vector<std::pair<std::string, std::string>>& query) {
    auto response = http_.get(query_url("https://api.themoviedb.org/3" + std::string(path), query),
                              {"Authorization: Bearer " + token_, "Accept: application/json"});
    if (response.status != 200)
        throw std::runtime_error("TMDB returned HTTP " + std::to_string(response.status));
    return Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
}

std::string TmdbProvider::image_url(std::string_view path) const {
    if (path.empty()) return {};
    return "https://image.tmdb.org/t/p/" + config_.image_size + std::string(path);
}

std::optional<Json> TmdbProvider::find_show(const MediaProbe& probe) {
    const auto key = normalized(probe.series) + "|" + (probe.year ? std::to_string(*probe.year) : "");
    if (auto it = show_cache_.find(key); it != show_cache_.end()) return it->second;
    std::vector<std::pair<std::string, std::string>> q{{"query", probe.series}, {"language", config_.language}};
    if (probe.year) q.emplace_back("first_air_date_year", std::to_string(*probe.year));
    auto root = api("/search/tv", q);
    auto result = best_result(root, probe.series, "name", probe.year, "first_air_date");
    if (!result) return {};
    show_cache_[key] = *result;
    return *result;
}

std::optional<ProviderMatch> TmdbProvider::lookup(const MediaProbe& probe) {
    if (!supports(probe.kind)) return {};
    ProviderMatch match;
    if (probe.kind == MediaProbeKind::movie) {
        std::vector<std::pair<std::string, std::string>> q{{"query", probe.title}, {"language", config_.language}};
        if (probe.year) q.emplace_back("primary_release_year", std::to_string(*probe.year));
        auto search = api("/search/movie", q);
        auto found = best_result(search, probe.title, "title", probe.year, "release_date");
        if (!found) return {};
        auto id_value = json_i32(found->find("id"));
        if (!id_value) return {};
        const auto tmdb_id = std::to_string(*id_value);
        auto detail = api("/movie/" + tmdb_id, {{"language", config_.language}});
        CatalogueItem movie;
        movie.id = item_id("tmdb", "movie", tmdb_id);
        movie.kind = CatalogueKind::movie;
        movie.title = json_string(detail.find("title"));
        if (movie.title.empty()) movie.title = probe.title;
        movie.sort_title = movie.title;
        movie.synopsis = json_string(detail.find("overview"));
        movie.year = json_year(detail.find("release_date"));
        movie.external_ids["tmdb"] = tmdb_id;
        if (auto collection = detail.find("belongs_to_collection"); collection && collection->isObject()) {
            if (auto cid = json_i32(collection->find("id"))) movie.external_ids["tmdb_collection"] = std::to_string(*cid);
        }
        movie.media_ids = {probe.media_id};
        scanner_marker(movie);
        match.items.push_back(movie);
        add_art(match.artwork, movie.id, "poster", image_url(json_string(detail.find("poster_path"))));
        add_art(match.artwork, movie.id, "backdrop", image_url(json_string(detail.find("backdrop_path"))));
        return match;
    }

    auto show_json = find_show(probe);
    if (!show_json || !probe.season || !probe.episode) return {};
    auto sid = json_i32(show_json->find("id"));
    if (!sid) return {};
    const auto series_id = std::to_string(*sid);
    const auto season_key = series_id + "|" + std::to_string(*probe.season);
    Json season_json;
    if (auto it = season_cache_.find(season_key); it != season_cache_.end()) season_json = it->second;
    else {
        season_json = api("/tv/" + series_id + "/season/" + std::to_string(*probe.season),
                          {{"language", config_.language}});
        season_cache_[season_key] = season_json;
    }
    const Json* episode_json = nullptr;
    if (auto episodes = season_json.find("episodes"); episodes && episodes->isArray()) {
        for (const auto& episode : episodes->asArray()) {
            auto n = json_i32(episode.find("episode_number"));
            if (n && *n == *probe.episode) { episode_json = &episode; break; }
        }
    }
    if (!episode_json) return {};

    CatalogueItem show;
    show.id = item_id("tmdb", "tv", series_id);
    show.kind = CatalogueKind::show;
    show.title = json_string(show_json->find("name"));
    if (show.title.empty()) show.title = probe.series;
    show.sort_title = show.title;
    show.synopsis = json_string(show_json->find("overview"));
    show.year = json_year(show_json->find("first_air_date"));
    show.external_ids["tmdb"] = series_id;
    scanner_marker(show);

    CatalogueItem season;
    season.id = item_id("tmdb", "season", series_id + ":" + std::to_string(*probe.season));
    season.kind = CatalogueKind::season;
    season.title = json_string(season_json.find("name"));
    if (season.title.empty()) season.title = "Season " + std::to_string(*probe.season);
    season.sort_title = season.title;
    season.synopsis = json_string(season_json.find("overview"));
    season.parent_id = show.id;
    season.season_number = probe.season;
    season.external_ids["tmdb"] = json_i32(season_json.find("id")) ? std::to_string(*json_i32(season_json.find("id"))) : season.id;
    scanner_marker(season);

    CatalogueItem episode;
    auto eid = json_i32(episode_json->find("id"));
    episode.id = item_id("tmdb", "episode", eid ? std::to_string(*eid) : series_id + ":" + std::to_string(*probe.season) + ":" + std::to_string(*probe.episode));
    episode.kind = CatalogueKind::episode;
    episode.title = json_string(episode_json->find("name"));
    if (episode.title.empty()) episode.title = probe.title.empty() ? "Episode " + std::to_string(*probe.episode) : probe.title;
    episode.sort_title = episode.title;
    episode.synopsis = json_string(episode_json->find("overview"));
    episode.parent_id = season.id;
    episode.season_number = probe.season;
    episode.episode_number = probe.episode;
    if (eid) episode.external_ids["tmdb"] = std::to_string(*eid);
    episode.media_ids = {probe.media_id};
    scanner_marker(episode);

    match.items = {show, season, episode};
    add_art(match.artwork, show.id, "poster", image_url(json_string(show_json->find("poster_path"))));
    add_art(match.artwork, show.id, "backdrop", image_url(json_string(show_json->find("backdrop_path"))));
    add_art(match.artwork, season.id, "poster", image_url(json_string(season_json.find("poster_path"))));
    add_art(match.artwork, episode.id, "still", image_url(json_string(episode_json->find("still_path"))));
    return match;
}

MusicBrainzProvider::MusicBrainzProvider(HttpClient& http, CatalogueMusicBrainzConfig config)
    : http_(http), config_(std::move(config)) {}

bool MusicBrainzProvider::supports(MediaProbeKind kind) const {
    return config_.enabled && kind == MediaProbeKind::track;
}

Json MusicBrainzProvider::api(std::string_view path,
                              const std::vector<std::pair<std::string, std::string>>& query) {
    if (last_request_ != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::steady_clock::now() - last_request_;
        if (elapsed < std::chrono::seconds(1)) std::this_thread::sleep_for(std::chrono::seconds(1) - elapsed);
    }
    auto ua = "Macha/0.9.4 (" + config_.contact + ")";
    auto q = query;
    q.emplace_back("fmt", "json");
    auto response = http_.get(query_url("https://musicbrainz.org/ws/2" + std::string(path), q),
                              {"Accept: application/json", "User-Agent: " + ua});
    last_request_ = std::chrono::steady_clock::now();
    if (response.status != 200)
        throw std::runtime_error("MusicBrainz returned HTTP " + std::to_string(response.status));
    return Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
}

std::optional<Json> MusicBrainzProvider::find_release(const MediaProbe& probe) {
    auto key = normalized(probe.artist) + "|" + normalized(probe.album);
    if (auto it = release_cache_.find(key); it != release_cache_.end()) return it->second;
    auto query = "release:\"" + lucene_quote(probe.album) + "\" AND artist:\"" +
                 lucene_quote(probe.artist) + "\"";
    auto search = api("/release", {{"query", query}, {"limit", "10"}});
    auto releases = search.find("releases");
    if (!releases || !releases->isArray() || releases->asArray().empty()) return {};
    const Json* best = &releases->asArray().front();
    int best_score = -1;
    for (const auto& candidate : releases->asArray()) {
        int score = 0;
        if (normalized(json_string(candidate.find("title"))) == normalized(probe.album)) score += 100;
        if (normalized(artist_credit_name(candidate.find("artist-credit"))) == normalized(probe.artist)) score += 80;
        if (auto s = json_i32(candidate.find("score"))) score += *s / 10;
        if (score > best_score) { best_score = score; best = &candidate; }
    }
    if (best_score < 100) return {};
    auto release_id = json_string(best->find("id"));
    if (release_id.empty()) return {};
    auto detail = api("/release/" + release_id, {{"inc", "recordings+artist-credits+release-groups"}});
    release_cache_[key] = detail;
    return detail;
}

std::optional<std::string> MusicBrainzProvider::cover_url(std::string_view release_id) {
    const auto key = std::string(release_id);
    if (auto it = cover_cache_.find(key); it != cover_cache_.end()) return it->second;

    std::optional<std::string> result;
    auto response = http_.get("https://coverartarchive.org/release/" + key,
                              {"Accept: application/json"}, 2 * 1024 * 1024);
    if (response.status == 200) {
        auto root = Json::parse(std::string_view(
            reinterpret_cast<const char*>(response.body.data()), response.body.size()));
        if (auto images = root.find("images"); images && images->isArray()) {
            for (const auto& image : images->asArray()) {
                auto front = image.find("front");
                if (!front || !front->isBool() || !front->asBool()) continue;
                if (auto thumbs = image.find("thumbnails"); thumbs && thumbs->isObject()) {
                    auto preferred = json_string(thumbs->find(config_.cover_size));
                    if (preferred.empty()) preferred = json_string(thumbs->find("500"));
                    if (!preferred.empty()) { result = std::move(preferred); break; }
                }
                auto original = json_string(image.find("image"));
                if (!original.empty()) { result = std::move(original); break; }
            }
        }
    }
    cover_cache_[key] = result;
    return result;
}

std::optional<ProviderMatch> MusicBrainzProvider::lookup(const MediaProbe& probe) {
    if (!supports(probe.kind)) return {};
    auto release = find_release(probe);
    if (!release) return {};
    const auto release_id = json_string(release->find("id"));
    if (release_id.empty()) return {};
    const auto credited = artist_credit_name(release->find("artist-credit"));
    std::string artist_id;
    std::string artist_name = credited.empty() ? probe.artist : credited;
    if (auto credit = release->find("artist-credit"); credit && credit->isArray() && !credit->asArray().empty()) {
        auto artist = credit->asArray().front().find("artist");
        if (artist && artist->isObject()) {
            artist_id = json_string(artist->find("id"));
            auto canonical = json_string(artist->find("name"));
            if (!canonical.empty()) artist_name = canonical;
        }
    }
    if (artist_id.empty()) artist_id = normalized(artist_name);

    const Json* recording = nullptr;
    int position = 0;
    if (auto media = release->find("media"); media && media->isArray()) {
        for (const auto& medium : media->asArray()) {
            if (probe.disc) {
                auto medium_position = json_i32(medium.find("position"));
                if (medium_position && *medium_position != *probe.disc) continue;
            }
            auto tracks = medium.find("tracks");
            if (!tracks || !tracks->isArray()) continue;
            for (const auto& track : tracks->asArray()) {
                auto number = json_i32(track.find("position"));
                const auto title = json_string(track.find("title"));
                bool number_match = probe.track && number && *probe.track == *number;
                bool title_match = normalized(title) == normalized(probe.title);
                if (!number_match && !title_match) continue;
                recording = track.find("recording");
                position = number.value_or(probe.track.value_or(0));
                break;
            }
            if (recording) break;
        }
    }
    if (!recording || !recording->isObject()) return {};
    auto recording_id = json_string(recording->find("id"));
    if (recording_id.empty()) return {};

    std::string release_group_id;
    if (auto group = release->find("release-group"); group && group->isObject())
        release_group_id = json_string(group->find("id"));
    const auto album_key = release_group_id.empty() ? release_id : release_group_id;

    CatalogueItem artist;
    artist.id = item_id("musicbrainz", "artist", artist_id);
    artist.kind = CatalogueKind::artist;
    artist.title = artist_name;
    artist.sort_title = artist.title;
    artist.external_ids["musicbrainz"] = artist_id;
    scanner_marker(artist);

    CatalogueItem album;
    album.id = item_id("musicbrainz", "album", album_key);
    album.kind = CatalogueKind::album;
    album.title = json_string(release->find("title"));
    if (album.title.empty()) album.title = probe.album;
    album.sort_title = album.title;
    album.parent_id = artist.id;
    album.year = json_year(release->find("date"));
    album.external_ids["musicbrainz_release"] = release_id;
    if (!release_group_id.empty()) album.external_ids["musicbrainz_release_group"] = release_group_id;
    scanner_marker(album);

    CatalogueItem track;
    track.id = item_id("musicbrainz", "recording", recording_id);
    track.kind = CatalogueKind::track;
    track.title = json_string(recording->find("title"));
    if (track.title.empty()) track.title = probe.title;
    track.sort_title = track.title;
    track.parent_id = album.id;
    track.disc_number = probe.disc;
    track.track_number = position ? std::optional<int32_t>(position) : probe.track;
    track.external_ids["musicbrainz"] = recording_id;
    track.media_ids = {probe.media_id};
    scanner_marker(track);

    ProviderMatch result;
    result.items = {artist, album, track};
    if (auto cover = cover_url(release_id)) add_art(result.artwork, album.id, "cover", *cover);
    return result;
}

CatalogueScanner::CatalogueScanner(NodeRuntime& node, FileSystem& fs, CatalogueManager& catalogue,
                                   CatalogueScannerConfig config, std::unique_ptr<HttpClient> http)
    : node_(node), fs_(fs), catalogue_(catalogue), config_(std::move(config)),
      http_(http ? std::move(http) : std::make_unique<CurlHttpClient>()) {
    configure_providers();
}
CatalogueScanner::~CatalogueScanner() { stop(); }

void CatalogueScanner::configure_providers() {
    providers_.clear();
    if (config_.tmdb.enabled) {
        try { providers_.push_back(std::make_unique<TmdbProvider>(*http_, config_.tmdb)); }
        catch (const std::exception& e) { Log::warn("catalogue TMDB disabled: " + std::string(e.what())); }
    }
    if (config_.musicbrainz.enabled)
        providers_.push_back(std::make_unique<MusicBrainzProvider>(*http_, config_.musicbrainz));
}

bool CatalogueScanner::coordinator() const {
    auto active = node_.membership().active();
    auto self = node_.node_id();
    for (const auto& peer : active) if (peer.id < self) return false;
    return true;
}

void CatalogueScanner::start() {
    std::lock_guard lock(config_mutex_);
    if (!config_.enabled || worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}
void CatalogueScanner::stop() {
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
}
void CatalogueScanner::reconfigure(CatalogueScannerConfig config) {
    stop();
    {
        std::lock_guard lock(config_mutex_);
        config_ = std::move(config);
        configure_providers();
    }
    start();
}

void CatalogueScanner::walk(std::string_view root,
                            std::vector<std::pair<std::string, FsEntry>>& out) {
    std::vector<std::string> pending{normalize_path(std::string(root))};
    while (!pending.empty()) {
        auto path = std::move(pending.back());
        pending.pop_back();
        for (auto& [name, entry] : fs_.readdir(path)) {
            if (name == "." || name == "..") continue;
            auto child = path == "/" ? "/" + name : path + "/" + name;
            if (entry.type == EntryType::directory) pending.push_back(std::move(child));
            else out.emplace_back(std::move(child), std::move(entry));
        }
    }
}

size_t CatalogueScanner::scan_once() {
    CatalogueScannerConfig config;
    {
        std::lock_guard lock(config_mutex_);
        config = config_;
    }
    if (!config.enabled || !coordinator()) return 0;
    std::vector<std::pair<std::string, FsEntry>> files;
    size_t roots_scanned = 0;
    size_t roots_unavailable = 0;
    for (const auto& root : config.roots) {
        try {
            walk(root, files);
            ++roots_scanned;
        } catch (const FsError& e) {
            if (e.code() != ENOENT) throw;
            ++roots_unavailable;
            Log::debug("catalogue scan: root unavailable root=" + root +
                       " reason=" + e.what());
        }
    }
    const bool complete_scan = roots_unavailable == 0;
    if (!complete_scan) {
        Log::info("catalogue scan: partial roots_scanned=" + std::to_string(roots_scanned) +
                  " roots_unavailable=" + std::to_string(roots_unavailable) +
                  " files=" + std::to_string(files.size()));
    }

    auto existing = catalogue_.snapshot();
    std::set<std::string> bound;
    for (const auto& [_, item] : existing.items)
        bound.insert(item.media_ids.begin(), item.media_ids.end());
    std::set<std::string> active_media_ids;

    std::map<std::string, CatalogueItem> discovered;
    std::vector<RemoteArtwork> remote_art;
    size_t matched = 0;
    for (const auto& [path, entry] : files) {
        auto probe = probe_media_path(path, entry);
        if (!probe) continue;
        active_media_ids.insert(probe->media_id);
        if (bound.contains(probe->media_id)) continue;
        std::optional<ProviderMatch> match;
        for (auto& provider : providers_) {
            if (!provider->supports(probe->kind)) continue;
            try { match = provider->lookup(*probe); }
            catch (const std::exception& e) {
                Log::warn("catalogue lookup failed for " + path + ": " + e.what());
            }
            if (match) break;
        }
        if (!match) {
            std::ostringstream parsed;
            if (probe->kind == MediaProbeKind::movie) {
                parsed << "movie title=\"" << probe->title << "\"";
                if (probe->year) parsed << " year=" << *probe->year;
            } else if (probe->kind == MediaProbeKind::episode) {
                parsed << "episode series=\"" << probe->series << "\"";
                if (probe->year) parsed << " year=" << *probe->year;
                if (probe->season) parsed << " season=" << *probe->season;
                if (probe->episode) parsed << " episode=" << *probe->episode;
            } else {
                parsed << "track artist=\"" << probe->artist << "\" album=\""
                       << probe->album << "\" title=\"" << probe->title << "\"";
            }
            Log::debug("catalogue: no provider match for " + path + " parsed " + parsed.str());
            continue;
        }
        ++matched;
        for (auto& item : match->items) {
            auto [it, inserted] = discovered.emplace(item.id, item);
            if (!inserted) {
                for (const auto& media : item.media_ids)
                    if (std::find(it->second.media_ids.begin(), it->second.media_ids.end(), media) == it->second.media_ids.end())
                        it->second.media_ids.push_back(media);
            }
        }
        remote_art.insert(remote_art.end(), match->artwork.begin(), match->artwork.end());
    }

    for (const auto& art : remote_art) {
        CatalogueItem* target = nullptr;
        if (auto it = discovered.find(art.item_id); it != discovered.end()) target = &it->second;
        const CatalogueItem* old = nullptr;
        if (auto it = existing.items.find(art.item_id); it != existing.items.end()) old = &it->second;
        if (!target || has_art_role(target, art.role) || has_art_role(old, art.role)) continue;
        try {
            auto response = http_->get(art.url, {}, config.max_artwork_bytes);
            if (response.status != 200 || response.body.empty()) continue;
            auto mime = response.content_type;
            if (auto semi = mime.find(';'); semi != std::string::npos) mime.resize(semi);
            if (!mime.starts_with("image/")) {
                Log::warn("catalogue artwork returned non-image content for " + art.item_id);
                continue;
            }
            target->artwork.push_back(catalogue_.stage_artwork(art.role, mime, response.body));
        } catch (const std::exception& e) {
            Log::warn("catalogue artwork failed for " + art.item_id + ": " + e.what());
        }
    }

    std::vector<CatalogueItem> items;
    items.reserve(discovered.size());
    for (auto& [_, item] : discovered) items.push_back(std::move(item));
    catalogue_.reconcile_scanner(items, active_media_ids, complete_scan);
    if (matched) Log::info("catalogue scan matched " + std::to_string(matched) + " media files");
    return matched;
}

void CatalogueScanner::loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-scanner", std::chrono::seconds(5), true);
    std::optional<Hash256> scanned_namespace;
    std::optional<std::chrono::steady_clock::time_point> mutation_due;
    auto observed_generation = node_.known_metadata_generation();
    auto next_periodic = std::chrono::steady_clock::now();
    bool was_coordinator = false;

    while (!stop.stop_requested()) {
        CatalogueScannerConfig config;
        { std::lock_guard lock(config_mutex_); config = config_; }
        const auto now = std::chrono::steady_clock::now();
        const bool is_coordinator = coordinator();
        if (is_coordinator && !was_coordinator)
            mutation_due = now; // a newly elected scanner must establish current state promptly
        was_coordinator = is_coordinator;

        const auto generation = node_.known_metadata_generation();
        if (generation != observed_generation) {
            observed_generation = generation;
            mutation_due = now + config.mutation_debounce;
            Log::debug("catalogue: metadata mutation observed; namespace rescan debounce reset generation=" +
                       std::to_string(generation));
        }

        const bool periodic_due = now >= next_periodic;
        const bool debounced_mutation_due = mutation_due && now >= *mutation_due;
        if (config.enabled && is_coordinator && (periodic_due || debounced_mutation_due)) {
            try {
                const auto before = fs_.namespace_signature();
                const bool namespace_changed = !scanned_namespace || before != *scanned_namespace;
                if (periodic_due || namespace_changed) {
                    if (debounced_mutation_due && namespace_changed && !periodic_due)
                        Log::info("catalogue: namespace mutation settled; rescanning");
                    (void)scan_once();
                    uint64_t after_generation = 0;
                    const auto after = fs_.namespace_signature(&after_generation);
                    scanned_namespace = before;
                    if (after != before) {
                        // A namespace mutation raced the scan. Do not claim that
                        // state as scanned; run again once the new mutation settles.
                        mutation_due = std::chrono::steady_clock::now() + config.mutation_debounce;
                    } else {
                        scanned_namespace = after;
                        mutation_due.reset();
                    }
                    // Record the generation represented by `after`, not a later
                    // live value. A namespace commit racing immediately after the
                    // signature read will then be observed on the next loop.
                    observed_generation = after_generation;
                    next_periodic = std::chrono::steady_clock::now() + config.interval;
                } else {
                    // The metadata generation changed only because catalogue or
                    // other non-namespace state changed. Suppress a pointless scan.
                    mutation_due.reset();
                }
            } catch (const std::exception& e) {
                Log::warn("catalogue scan: " + std::string(e.what()));
                mutation_due = std::chrono::steady_clock::now() + config.mutation_debounce;
                next_periodic = std::chrono::steady_clock::now() + config.interval;
            }
        }
        cpu_reporter.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace macha
