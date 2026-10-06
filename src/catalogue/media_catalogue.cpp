// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/media_catalogue.hpp"
#include "diagnostics.hpp"

#include "crypto.hpp"
#include "log.hpp"
#include "supervised.hpp"
#include "macha_version.hpp"
#include "media/media_metadata.hpp"
#include "media/media_containers.hpp"
#include "catalogue/media_information.hpp"

#include <curl/curl.h>


#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

namespace macha {
namespace {

constexpr size_t provider_cache_max_entries = 256;
constexpr size_t provider_cache_max_bytes = 8ULL * 1024 * 1024;

size_t provider_cache_weight(const Json& value) {
    // A conservative estimate of parsed DOM cost over its text, not accounting.
    return sizeof(Json) + value.dump().size() * 2;
}

size_t provider_cache_weight(const std::string& value) {
    return sizeof(std::string) + value.capacity();
}

template <class T>
size_t provider_cache_weight(const std::optional<T>& value) {
    return sizeof(std::optional<T>) + (value ? provider_cache_weight(*value) : 0);
}

template <class Map, class Value>
void provider_cache_store(Map& cache, size_t& owned_bytes, std::string key, Value value) {
    using Stored = typename Map::mapped_type;
    Stored stored(std::move(value));
    const auto weight = sizeof(typename Map::value_type) + key.capacity() +
                        provider_cache_weight(stored);
    if (weight > provider_cache_max_bytes)
        return;
    if (auto found = cache.find(key); found != cache.end()) {
        owned_bytes -= sizeof(typename Map::value_type) + found->first.capacity() +
                       provider_cache_weight(found->second);
        found->second = std::move(stored);
        owned_bytes += weight;
        return;
    }
    while (!cache.empty() &&
           (cache.size() >= provider_cache_max_entries ||
            owned_bytes > provider_cache_max_bytes - weight)) {
        auto victim = cache.begin();
        owned_bytes -= sizeof(typename Map::value_type) + victim->first.capacity() +
                       provider_cache_weight(victim->second);
        cache.erase(victim);
    }
    // Provider maps share one budget this cache cannot evict from others: when
    // it is spent, decline the optional entry.
    if (owned_bytes > provider_cache_max_bytes - weight)
        return;
    cache.emplace(std::move(key), std::move(stored));
    owned_bytes += weight;
}

class CatalogueMediaInput final : public MediaInput {
    std::shared_ptr<ReadHandle> handle_;
    uint64_t size_{};
  public:
    CatalogueMediaInput(std::shared_ptr<ReadHandle> handle, uint64_t size)
        : handle_(std::move(handle)), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination,
                Clock::time_point deadline, std::atomic_bool* cancelled) override {
        if (offset >= size_) return 0;
        const auto wanted = static_cast<size_t>(
            std::min<uint64_t>(destination.size(), size_ - offset));
        return handle_->read(offset, destination.first(wanted), deadline, cancelled);
    }
};

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

struct ScannerPersistentState {
    std::optional<Hash256> namespace_signature;
    uint64_t next_safety_scan_unix_ms{};
    bool reconciled{true};
};

std::filesystem::path scanner_state_path(const std::filesystem::path& state_path) {
    return state_path / "catalogue" / "scanner.state";
}

ScannerPersistentState load_scanner_state(const std::filesystem::path& state_path) {
    ScannerPersistentState state;
    std::ifstream in(scanner_state_path(state_path));
    if (!in)
        return state;

    std::string signature;
    uint64_t next_due{};
    if (!(in >> signature >> next_due))
        return {};
    int reconciled = 1;
    if (in >> reconciled)
        state.reconciled = reconciled != 0;
    auto bytes = unhex(signature);
    if (!bytes || bytes->size() != state.namespace_signature.emplace().bytes.size())
        return {};
    std::copy(bytes->begin(), bytes->end(), state.namespace_signature->bytes.begin());
    state.next_safety_scan_unix_ms = next_due;
    return state;
}

void persist_scanner_state(const std::filesystem::path& state_path, const Hash256& signature,
                           uint64_t next_safety_scan_unix_ms, bool reconciled = true) {
    const auto path = scanner_state_path(state_path);
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write catalogue scanner state " + temp.string());
        out << to_string(signature) << ' ' << next_safety_scan_unix_ms << ' '
            << (reconciled ? 1 : 0) << '\n';
        out.flush();
        if (!out)
            throw std::runtime_error("cannot flush catalogue scanner state " + temp.string());
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temp, path, error);
    }
    if (error)
        throw std::runtime_error("cannot publish catalogue scanner state: " + error.message());
}

uint64_t next_scan_due_unix_ms(std::chrono::milliseconds interval) {
    const auto now = unix_ms();
    const auto delta = static_cast<uint64_t>(std::max<int64_t>(1, interval.count()));
    return now > std::numeric_limits<uint64_t>::max() - delta
        ? std::numeric_limits<uint64_t>::max()
        : now + delta;
}

Clock::time_point steady_due_from_unix_ms(uint64_t due_unix_ms) {
    const auto wall_now = unix_ms();
    if (!due_unix_ms || due_unix_ms <= wall_now)
        return Clock::now();
    const auto remaining = due_unix_ms - wall_now;
    const auto capped = std::min<uint64_t>(
        remaining, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    return Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(capped));
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
    // Matching forgives what parsing keeps: filenames and providers differ on
    // punctuation, apostrophes, ampersands, sequel numerals and abbreviations.
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
        {"volume", "vol"}, {"vol", "vol"},
    };

    std::string prepared;
    prepared.reserve(value.size() + 8);
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '&') {
            prepared += " and ";
        } else if (c == '\'') {
            // "Knight's" compares equal to "Knights".
            continue;
        } else if (c == 0xe2 && i + 2 < value.size() &&
                   static_cast<unsigned char>(value[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(value[i + 2]) == 0x98 ||
                    static_cast<unsigned char>(value[i + 2]) == 0x99)) {
            i += 2; // UTF-8 left/right single quotation mark.
        } else {
            prepared.push_back(static_cast<char>(c));
        }
    }

    std::istringstream in(normalized(prepared));
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

std::optional<int32_t> year_from(std::string_view text);

struct MusicMetadataReadResult {
    MediaProbe probe;
    std::vector<LocalArtworkCandidate> artwork;
};

std::optional<MusicMetadataReadResult> read_music_metadata_probe(FileSystem& fs,
                                                                  std::string_view path,
                                                                  const FsEntry& entry,
                                                                  size_t max_artwork_bytes) {
    if (entry.type != EntryType::file || entry.size == 0 ||
        !audio_extension(path_extension(path)))
        return {};

    MusicMetadataReadResult result;
    auto& probe = result.probe;
    probe.kind = MediaProbeKind::track;
    probe.path = normalize_path(std::string(path));
    probe.media_id = file_media_id(entry);
    try {
        apply_embedded_music_metadata(fs, path, entry, probe, result.artwork, max_artwork_bytes);
    } catch (const std::exception& e) {
        Log::debug("catalogue music tags unavailable path=" + std::string(path) +
                   " reason=" + e.what());
    }
    return result;
}

// No release year is later than next year, so "Blade Runner 2049" keeps its
// number as title.
int32_t plausible_year_ceiling() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
    gmtime_r(&now, &parts);
    return parts.tm_year + 1900 + 1;
}

std::optional<int32_t> year_from(std::string_view text) {
    static const std::regex re(R"((?:19|20)[0-9]{2})");
    static const int32_t ceiling = plausible_year_ceiling();
    std::string owned(text);
    std::optional<int32_t> result;
    for (std::sregex_iterator it(owned.begin(), owned.end(), re), end; it != end; ++it) {
        const auto pos = static_cast<size_t>((*it).position());
        const auto len = static_cast<size_t>((*it).length());
        const bool numeric_left = pos && std::isdigit(static_cast<unsigned char>(owned[pos - 1]));
        const bool numeric_right = pos + len < owned.size() &&
                                   std::isdigit(static_cast<unsigned char>(owned[pos + len]));
        if (numeric_left || numeric_right) continue;
        // Dimensions such as 1920x816 are not years.
        if (pos + len < owned.size() && (owned[pos + len] == 'x' || owned[pos + len] == 'X') &&
            pos + len + 1 < owned.size() &&
            std::isdigit(static_cast<unsigned char>(owned[pos + len + 1])))
            continue;
        const auto value = std::stoi((*it).str());
        // A number the calendar has not reached is title text, not a year.
        if (value > ceiling) continue;
        result = value;
    }
    return result;
}

std::string strip_release_noise(std::string value) {
    static const std::regex technical(
        R"((?:^|[ ._\-(\[]+)(?:[0-9]{3,4}x[0-9]{3,4}|2160p|1440p|1080p|720p|576p|480p|360p|uhd|bluray|blu-ray|bdrip|webrip|web-dl|webdl|hdtv|hdrip|dvdrip|dvd|brrip|xvid|divx|remux|amzn|nf|x264|x265|h264|h265|h\.264|h\.265|hevc|avc|av1|vp9|aac(?:[0-9.]*)?|eac3|ac3|ddp(?:[0-9.]*)?|dd(?:[0-9.]*)?|dts(?:-hd)?(?:[ .]ma)?|flac|opus|multi-subs|multisubs)\b.*$)",
        std::regex::icase);
    std::smatch match;
    if (std::regex_search(value, match, technical))
        value.resize(static_cast<size_t>(match.position()));
    // Remove trailing brackets only when they look like release/site tags
    // ([rartv], [EZTVx.to]); [Pilot] is title text.
    static const std::regex site_tag(R"([ ._-]*\[([^\]]+)\]\s*$)", std::regex::icase);
    std::smatch site_match;
    if (std::regex_search(value, site_match, site_tag)) {
        auto tag = lower(trim(site_match[1].str()));
        static const std::set<std::string> known_site_tags{
            "eztv", "eztvx", "eztvx.to", "ettv", "galaxyrg", "i_c",
            "qxr", "rarbg", "rartv", "tgx", "torrentgalaxy", "utr",
            "yify", "yts", "yts.mx"};
        const bool looks_like_site = tag.find('.') != std::string::npos ||
                                     tag.find('_') != std::string::npos ||
                                     tag.find('@') != std::string::npos ||
                                     known_site_tags.contains(tag);
        if (looks_like_site)
            value.resize(static_cast<size_t>(site_match.position()));
    }
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

    // Cut from the first bundle marker ("Season 1-4 S01-S04", "Complete
    // Series"): it describes the layout, not the series title.
    static const std::regex bundle_suffix(
        R"((?:[ ._\-]+)(?:(?:season|seasons|series)[ ._\-]*[0-9]{1,2}[ ._\-]*-[ ._\-]*[0-9]{1,2}|s[0-9]{1,2}[ ._\-]*-[ ._\-]*s?[0-9]{1,2}|complete(?:[ ._\-]+series)?)(?:[ ._\-].*)?$)",
        std::regex::icase);
    value = std::regex_replace(value, bundle_suffix, "");

    static const std::regex season_suffix(
        R"((?:[ ._\-]+)(?:s[0-9]{1,2}|season[ ._\-]*[0-9]{1,2}|series[ ._\-]*[0-9]{1,2})\s*$)",
        std::regex::icase);
    value = std::regex_replace(value, season_suffix, "");
    if (auto year = year_from(value)) value = remove_year_token(std::move(value), *year);
    return clean_title(value);
}

std::string clean_episode_title(std::string value) {
    // Recover a possessive written as a dot ("Tasty.Tudi.s") before cleanup
    // makes it a standalone S.
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] != '.' || (value[i + 1] != 's' && value[i + 1] != 'S')) continue;
        if (!std::isalnum(static_cast<unsigned char>(value[i - 1]))) continue;
        if (i + 2 < value.size() &&
            std::isalnum(static_cast<unsigned char>(value[i + 2]))) continue;
        value[i] = '\'';
    }
    value = strip_release_noise(std::move(value));
    return clean_title(value);
}

std::string clean_music_artist_directory(std::string value) {
    static const std::regex discography_suffix(
        R"(^\s*(.+?)\s+[ ._-]*discography(?:[ ._@\[(].*)?$)", std::regex::icase);
    std::smatch match;
    if (std::regex_match(value, match, discography_suffix)) value = match[1].str();
    return clean_title(value);
}

std::string clean_music_album_directory(std::string value,
                                        std::optional<int32_t>& year) {
    static const std::regex year_prefix(
        R"(^\s*((?:19|20)[0-9]{2})\s*[ ._-]+\s*(.+?)\s*$)", std::regex::icase);
    std::smatch match;
    if (std::regex_match(value, match, year_prefix)) {
        if (!year) year = std::stoi(match[1].str());
        value = match[2].str();
    }
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

    // Strip zero-padded collection ordinals; "12 Monkeys" stays intact.
    static const std::regex ordinal(R"(^\s*0[0-9]{1,2}[ ._-]+)");
    value = std::regex_replace(value, ordinal, "");

    // Numbered collection entries may append an actor after " - "; only cut
    // when the part before ends in a digit, sparing "Star Wars - A New Hope".
    static const std::regex numbered_credit(
        R"(^(.+[0-9])\s+-\s+[A-Za-z][A-Za-z' .-]*$)", std::regex::icase);
    std::smatch match;
    if (std::regex_match(value, match, numbered_credit)) value = match[1].str();
    return clean_title(value);
}


MediaProbe make_probe(std::string_view path, const FsEntry& entry, MediaProbeKind kind) {
    MediaProbe probe;
    probe.kind = kind;
    probe.path = normalize_path(std::string(path));
    probe.media_id = file_media_id(entry);
    return probe;
}

struct EpisodePattern {
    std::string prefix;
    std::string suffix;
    int32_t season{};
    int32_t episode{};
    std::optional<int32_t> episode_end;
};

std::optional<EpisodePattern> episode_pattern(std::string_view value) {
    static const std::regex se_re(
        R"((.*?)(?:[ ._-]+|^)s(\d{1,2})e(\d{1,3})(?:[ ._-]*(?:-|e)[ ._-]*e?(\d{1,3}))?(?:[ ._-]+(.*))?$)",
        std::regex::icase);
    static const std::regex x_re(
        R"((.*?)(?:[ ._-]+|^)(\d{1,2})x(\d{1,3})(?:[ ._-]*-[ ._-]*(\d{1,3}))?(?:[ ._-]+(.*))?$)",
        std::regex::icase);
    std::string owned(value);
    std::smatch match;
    if (!std::regex_match(owned, match, se_re) && !std::regex_match(owned, match, x_re))
        return {};
    EpisodePattern out;
    out.prefix = match[1].str();
    out.season = std::stoi(match[2].str());
    out.episode = std::stoi(match[3].str());
    if (match[4].matched) {
        const auto episode_end = std::stoi(match[4].str());
        if (episode_end >= out.episode) out.episode_end = episode_end;
    }
    if (match[5].matched) out.suffix = match[5].str();
    return out;
}

std::optional<int32_t> season_directory_number(std::string_view value) {
    static const std::regex re(R"(^\s*(?:season|series)\s*([0-9]{1,2})\s*$)",
                               std::regex::icase);
    std::smatch match;
    std::string owned = clean_title(std::string(value));
    if (lower(owned) == "specials") return 0;
    if (!std::regex_match(owned, match, re)) return {};
    return std::stoi(match[1].str());
}

std::string strip_collection_ordinal(std::string value) {
    static const std::regex ordinal(R"(^\s*0[0-9]{1,2}[ ._-]+)");
    return std::regex_replace(value, ordinal, "");
}

std::pair<std::string, std::optional<std::string>> strip_movie_edition(std::string value) {
    static const std::regex edition_re(
        R"((?:^|[ ._\-]+)(director'?s?[ ._\-]+cut|extended(?:[ ._\-]+edition)?|remastered|unrated|special[ ._\-]+edition|theatrical(?:[ ._\-]+cut)?)(?=$|[ ._\-]+))",
        std::regex::icase);
    std::smatch match;
    std::optional<std::string> edition;
    while (std::regex_search(value, match, edition_re)) {
        if (!edition) edition = clean_title(match[1].str());
        value.replace(static_cast<size_t>(match.position()),
                      static_cast<size_t>(match.length()), " ");
    }
    return {trim(value), edition};
}

struct YearPosition {
    int32_t year{};
    size_t pos{};
    size_t length{};
};

std::vector<YearPosition> year_positions(std::string_view text) {
    static const std::regex re(R"((?:19|20)[0-9]{2})");
    static const int32_t ceiling = plausible_year_ceiling();
    std::string owned(text);
    std::vector<YearPosition> out;
    for (std::sregex_iterator it(owned.begin(), owned.end(), re), end; it != end; ++it) {
        const auto pos = static_cast<size_t>((*it).position());
        const auto len = static_cast<size_t>((*it).length());
        if (pos && std::isdigit(static_cast<unsigned char>(owned[pos - 1]))) continue;
        if (pos + len < owned.size() &&
            std::isdigit(static_cast<unsigned char>(owned[pos + len]))) continue;
        if (pos + len < owned.size() && (owned[pos + len] == 'x' || owned[pos + len] == 'X') &&
            pos + len + 1 < owned.size() &&
            std::isdigit(static_cast<unsigned char>(owned[pos + len + 1])))
            continue;
        const auto value = std::stoi((*it).str());
        // The same year ceiling as year_from().
        if (value > ceiling) continue;
        out.push_back({value, pos, len});
    }
    return out;
}

int movie_year_score(std::string_view core, const YearPosition& year) {
    int score = 60;
    if (year.pos == 0) score += 55;
    if (year.pos + year.length == core.size()) score += 80;
    if (year.pos && core[year.pos - 1] == '(' && year.pos + year.length < core.size() &&
        core[year.pos + year.length] == ')') score += 90;
    if (!core.empty()) score += static_cast<int>((year.pos * 35) / core.size());
    return score;
}

std::string title_for_movie_year(std::string core, const YearPosition& year) {
    if (year.pos == 0) {
        core.erase(0, year.length);
    } else {
        core.resize(year.pos);
    }
    core = strip_collection_ordinal(std::move(core));
    return clean_title(core);
}

bool same_probe_identity(const MediaProbe& a, const MediaProbe& b) {
    return a.kind == b.kind && a.title == b.title && a.year == b.year && a.edition == b.edition &&
           a.series == b.series && a.season == b.season && a.episode == b.episode &&
           a.episode_end == b.episode_end &&
           a.artist == b.artist && a.album == b.album && a.disc == b.disc && a.track == b.track &&
           a.lookup_strategy == b.lookup_strategy;
}

class SemanticMovieCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "movie-semantic"; }

    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        const auto path = context.path;
        const auto& entry = context.entry;
        if (!video_extension(path_extension(path)) || episode_pattern(stem(path))) return {};
        auto core = strip_release_noise(stem(path));
        auto [without_edition, edition] = strip_movie_edition(core);
        core = std::move(without_edition);

        std::vector<MediaProbeCandidate> out;
        auto years = year_positions(core);
        for (const auto& year : years) {
            auto title = title_for_movie_year(core, year);
            if (title.empty()) continue;
            auto probe = make_probe(path, entry, MediaProbeKind::movie);
            probe.title = std::move(title);
            probe.year = year.year;
            probe.edition = edition;
            int score = 180 + movie_year_score(core, year);
            std::vector<std::string> evidence{
                "year before technical release boundary",
                year.pos == 0 ? "leading release year" : "title/year boundary"};
            if (edition) evidence.push_back("edition metadata separated from title");
            out.push_back({std::move(probe), score, std::string(name()), std::move(evidence)});
        }

        if (years.empty()) {
            auto title = clean_title(strip_collection_ordinal(core));
            if (!title.empty()) {
                auto probe = make_probe(path, entry, MediaProbeKind::movie);
                probe.title = std::move(title);
                probe.edition = edition;
                int score = 175;
                std::vector<std::string> evidence{"technical release boundary"};
                if (edition) evidence.push_back("edition metadata separated from title");
                out.push_back({std::move(probe), score, std::string(name()), std::move(evidence)});
            }
        }

        // Keep the full title, but add a stronger candidate split at " - " when
        // the right side has a year and distribution metadata.
        const auto split = core.find(" - ");
        if (split != std::string::npos) {
            const auto left = clean_title(strip_collection_ordinal(core.substr(0, split)));
            const auto right = core.substr(split + 3);
            if (!left.empty() && year_from(right)) {
                static const std::regex release_words(
                    R"(\b(?:eng|english|rus|ita|multi|subs?|comm|commentary|sci[ ._-]*fi|h264|h265|x264|x265)\b)",
                    std::regex::icase);
                if (std::regex_search(right, release_words)) {
                    auto probe = make_probe(path, entry, MediaProbeKind::movie);
                    probe.title = left;
                    probe.year = year_from(right);
                    probe.edition = edition;
                    out.push_back({std::move(probe), 330, std::string(name()),
                                   {"human title before release-description separator"}});
                }
            }
        }
        return out;
    }
};

class CompactMovieTitleCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "movie-compact-title"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        const auto path = context.path;
        const auto& entry = context.entry;
        if (!video_extension(path_extension(path)) || episode_pattern(stem(path))) return {};
        const auto raw = stem(path);
        static const std::regex prefixed_compact(
            R"(^([a-z0-9]{2,12})-([a-z][a-z0-9]{5,})$)");
        std::smatch match;
        if (!std::regex_match(raw, match, prefixed_compact)) return {};
        std::string compact = match[2].str();
        const auto and_pos = compact.find("and");
        if (and_pos == std::string::npos || and_pos < 2 || and_pos + 5 > compact.size()) return {};
        compact.insert(and_pos, " ");
        compact.insert(and_pos + 4, " ");
        auto probe = make_probe(path, entry, MediaProbeKind::movie);
        probe.title = clean_title(compact);
        return {{std::move(probe), 150, std::string(name()),
                 {"compact uploader-prefix title hypothesis"}}};
    }
};

class LegacyMovieCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "movie-legacy"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        const auto path = context.path;
        const auto& entry = context.entry;
        if (!video_extension(path_extension(path)) || episode_pattern(stem(path))) return {};
        auto probe = make_probe(path, entry, MediaProbeKind::movie);
        probe.year = year_from(stem(path));
        probe.title = movie_title_before_year(stem(path), probe.year);
        if (probe.title.empty()) return {};
        return {{std::move(probe), 90, std::string(name()), {"legacy deterministic parser"}}};
    }
};

class FilenameEpisodeCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "episode-filename"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        const auto path = context.path;
        const auto& entry = context.entry;
        if (!video_extension(path_extension(path))) return {};
        auto pattern = episode_pattern(stem(path));
        if (!pattern) return {};
        const auto prefix = strip_collection_ordinal(pattern->prefix);
        if (clean_series_name(prefix).empty()) return {};
        auto probe = make_probe(path, entry, MediaProbeKind::episode);
        probe.series = clean_series_name(prefix);
        probe.year = year_from(prefix);
        probe.season = pattern->season;
        probe.episode = pattern->episode;
        probe.episode_end = pattern->episode_end;
        probe.title = clean_episode_title(pattern->suffix);
        int score = 260;
        std::vector<std::string> evidence{"series prefix adjacent to SxxExx", "explicit episode marker"};

        const auto parts = components(path);
        if (parts.size() >= 3) {
            if (auto parent_season = season_directory_number(parts[parts.size() - 2])) {
                if (*parent_season == pattern->season) {
                    score += 35;
                    evidence.push_back("season directory agrees with filename");
                }
                const auto& raw_series_dir = parts[parts.size() - 3];
                const auto dir_series = clean_series_name(raw_series_dir);
                if (!probe.year && comparable_title(dir_series) == comparable_title(probe.series)) {
                    probe.year = year_from(raw_series_dir);
                    if (probe.year) {
                        score += 25;
                        evidence.push_back("series directory supplies matching year");
                    }
                }
            }
        }
        return {{std::move(probe), score, std::string(name()), std::move(evidence)}};
    }
};

class YearlessFilenameEpisodeCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "episode-filename-yearless"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        FilenameEpisodeCandidateGenerator filename;
        auto candidates = filename.generate(context);
        if (candidates.empty() || !candidates.front().probe.year) return {};
        auto candidate = std::move(candidates.front());
        candidate.probe.year.reset();
        candidate.score -= 45;
        candidate.generator = std::string(name());
        candidate.evidence.push_back("yearless provider fallback for ambiguous TV premiere year");
        return {std::move(candidate)};
    }
};

class DirectoryEpisodeCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "episode-directory"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        const auto path = context.path;
        const auto& entry = context.entry;
        if (!video_extension(path_extension(path))) return {};
        auto pattern = episode_pattern(stem(path));
        if (!pattern) return {};
        const auto parts = components(path);
        if (parts.size() < 2) return {};

        size_t series_index = parts.size() - 2;
        int score = 175;
        std::vector<std::string> evidence{"directory-derived series", "explicit episode marker"};
        if (parts.size() >= 3) {
            if (auto directory_season = season_directory_number(parts[parts.size() - 2])) {
                series_index = parts.size() - 3;
                if (*directory_season == pattern->season) {
                    score += 45;
                    evidence.push_back("season directory agrees with filename");
                }
            }
        }
        const auto raw_series = parts[series_index];
        auto series = clean_series_name(raw_series);
        if (series.empty()) return {};
        auto probe = make_probe(path, entry, MediaProbeKind::episode);
        probe.series = std::move(series);
        probe.year = year_from(raw_series);
        probe.season = pattern->season;
        probe.episode = pattern->episode;
        probe.episode_end = pattern->episode_end;
        probe.title = clean_episode_title(pattern->suffix);
        if (probe.year) {
            score += 20;
            evidence.push_back("series directory contains year");
        }
        return {{std::move(probe), score, std::string(name()), std::move(evidence)}};
    }
};

class StructuredMusicCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "music-structured-path"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        const auto root = context.root;
        const auto path = context.path;
        const auto& entry = context.entry;
        if (!audio_extension(path_extension(path))) return {};
        auto probe = make_probe(path, entry, MediaProbeKind::track);
        auto title_candidate = stem(path);
        static const std::regex track_re(
            R"(^\s*(?:(\d{1,2})[-.]\s*)?(\d{1,3})\s*[-_. ]+(.+)$)", std::regex::icase);
        std::smatch track_match;
        if (std::regex_match(title_candidate, track_match, track_re)) {
            if (track_match[1].matched) probe.disc = std::stoi(track_match[1].str());
            probe.track = std::stoi(track_match[2].str());
            title_candidate = track_match[3].str();
        }

        static const std::regex artist_title(R"(^\s*(.+?)\s+-\s+(.+?)\s*$)");
        std::smatch artist_match;
        bool artist_from_filename = false;
        if (std::regex_match(title_candidate, artist_match, artist_title)) {
            probe.artist = clean_title(artist_match[1].str());
            probe.title = clean_title(artist_match[2].str());
            artist_from_filename = true;
        } else {
            probe.title = clean_title(title_candidate);
        }

        const auto normalized_root = normalize_path(std::string(root));
        const auto root_parts = components(normalized_root);
        const auto parts = components(path);
        if (!root.empty() && parts.size() >= root_parts.size() &&
            std::equal(root_parts.begin(), root_parts.end(), parts.begin())) {
            const auto relative_parts = parts.size() - root_parts.size();
            if (relative_parts == 3) {
                const auto candidate_artist = clean_music_artist_directory(parts[parts.size() - 3]);
                const auto candidate_album = clean_music_album_directory(parts[parts.size() - 2], probe.year);
                if (probe.artist.empty()) probe.artist = candidate_artist;
                if (probe.album.empty() && (!artist_from_filename ||
                    comparable_title(probe.artist) == comparable_title(candidate_artist)))
                    probe.album = candidate_album;
            } else if (relative_parts == 4) {
                static const std::regex disc_dir(R"(^(?:cd|disc|disk)\s*([0-9]{1,2})$)",
                                                 std::regex::icase);
                std::smatch disc_match;
                auto parent = clean_title(parts[parts.size() - 2]);
                if (std::regex_match(parent, disc_match, disc_dir)) {
                    if (!probe.disc) probe.disc = std::stoi(disc_match[1].str());
                    if (probe.artist.empty())
                        probe.artist = clean_music_artist_directory(parts[parts.size() - 4]);
                    if (probe.album.empty())
                        probe.album = clean_music_album_directory(parts[parts.size() - 3], probe.year);
                }
            }
        } else if (parts.size() >= 3 && probe.artist.empty()) {
            // No configured root: fall back to Artist/Album[/Disc]/File.
            static const std::regex disc_dir(R"(^(?:cd|disc|disk)\s*([0-9]{1,2})$)",
                                             std::regex::icase);
            std::smatch disc_match;
            auto parent = clean_title(parts[parts.size() - 2]);
            if (parts.size() >= 4 && std::regex_match(parent, disc_match, disc_dir)) {
                if (!probe.disc) probe.disc = std::stoi(disc_match[1].str());
                probe.album = clean_music_album_directory(parts[parts.size() - 3], probe.year);
                probe.artist = clean_music_artist_directory(parts[parts.size() - 4]);
            } else {
                probe.album = clean_music_album_directory(parts[parts.size() - 2], probe.year);
                probe.artist = clean_music_artist_directory(parts[parts.size() - 3]);
            }
        }

        if (probe.title.empty()) return {};
        int score = 190 + (probe.track ? 25 : 0) + (!probe.artist.empty() ? 20 : 0) +
                    (!probe.album.empty() ? 20 : 0);
        return {{std::move(probe), score, std::string(name()),
                 {"structured music filename/path fallback"}}};
    }
};

bool has_embedded_music_tags(const MediaProbe& probe) {
    return !probe.title.empty() || !probe.artist.empty() || !probe.album.empty() ||
           probe.track || probe.disc || probe.year || probe.musicbrainz_recording_id ||
           probe.musicbrainz_release_id || probe.musicbrainz_artist_id;
}

class RecordingFirstStructuredMusicCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "music-structured-recording"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        StructuredMusicCandidateGenerator structured;
        auto candidates = structured.generate(context);
        std::vector<MediaProbeCandidate> out;
        for (auto& candidate : candidates) {
            if (candidate.probe.artist.empty() || candidate.probe.title.empty()) continue;
            candidate.probe.lookup_strategy = MediaProbeLookupStrategy::music_recording_first;
            candidate.score -= 20;
            candidate.generator = std::string(name());
            candidate.evidence.push_back("recording-first fallback for structured path metadata");
            out.push_back(std::move(candidate));
        }
        return out;
    }
};

class EmbeddedMusicTagsCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "music-embedded-tags"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        if (!context.embedded_metadata || context.embedded_metadata->kind != MediaProbeKind::track ||
            !has_embedded_music_tags(*context.embedded_metadata))
            return {};
        auto probe = *context.embedded_metadata;
        if (probe.title.empty() && !probe.musicbrainz_recording_id) return {};
        probe.lookup_strategy = (probe.musicbrainz_release_id || !probe.album.empty())
            ? MediaProbeLookupStrategy::music_release_first
            : MediaProbeLookupStrategy::music_recording_first;
        int score = 900;
        if (!probe.title.empty()) score += 35;
        if (!probe.artist.empty()) score += 35;
        if (!probe.album.empty()) score += 35;
        if (probe.track) score += 15;
        if (probe.musicbrainz_release_id) score += 120;
        if (probe.musicbrainz_recording_id) score += 140;
        return {{std::move(probe), score, std::string(name()),
                 {"embedded audio metadata", "embedded tags remain authoritative"}}};
    }
};

class RecordingFirstMusicTagsCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "music-recording-tags"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        if (!context.embedded_metadata || context.embedded_metadata->kind != MediaProbeKind::track ||
            !has_embedded_music_tags(*context.embedded_metadata))
            return {};
        auto probe = *context.embedded_metadata;
        if (!probe.track_artist.empty()) probe.artist = probe.track_artist;
        if ((probe.artist.empty() || probe.title.empty()) && !probe.musicbrainz_recording_id)
            return {};
        probe.lookup_strategy = MediaProbeLookupStrategy::music_recording_first;
        int score = 860;
        if (!probe.track_artist.empty()) score += 45;
        if (!probe.album.empty()) score += 20;
        if (probe.musicbrainz_recording_id) score += 180;
        return {{std::move(probe), score, std::string(name()),
                 {"recording-first MusicBrainz hypothesis",
                  "track artist preferred over album artist for recording search"}}};
    }
};

class TagsWithStructuredPathMusicCandidateGenerator final : public MediaProbeCandidateGenerator {
  public:
    std::string_view name() const noexcept override { return "music-tags-plus-path"; }
    std::vector<MediaProbeCandidate> generate(const MediaProbeContext& context) const override {
        if (!context.embedded_metadata || context.embedded_metadata->kind != MediaProbeKind::track ||
            !has_embedded_music_tags(*context.embedded_metadata))
            return {};
        StructuredMusicCandidateGenerator structured;
        auto path_candidates = structured.generate(context);
        std::vector<MediaProbeCandidate> out;
        for (const auto& path_candidate : path_candidates) {
            auto probe = *context.embedded_metadata;
            const auto& path_probe = path_candidate.probe;
            bool filled = false;
            if (probe.title.empty() && !path_probe.title.empty()) {
                probe.title = path_probe.title;
                filled = true;
            }
            if (probe.artist.empty() && !path_probe.artist.empty()) {
                probe.artist = path_probe.artist;
                filled = true;
            }
            if (probe.album.empty() && !path_probe.album.empty()) {
                probe.album = path_probe.album;
                filled = true;
            }
            if (!probe.disc && path_probe.disc) {
                probe.disc = path_probe.disc;
                filled = true;
            }
            if (!probe.track && path_probe.track) {
                probe.track = path_probe.track;
                filled = true;
            }
            if (!probe.year && path_probe.year) {
                probe.year = path_probe.year;
                filled = true;
            }
            if (!filled || (probe.title.empty() && !probe.musicbrainz_recording_id)) continue;
            probe.lookup_strategy = (probe.musicbrainz_release_id || !probe.album.empty())
                ? MediaProbeLookupStrategy::music_release_first
                : MediaProbeLookupStrategy::music_recording_first;
            int score = 760 + (!probe.artist.empty() ? 35 : 0) + (!probe.album.empty() ? 35 : 0) +
                        (probe.track ? 20 : 0) + (probe.disc ? 10 : 0);
            out.push_back({std::move(probe), score, std::string(name()),
                           {"embedded tags supplemented by structured path evidence"}});
        }
        return out;
    }
};

std::vector<std::unique_ptr<MediaProbeCandidateGenerator>> default_candidate_generators() {
    std::vector<std::unique_ptr<MediaProbeCandidateGenerator>> generators;
    generators.push_back(std::make_unique<FilenameEpisodeCandidateGenerator>());
    generators.push_back(std::make_unique<YearlessFilenameEpisodeCandidateGenerator>());
    generators.push_back(std::make_unique<DirectoryEpisodeCandidateGenerator>());
    generators.push_back(std::make_unique<SemanticMovieCandidateGenerator>());
    generators.push_back(std::make_unique<CompactMovieTitleCandidateGenerator>());
    generators.push_back(std::make_unique<LegacyMovieCandidateGenerator>());
    generators.push_back(std::make_unique<EmbeddedMusicTagsCandidateGenerator>());
    generators.push_back(std::make_unique<RecordingFirstMusicTagsCandidateGenerator>());
    generators.push_back(std::make_unique<TagsWithStructuredPathMusicCandidateGenerator>());
    generators.push_back(std::make_unique<RecordingFirstStructuredMusicCandidateGenerator>());
    generators.push_back(std::make_unique<StructuredMusicCandidateGenerator>());
    return generators;
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

// Keeps Retry-After when it is given in seconds.
size_t curl_header(char* buffer, size_t size, size_t nitems, void* opaque) {
    auto* retry_after = static_cast<std::optional<std::chrono::milliseconds>*>(opaque);
    const auto bytes = size * nitems;
    std::string_view line(buffer, bytes);
    constexpr std::string_view name = "retry-after:";
    if (line.size() > name.size()) {
        bool match = true;
        for (size_t i = 0; i < name.size() && match; ++i)
            match = std::tolower(static_cast<unsigned char>(line[i])) == name[i];
        if (match) {
            auto value = line.substr(name.size());
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            uint64_t seconds = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (ec == std::errc{} && end != value.data() && seconds <= 86400)
                *retry_after = std::chrono::seconds(seconds);
        }
    }
    return bytes;
}

int curl_cancelled(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* stop = static_cast<std::atomic_bool*>(opaque);
    return stop && stop->load(std::memory_order_relaxed) ? 1 : 0;
}

std::string item_id(std::string_view provider, std::string_view kind, std::string_view id) {
    return std::string(provider) + ":" + std::string(kind) + ":" + std::string(id);
}

void scanner_marker(CatalogueItem& item) { item.external_ids["macha_scanner"] = "1"; }

void add_art(std::vector<RemoteArtwork>& art, const std::string& item, std::string role,
             std::string url) {
    if (!url.empty()) art.push_back({item, std::move(role), std::move(url)});
}

int title_similarity(std::string_view wanted, std::string_view candidate) {
    const auto left = comparable_title(wanted);
    const auto right = comparable_title(candidate);
    if (left.empty() || right.empty()) return 0;
    if (left == right) return 120;

    std::set<std::string> left_tokens;
    std::set<std::string> right_tokens;
    std::istringstream left_in(left);
    std::istringstream right_in(right);
    for (std::string token; left_in >> token;) left_tokens.insert(std::move(token));
    for (std::string token; right_in >> token;) right_tokens.insert(std::move(token));
    size_t intersection = 0;
    for (const auto& token : left_tokens)
        if (right_tokens.contains(token)) ++intersection;
    if (!intersection) return 0;
    const auto denominator = left_tokens.size() + right_tokens.size();
    int score = static_cast<int>((200 * intersection) / denominator);
    if (left.find(right) != std::string::npos || right.find(left) != std::string::npos)
        score += 15;
    return std::min(score, 115);
}

const Json* best_result(const Json& root, std::string_view title, std::string_view title_key,
                        const std::optional<int32_t>& year, std::string_view date_key,
                        bool allow_adjacent_year = false) {
    auto results = root.find("results");
    if (!results || !results->isArray() || results->asArray().empty()) return nullptr;
    const Json* best = nullptr;
    int best_score = -1000;
    size_t rank = 0;
    for (const auto& candidate : results->asArray()) {
        if (!candidate.isObject()) {
            ++rank;
            continue;
        }
        int score = title_similarity(title, json_string(candidate.find(title_key)));
        score += std::max(0, 18 - static_cast<int>(rank) * 3);
        if (year) {
            auto found_year = json_year(candidate.find(date_key));
            if (found_year && *found_year == *year) {
                score += 45;
                // Exact-year agreement on a high-ranked result supports aliases
                // and translations whose titles share little with the filename.
                if (rank == 0) score += 30;
            } else if (allow_adjacent_year && found_year &&
                       std::abs(*found_year - *year) == 1) {
                score += 20;
            } else if (found_year) score -= 35;
        }
        if (score > best_score) {
            best_score = score;
            best = &candidate;
        }
        ++rank;
    }
    const int minimum_score = year ? 90 : 82;
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

std::string discogs_artist_name(const Json& release) {
    auto artists = release.find("artists");
    if (!artists || !artists->isArray() || artists->asArray().empty())
        return json_string(release.find("artists_sort"));
    std::string out;
    for (const auto& artist : artists->asArray()) {
        if (!artist.isObject()) continue;
        auto name = json_string(artist.find("name"));
        // Drop Discogs disambiguators such as "Artist (2)".
        static const std::regex suffix(R"(\s+\([0-9]+\)$)");
        name = std::regex_replace(name, suffix, "");
        if (name.empty()) continue;
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

std::optional<int32_t> discogs_track_number(std::string_view position) {
    size_t begin = 0;
    while (begin < position.size() && !std::isdigit(static_cast<unsigned char>(position[begin]))) ++begin;
    if (begin == position.size()) return {};
    size_t end = begin;
    while (end < position.size() && std::isdigit(static_cast<unsigned char>(position[end]))) ++end;
    int32_t value{};
    auto [ptr, ec] = std::from_chars(position.data() + begin, position.data() + end, value);
    if (ec != std::errc{} || ptr != position.data() + end) return {};
    return value;
}

std::string discogs_image_url(const Json& release) {
    auto images = release.find("images");
    if (!images || !images->isArray()) return {};
    const Json* fallback = nullptr;
    for (const auto& image : images->asArray()) {
        if (!image.isObject()) continue;
        if (!fallback) fallback = &image;
        if (lower(json_string(image.find("type"))) != "primary") continue;
        auto uri = json_string(image.find("uri"));
        if (uri.empty()) uri = json_string(image.find("resource_url"));
        if (!uri.empty()) return uri;
    }
    if (!fallback) return {};
    auto uri = json_string(fallback->find("uri"));
    if (uri.empty()) uri = json_string(fallback->find("resource_url"));
    return uri;
}

std::pair<std::string, std::string> discogs_result_title(std::string_view value) {
    const auto split = value.find(" - ");
    if (split == std::string_view::npos) return {std::string(value), {}};
    return {std::string(value.substr(0, split)), std::string(value.substr(split + 3))};
}


class ProviderBudgetExhausted final : public std::runtime_error {
  public:
    ProviderBudgetExhausted() : std::runtime_error("catalogue provider request budget exhausted") {}
};

class ProviderTemporarilyUnavailable final : public std::runtime_error {
    std::string provider_;
    std::chrono::milliseconds retry_after_;

  public:
    ProviderTemporarilyUnavailable(
        std::string provider, std::string message,
        std::chrono::milliseconds retry_after = std::chrono::seconds(60))
        : std::runtime_error(std::move(message)), provider_(std::move(provider)),
          retry_after_(retry_after) {}
    const std::string& provider() const noexcept { return provider_; }
    std::chrono::milliseconds retry_after() const noexcept { return retry_after_; }
};

// The provider has no record under the id asked for.
class ProviderRecordNotFound final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

bool transient_provider_status(long status) {
    return status == 429 || status >= 500;
}

class BudgetHttpClient final : public HttpClient {
    HttpClient& upstream_;
    size_t limit_{};
    size_t used_{};

  public:
    explicit BudgetHttpClient(HttpClient& upstream) : upstream_(upstream) {}

    void reset_budget(size_t limit) noexcept {
        limit_ = limit;
        used_ = 0;
    }
    size_t used() const noexcept { return used_; }
    bool exhausted() const noexcept { return used_ >= limit_; }

    void request_stop() noexcept override { upstream_.request_stop(); }
    void reset_stop() noexcept override { upstream_.reset_stop(); }
    bool stop_requested() const noexcept override { return upstream_.stop_requested(); }

    RemoteHttpResponse get(std::string_view url, const std::vector<std::string>& headers,
                           size_t maximum_bytes) override {
        if (exhausted()) throw ProviderBudgetExhausted();
        ++used_;
        return upstream_.get(url, headers, maximum_bytes);
    }
};

} // namespace

std::vector<MediaProbeCandidate> probe_media_candidates(const MediaProbeContext& context) {
    if (context.entry.type != EntryType::file || context.entry.size == 0) return {};
    static const auto generators = default_candidate_generators();
    std::vector<MediaProbeCandidate> candidates;
    for (const auto& generator : generators) {
        auto generated = generator->generate(context);
        for (auto& candidate : generated) {
            bool valid = false;
            switch (candidate.probe.kind) {
                case MediaProbeKind::movie:
                    valid = !candidate.probe.title.empty();
                    break;
                case MediaProbeKind::episode:
                    valid = !candidate.probe.series.empty() && candidate.probe.season &&
                            candidate.probe.episode;
                    break;
                case MediaProbeKind::track:
                    valid = !candidate.probe.title.empty();
                    break;
            }
            if (!valid) continue;
            auto duplicate = std::find_if(candidates.begin(), candidates.end(), [&](const auto& existing) {
                return same_probe_identity(existing.probe, candidate.probe);
            });
            if (duplicate == candidates.end()) {
                candidates.push_back(std::move(candidate));
            } else {
                duplicate->evidence.insert(duplicate->evidence.end(),
                                           candidate.evidence.begin(), candidate.evidence.end());
                if (candidate.score > duplicate->score) {
                    duplicate->score = candidate.score;
                    duplicate->generator = std::move(candidate.generator);
                }
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.generator < b.generator;
    });
    return candidates;
}

std::vector<MediaProbeCandidate> probe_media_candidates(std::string_view path,
                                                        const FsEntry& entry,
                                                        std::string_view root) {
    return probe_media_candidates(MediaProbeContext{root, path, entry, nullptr});
}

std::vector<MediaProbeCandidate> probe_host_media_candidates(const std::filesystem::path& path,
                                                             uint64_t size) {
    FsEntry entry;
    entry.type = EntryType::file;
    entry.size = size;

    std::optional<MediaProbe> embedded;
    const auto ext = lower(path.extension().string());
    if (audio_extension(ext))
        embedded = embedded_music_metadata_from_host(path);
    return probe_media_candidates(MediaProbeContext{{}, path.generic_string(), entry,
                                                     embedded ? &*embedded : nullptr});
}

std::optional<MediaProbe> probe_media_path(std::string_view path, const FsEntry& entry) {
    auto candidates = probe_media_candidates(path, entry);
    if (candidates.empty()) return {};
    auto probe = std::move(candidates.front().probe);
    if (probe.kind == MediaProbeKind::track &&
        (probe.artist.empty() || probe.album.empty() || probe.title.empty()))
        return {};
    return probe;
}

FrameType catalogue_media_profile_frame_type() noexcept {
    // Optional enrichment: yields to viewer and loader traffic.
    return FrameType::speculative;
}

CurlHttpClient::CurlHttpClient() {
    static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
    if (initialized != CURLE_OK) throw std::runtime_error("curl_global_init failed");
}
CurlHttpClient::~CurlHttpClient() = default;

RemoteHttpResponse CurlHttpClient::get(std::string_view url, const std::vector<std::string>& headers,
                                 size_t maximum_bytes) {
    if (stop_requested_.load(std::memory_order_relaxed))
        throw std::runtime_error("HTTP GET cancelled");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    RemoteHttpResponse out;
    std::pair<Bytes*, size_t> sink{&out.body, maximum_bytes};
    const auto owned_url = std::string(url);
    curl_easy_setopt(curl.get(), CURLOPT_URL, owned_url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    const auto user_agent = std::string("Macha/") + std::string(kServerVersion) + " (https://github.com/tomdionysus/macha)";
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, curl_cancelled);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &stop_requested_);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &out.retry_after);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_guard(
        nullptr, curl_slist_free_all);
    for (const auto& header : headers) {
        auto* appended = curl_slist_append(header_guard.get(), header.c_str());
        if (!appended)
            throw std::bad_alloc();
        // curl_slist_append returns the possibly new head owning the chain.
        (void)header_guard.release();
        header_guard.reset(appended);
    }
    if (header_guard)
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_guard.get());
    auto rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && stop_requested_.load(std::memory_order_relaxed))
            throw std::runtime_error("HTTP GET cancelled");
        throw std::runtime_error(std::string("HTTP GET failed: ") + curl_easy_strerror(rc));
    }
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

std::optional<Json> TmdbProvider::api_optional(
    std::string_view path,
    const std::vector<std::pair<std::string, std::string>>& query) {
    auto response = http_.get(query_url("https://api.themoviedb.org/3" + std::string(path), query),
                              {"Authorization: Bearer " + token_, "Accept: application/json"});
    if (response.status == 404) return {};
    if (response.status != 200)
        throw std::runtime_error("TMDB returned HTTP " + std::to_string(response.status));
    return Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
}

std::string TmdbProvider::image_url(std::string_view path) const {
    if (path.empty()) return {};
    return "https://image.tmdb.org/t/p/" + config_.image_size + std::string(path);
}

std::optional<Json> TmdbProvider::find_show(const MediaProbe& probe) {
    if (probe.tmdb_id) {
        const auto key = "id:" + *probe.tmdb_id;
        if (auto it = show_cache_.find(key); it != show_cache_.end()) return it->second;
        auto show = api_optional("/tv/" + *probe.tmdb_id, {{"language", config_.language}});
        provider_cache_store(show_cache_, cache_bytes_, key, show);
        return show;
    }
    const auto key = normalized(probe.series) + "|" + (probe.year ? std::to_string(*probe.year) : "");
    if (auto it = show_cache_.find(key); it != show_cache_.end()) return it->second;
    std::vector<std::pair<std::string, std::string>> q{{"query", probe.series}, {"language", config_.language}};
    // No first_air_date_year filter: libraries often use a pilot or production
    // year one off TMDB's, so the year is scored locally and +/-1 still counts.
    auto root = api("/search/tv", q);
    const auto* result = best_result(root, probe.series, "name", probe.year,
                                     "first_air_date", true);
    if (!result) {
        provider_cache_store(show_cache_, cache_bytes_, key, std::optional<Json>{});
        return {};
    }
    provider_cache_store(show_cache_, cache_bytes_, key, std::optional<Json>{*result});
    return *result;
}

std::optional<ProviderMatch> TmdbProvider::lookup(const MediaProbe& probe) {
    if (!supports(probe.kind)) return {};
    ProviderMatch match;
    if (probe.kind == MediaProbeKind::movie) {
        const auto key = probe.tmdb_id
            ? "id:" + *probe.tmdb_id
            : normalized(probe.title) + "|" + (probe.year ? std::to_string(*probe.year) : "");
        std::optional<Json> cached_detail;
        if (auto it = movie_cache_.find(key); it != movie_cache_.end()) {
            cached_detail = it->second;
        } else if (probe.tmdb_id) {
            cached_detail = api_optional("/movie/" + *probe.tmdb_id, {{"language", config_.language}});
            provider_cache_store(movie_cache_, cache_bytes_, key, cached_detail);
        } else {
            std::vector<std::pair<std::string, std::string>> q{
                {"query", probe.title}, {"language", config_.language}};
            if (probe.year) q.emplace_back("primary_release_year", std::to_string(*probe.year));
            auto search = api("/search/movie", q);
            auto found = best_result(search, probe.title, "title", probe.year, "release_date");
            if (!found) {
                provider_cache_store(movie_cache_, cache_bytes_, key, std::optional<Json>{});
                return {};
            }
            auto id_value = json_i32(found->find("id"));
            if (!id_value) {
                provider_cache_store(movie_cache_, cache_bytes_, key, std::optional<Json>{});
                return {};
            }
            const auto tmdb_id = std::to_string(*id_value);
            cached_detail = api("/movie/" + tmdb_id, {{"language", config_.language}});
            provider_cache_store(movie_cache_, cache_bytes_, key, cached_detail);
        }
        if (!cached_detail) return {};
        const auto& detail = *cached_detail;
        auto detail_id = json_i32(detail.find("id"));
        if (!detail_id) return {};
        const auto tmdb_id = std::to_string(*detail_id);
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

    auto load_season = [&](int32_t season_number) -> std::optional<Json> {
        const auto season_key = series_id + "|" + std::to_string(season_number);
        if (auto it = season_cache_.find(season_key); it != season_cache_.end())
            return it->second;
        auto season = api_optional("/tv/" + series_id + "/season/" + std::to_string(season_number),
                                   {{"language", config_.language}});
        provider_cache_store(season_cache_, cache_bytes_, season_key, season);
        return season;
    };

    auto numbered_episode = [](const Json& season, int32_t wanted) -> const Json* {
        auto episodes = season.find("episodes");
        if (!episodes || !episodes->isArray()) return nullptr;
        for (const auto& episode : episodes->asArray()) {
            auto number = json_i32(episode.find("episode_number"));
            if (number && *number == wanted) return &episode;
        }
        return nullptr;
    };

    auto title_episode = [&](const Json& season, std::string_view wanted,
                             int minimum_score = 90) -> const Json* {
        if (wanted.empty()) return nullptr;
        auto episodes = season.find("episodes");
        if (!episodes || !episodes->isArray()) return nullptr;
        const Json* best = nullptr;
        int best_score = -1;
        for (const auto& episode : episodes->asArray()) {
            const auto score = title_similarity(wanted, json_string(episode.find("name")));
            if (score > best_score) {
                best_score = score;
                best = &episode;
            }
        }
        return best_score >= minimum_score ? best : nullptr;
    };

    auto standalone_special = [&]() -> std::optional<ProviderMatch> {
        if (*probe.season != 0 || probe.title.empty()) return {};
        MediaProbe movie_probe = probe;
        movie_probe.kind = MediaProbeKind::movie;
        movie_probe.title = clean_title(probe.series + " " + probe.title);
        movie_probe.year.reset();
        movie_probe.edition.reset();
        movie_probe.series.clear();
        movie_probe.season.reset();
        movie_probe.episode.reset();
        movie_probe.episode_end.reset();
        return lookup(movie_probe);
    };

    int32_t resolved_season_number = *probe.season;
    auto season_json = load_season(resolved_season_number);

    // Libraries file a pilot under Specials where TMDB may model it as its own
    // one-season show: with no season zero, try the episode in season one.
    if (!season_json && resolved_season_number == 0 && probe.year &&
        json_year(show_json->find("first_air_date")) == probe.year &&
        comparable_title(json_string(show_json->find("name"))) == comparable_title(probe.series)) {
        auto season_one = load_season(1);
        if (season_one && numbered_episode(*season_one, *probe.episode)) {
            season_json = std::move(season_one);
            resolved_season_number = 1;
        }
    }

    if (!season_json) {
        if (auto movie = standalone_special()) return movie;
        return {};
    }

    const Json* episode_json = numbered_episode(*season_json, *probe.episode);
    bool remapped_by_title = false;
    int episode_title_score = 0;
    if (episode_json && !probe.title.empty()) {
        const auto remote_title = json_string(episode_json->find("name"));
        if (!remote_title.empty()) episode_title_score = title_similarity(probe.title, remote_title);
    }

    // Episode numbers identify once series and year are known. Without a year
    // the title corroborates; differing numbering is remapped by a strong title
    // match within the resolved season.
    const bool weak_episode_title = episode_json && !probe.title.empty() && episode_title_score < 75;
    if (!episode_json || weak_episode_title) {
        if (const auto* by_title = title_episode(*season_json, probe.title)) {
            episode_json = by_title;
            remapped_by_title = true;
            episode_title_score = title_similarity(probe.title,
                                                    json_string(episode_json->find("name")));
        } else if (!episode_json || !probe.year || resolved_season_number == 0) {
            if (auto movie = standalone_special()) return movie;
            return {};
        }
    }

    if (!episode_json) {
        if (auto movie = standalone_special()) return movie;
        return {};
    }

    std::vector<const Json*> episode_jsons{episode_json};
    if (probe.episode_end && *probe.episode_end > *probe.episode && !remapped_by_title) {
        episode_jsons.clear();
        for (int32_t number = *probe.episode; number <= *probe.episode_end; ++number) {
            const auto* ranged = numbered_episode(*season_json, number);
            if (!ranged) {
                if (auto movie = standalone_special()) return movie;
                return {};
            }
            episode_jsons.push_back(ranged);
        }
    }

    const auto remote_episode_title = json_string(episode_json->find("name"));
    Log::debug("catalogue: tmdb tv match path=" + probe.path +
               " local_series=\"" + probe.series + "\" remote_series=\"" +
               json_string(show_json->find("name")) + "\" remote_year=" +
               std::to_string(json_year(show_json->find("first_air_date")).value_or(0)) +
               " season=" + std::to_string(resolved_season_number) +
               " episode=" + std::to_string(json_i32(episode_json->find("episode_number")).value_or(*probe.episode)) +
               (probe.episode_end ? " episode_end=" + std::to_string(*probe.episode_end) : std::string{}) +
               " local_episode=\"" + probe.title + "\" remote_episode=\"" +
               remote_episode_title + "\" episode_title_score=" +
               std::to_string(episode_title_score) +
               (remapped_by_title ? " remapped_by_title=1" : ""));

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
    season.id = item_id("tmdb", "season",
                        series_id + ":" + std::to_string(resolved_season_number));
    season.kind = CatalogueKind::season;
    season.title = json_string(season_json->find("name"));
    if (season.title.empty()) season.title = "Season " + std::to_string(resolved_season_number);
    season.sort_title = season.title;
    season.synopsis = json_string(season_json->find("overview"));
    season.parent_id = show.id;
    season.season_number = resolved_season_number;
    season.external_ids["tmdb"] = json_i32(season_json->find("id"))
        ? std::to_string(*json_i32(season_json->find("id"))) : season.id;
    scanner_marker(season);

    match.items = {show, season};
    add_art(match.artwork, show.id, "poster", image_url(json_string(show_json->find("poster_path"))));
    add_art(match.artwork, show.id, "backdrop", image_url(json_string(show_json->find("backdrop_path"))));
    add_art(match.artwork, season.id, "poster", image_url(json_string(season_json->find("poster_path"))));

    for (const auto* remote_episode : episode_jsons) {
        const auto remote_number = json_i32(remote_episode->find("episode_number")).value_or(*probe.episode);
        CatalogueItem episode;
        auto eid = json_i32(remote_episode->find("id"));
        episode.id = item_id("tmdb", "episode", eid ? std::to_string(*eid)
            : series_id + ":" + std::to_string(resolved_season_number) + ":" +
              std::to_string(remote_number));
        episode.kind = CatalogueKind::episode;
        episode.title = json_string(remote_episode->find("name"));
        if (episode.title.empty())
            episode.title = probe.title.empty() ? "Episode " + std::to_string(remote_number)
                                                : probe.title;
        episode.sort_title = episode.title;
        episode.synopsis = json_string(remote_episode->find("overview"));
        episode.parent_id = season.id;
        episode.season_number = resolved_season_number;
        episode.episode_number = remote_number;
        if (eid) episode.external_ids["tmdb"] = std::to_string(*eid);
        episode.media_ids = {probe.media_id};
        scanner_marker(episode);
        match.items.push_back(episode);
        add_art(match.artwork, episode.id, "still",
                image_url(json_string(remote_episode->find("still_path"))));
    }
    return match;
}

std::vector<ProviderSearchResult> TmdbProvider::search(const ProviderSearchQuery& query) {
    const bool movie = query.kind == "movie";
    std::vector<std::pair<std::string, std::string>> q{{"query", query.text},
                                                       {"language", config_.language}};
    if (query.year)
        q.emplace_back(movie ? "primary_release_year" : "first_air_date_year",
                       std::to_string(*query.year));
    auto root = api(movie ? "/search/movie" : "/search/tv", q);
    std::vector<ProviderSearchResult> out;
    const auto* results = root.find("results");
    if (!results || !results->isArray()) return out;
    for (const auto& result : results->asArray()) {
        if (out.size() >= query.limit) break;
        const auto id = json_i32(result.find("id"));
        if (!id) continue;
        ProviderSearchResult found;
        found.ref = item_id("tmdb", movie ? "movie" : "tv", std::to_string(*id));
        found.provider = "tmdb";
        found.kind = query.kind;
        found.title = json_string(result.find(movie ? "title" : "name"));
        found.year = json_year(result.find(movie ? "release_date" : "first_air_date"));
        found.overview = json_string(result.find("overview"));
        found.catalogue_id = found.ref;
        out.push_back(std::move(found));
    }
    return out;
}

std::vector<ArtworkOption> TmdbProvider::artwork_options(std::string_view kind,
                                                         std::string_view id,
                                                         std::string_view role,
                                                         const ProviderRefNumbers& numbers) {
    auto path = "/" + std::string(kind) + "/" + std::string(id);
    if (kind == "tv" && numbers.season) {
        path += "/season/" + std::to_string(*numbers.season);
        if (numbers.episode) path += "/episode/" + std::to_string(*numbers.episode);
    }
    auto root = api_optional(path + "/images");
    if (!root) throw ProviderRecordNotFound("TMDB has no record at " + path);
    const auto* images = root->find(role == "poster" ? "posters"
                                    : role == "backdrop" ? "backdrops" : "stills");
    std::vector<ArtworkOption> out;
    if (!images || !images->isArray()) return out;
    for (const auto& image : images->asArray()) {
        const auto file = json_string(image.find("file_path"));
        if (file.empty()) continue;
        ArtworkOption option;
        option.option_id = file;
        option.role = std::string(role);
        option.width = json_i32(image.find("width"));
        option.height = json_i32(image.find("height"));
        option.language = json_string(image.find("iso_639_1"));
        option.preview_url = "https://image.tmdb.org/t/p/w185" + file;
        option.url = image_url(file);
        out.push_back(std::move(option));
    }
    return out;
}

MusicBrainzProvider::MusicBrainzProvider(HttpClient& http, CatalogueMusicBrainzConfig config,
                                         std::shared_ptr<MusicBrainzGate> gate, bool interactive)
    : http_(http), config_(std::move(config)),
      gate_(gate ? std::move(gate) : std::make_shared<MusicBrainzGate>()),
      interactive_(interactive) {}

bool MusicBrainzProvider::supports(MediaProbeKind kind) const {
    return config_.enabled && kind == MediaProbeKind::track;
}

Json MusicBrainzProvider::api(std::string_view path,
                              const std::vector<std::pair<std::string, std::string>>& query) {
    using std::chrono::steady_clock;
    const auto milliseconds = [](steady_clock::duration value) {
        return std::chrono::ceil<std::chrono::milliseconds>(value);
    };
    // Sleeps until `until`, a slice at a time, so a stop is noticed.
    const auto wait_until = [&](steady_clock::time_point until) {
        while (steady_clock::now() < until) {
            if (http_.stop_requested())
                throw std::runtime_error("MusicBrainz request cancelled");
            std::this_thread::sleep_for(
                std::min<steady_clock::duration>(until - steady_clock::now(),
                                                 std::chrono::milliseconds(50)));
        }
    };
    Lock gate(gate_->mutex);
    // The gate backs off after a failure; the editor waits out a short one.
    const auto backing_off = [&]() MACHA_REQUIRES(gate_->mutex) {
        const auto left = gate_->unavailable_until - steady_clock::now();
        if (left <= steady_clock::duration::zero())
            return;
        if (!interactive_ || left > interactive_wait_max)
            throw ProviderTemporarilyUnavailable(
                "musicbrainz", "MusicBrainz is backing off after a failure", milliseconds(left));
        wait_until(gate_->unavailable_until);
    };
    const auto failed = [&](std::optional<std::chrono::milliseconds> retry_after)
                            MACHA_REQUIRES(gate_->mutex) {
        gate_->backoff = gate_->backoff == steady_clock::duration::zero()
                             ? steady_clock::duration(MusicBrainzGate::backoff_first)
                             : std::min<steady_clock::duration>(gate_->backoff * 2,
                                                                MusicBrainzGate::backoff_max);
        const steady_clock::duration wait =
            retry_after ? steady_clock::duration(*retry_after) : gate_->backoff;
        gate_->unavailable_until = steady_clock::now() + wait;
        return milliseconds(wait);
    };

    auto ua = std::string("Macha/") + std::string(kServerVersion) + " (" + config_.contact + ")";
    auto q = query;
    q.emplace_back("fmt", "json");
    const auto url = query_url("https://musicbrainz.org/ws/2" + std::string(path), q);
    for (int attempt = 0;; ++attempt) {
        backing_off();
        if (gate_->last_request != steady_clock::time_point{})
            wait_until(gate_->last_request + gate_->interval);
        // The editor tries once more after a failure it can wait out.
        const bool again = interactive_ && attempt == 0;
        RemoteHttpResponse response;
        try {
            response = http_.get(url, {"Accept: application/json", "User-Agent: " + ua});
        } catch (const ProviderBudgetExhausted&) {
            throw;
        } catch (const std::exception& e) {
            gate_->last_request = steady_clock::now();
            const auto wait = failed({});
            if (again && wait <= interactive_wait_max)
                continue;
            throw ProviderTemporarilyUnavailable(
                "musicbrainz", "MusicBrainz transport unavailable: " + std::string(e.what()), wait);
        }
        gate_->last_request = steady_clock::now();
        if (transient_provider_status(response.status)) {
            const auto wait = failed(response.retry_after);
            if (again && wait <= interactive_wait_max)
                continue;
            throw ProviderTemporarilyUnavailable(
                "musicbrainz", "MusicBrainz returned HTTP " + std::to_string(response.status), wait);
        }
        gate_->backoff = {};
        if (response.status == 404)
            throw ProviderRecordNotFound("MusicBrainz has no record at " + std::string(path));
        if (response.status != 200)
            throw std::runtime_error("MusicBrainz returned HTTP " + std::to_string(response.status));
        return Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()),
                                            response.body.size()));
    }
}

std::optional<Json> MusicBrainzProvider::release_by_id(std::string_view release_id) {
    if (release_id.empty()) return {};
    const auto key = std::string(release_id);
    if (auto it = release_id_cache_.find(key); it != release_id_cache_.end()) return it->second;
    auto detail = api("/release/" + key,
                      {{"inc", "recordings+artist-credits+release-groups"}});
    provider_cache_store(release_id_cache_, cache_bytes_, key, detail);
    return detail;
}

std::optional<Json> MusicBrainzProvider::find_release(const MediaProbe& probe) {
    if (probe.musicbrainz_release_id) return release_by_id(*probe.musicbrainz_release_id);
    if (probe.artist.empty() || probe.album.empty()) return {};
    auto key = normalized(probe.artist) + "|" + normalized(probe.album);
    if (auto it = release_cache_.find(key); it != release_cache_.end()) return it->second;
    auto query = "release:\"" + lucene_quote(probe.album) + "\" AND artist:\"" +
                 lucene_quote(probe.artist) + "\"";
    auto search = api("/release", {{"query", query}, {"limit", "10"}});
    auto releases = search.find("releases");
    if (!releases || !releases->isArray() || releases->asArray().empty()) {
        provider_cache_store(release_cache_, cache_bytes_, key, std::optional<Json>{});
        return {};
    }
    const Json* best = &releases->asArray().front();
    int best_score = -1;
    for (const auto& candidate : releases->asArray()) {
        int score = 0;
        if (normalized(json_string(candidate.find("title"))) == normalized(probe.album)) score += 100;
        if (normalized(artist_credit_name(candidate.find("artist-credit"))) == normalized(probe.artist)) score += 80;
        if (auto provider_score = json_i32(candidate.find("score"))) score += *provider_score / 10;
        if (score > best_score) { best_score = score; best = &candidate; }
    }
    if (best_score < 100) {
        provider_cache_store(release_cache_, cache_bytes_, key, std::optional<Json>{});
        return {};
    }
    auto release_id = json_string(best->find("id"));
    if (release_id.empty()) {
        provider_cache_store(release_cache_, cache_bytes_, key, std::optional<Json>{});
        return {};
    }
    auto detail = release_by_id(release_id);
    provider_cache_store(release_cache_, cache_bytes_, key, detail);
    return detail;
}

// Filename decorations the provider title lacks ("(feat. X)", "(Live)",
// "(Radio Edit)"), which make a search find nothing. The decorated form is
// tried first: a live or remix version is a distinct recording.
std::string music_title_without_decorations(const std::string& title) {
    static const std::regex decoration(
        R"(\s*[(\[]\s*(?:feat\.?|ft\.?|featuring|with)\b[^)\]]*[)\]]|)"
        R"(\s*[(\[][^)\]]*\b(?:live|acoustic|instrumental|explicit|clean|remaster(?:ed)?|)"
        R"(mono|stereo|radio edit|single version|album version|extended|bonus track[s]?|)"
        R"(deluxe|demo|reprise|version)\b[^)\]]*[)\]])",
        std::regex::icase);
    auto stripped = std::regex_replace(title, decoration, "");
    // Collapse the whitespace the removal leaves behind.
    static const std::regex spaces(R"(\s{2,})");
    stripped = std::regex_replace(stripped, spaces, " ");
    while (!stripped.empty() && (stripped.back() == ' ' || stripped.back() == '-'))
        stripped.pop_back();
    while (!stripped.empty() && stripped.front() == ' ')
        stripped.erase(stripped.begin());
    return stripped;
}

// MusicBrainz credits guests in the artist ("Avicii feat. Sandro Cavazza");
// compare on the primary artist the path carries.
std::string primary_artist_credit(const std::string& credit) {
    static const std::regex secondary(R"(\s+(?:feat\.?|ft\.?|featuring|with|&|vs\.?|x)\s+.*$)",
                                      std::regex::icase);
    return std::regex_replace(credit, secondary, "");
}

std::optional<Json> MusicBrainzProvider::find_recording(const MediaProbe& probe) {
    std::string key;
    std::string recording_id;
    if (probe.musicbrainz_recording_id) {
        recording_id = *probe.musicbrainz_recording_id;
        key = "id:" + recording_id;
    } else {
        if (probe.artist.empty() || probe.title.empty()) return {};
        key = normalized(probe.artist) + "|" + normalized(probe.title);
    }
    if (auto it = recording_cache_.find(key); it != recording_cache_.end()) return it->second;

    const auto undecorated = music_title_without_decorations(probe.title);
    if (recording_id.empty()) {
        auto search_for = [&](const std::string& title) {
            auto query = "recording:\"" + lucene_quote(title) + "\" AND artist:\"" +
                         lucene_quote(probe.artist) + "\"";
            return api("/recording", {{"query", query}, {"limit", "10"}});
        };
        auto search = search_for(probe.title);
        auto recordings = search.find("recordings");
        // The undecorated title is tried only when the decorated finds nothing.
        if ((!recordings || !recordings->isArray() || recordings->asArray().empty()) &&
            !undecorated.empty() && normalized(undecorated) != normalized(probe.title)) {
            search = search_for(undecorated);
            recordings = search.find("recordings");
        }
        if (!recordings || !recordings->isArray() || recordings->asArray().empty()) {
            provider_cache_store(recording_cache_, cache_bytes_, key, std::optional<Json>{});
            return {};
        }
        const Json* best = &recordings->asArray().front();
        int best_score = -1;
        for (const auto& candidate : recordings->asArray()) {
            int score = 0;
            const auto candidate_title = normalized(json_string(candidate.find("title")));
            if (candidate_title == normalized(probe.title)) score += 100;
            // Weaker evidence than an exact match, which wins when both exist.
            else if (!undecorated.empty() && candidate_title == normalized(undecorated)) score += 85;
            const auto credit = artist_credit_name(candidate.find("artist-credit"));
            if (normalized(credit) == normalized(probe.artist)) score += 80;
            else if (normalized(primary_artist_credit(credit)) == normalized(probe.artist)) score += 70;
            if (auto provider_score = json_i32(candidate.find("score"))) score += *provider_score / 10;
            if (score > best_score) { best_score = score; best = &candidate; }
        }
        if (best_score < 120) {
            provider_cache_store(recording_cache_, cache_bytes_, key, std::optional<Json>{});
            return {};
        }
        recording_id = json_string(best->find("id"));
        if (recording_id.empty()) {
            provider_cache_store(recording_cache_, cache_bytes_, key, std::optional<Json>{});
            return {};
        }
    }

    auto detail = api("/recording/" + recording_id,
                      {{"inc", "artist-credits+releases"}});
    provider_cache_store(recording_cache_, cache_bytes_, key, detail);
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
    provider_cache_store(cover_cache_, cache_bytes_, key, result);
    return result;
}

std::vector<ProviderSearchResult> MusicBrainzProvider::search(const ProviderSearchQuery& query) {
    auto lucene = "release:\"" + lucene_quote(query.text) + "\"";
    if (!query.artist.empty()) lucene += " AND artist:\"" + lucene_quote(query.artist) + "\"";
    if (query.year) lucene += " AND date:" + std::to_string(*query.year);
    auto root = api("/release", {{"query", lucene}, {"limit", std::to_string(query.limit)}});
    std::vector<ProviderSearchResult> out;
    const auto* releases = root.find("releases");
    if (!releases || !releases->isArray()) return out;
    for (const auto& release : releases->asArray()) {
        if (out.size() >= query.limit) break;
        const auto id = json_string(release.find("id"));
        if (id.empty()) continue;
        std::string group;
        if (auto found_group = release.find("release-group"); found_group && found_group->isObject())
            group = json_string(found_group->find("id"));
        ProviderSearchResult found;
        found.ref = item_id("musicbrainz", "release", id);
        found.provider = "musicbrainz";
        found.kind = "album";
        found.title = json_string(release.find("title"));
        found.year = json_year(release.find("date"));
        found.artist = artist_credit_name(release.find("artist-credit"));
        found.catalogue_id = item_id("musicbrainz", "album", group.empty() ? id : group);
        out.push_back(std::move(found));
    }
    return out;
}

std::vector<ArtworkOption> MusicBrainzProvider::artwork_options(std::string_view,
                                                                std::string_view id,
                                                                std::string_view role,
                                                                const ProviderRefNumbers&) {
    auto response = http_.get("https://coverartarchive.org/release/" + std::string(id),
                              {"Accept: application/json"}, 2 * 1024 * 1024);
    // The Cover Art Archive answers 404 for a release with no art.
    if (response.status == 404) return {};
    if (response.status != 200)
        throw std::runtime_error("Cover Art Archive returned HTTP " + std::to_string(response.status));
    auto root = Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()),
                                             response.body.size()));
    std::vector<ArtworkOption> out;
    const auto* images = root.find("images");
    if (!images || !images->isArray()) return out;
    for (const auto& image : images->asArray()) {
        bool front = false;
        if (auto flag = image.find("front"); flag && flag->isBool()) front = flag->asBool();
        if (auto types = image.find("types"); types && types->isArray())
            for (const auto& type : types->asArray())
                if (type.isString() && type.asString() == "Front") front = true;
        if (!front) continue;
        const auto* image_id = image.find("id");
        if (!image_id || image_id->isNull()) continue;
        ArtworkOption option;
        option.option_id = image_id->isString() ? image_id->asString() : image_id->dump();
        option.role = std::string(role);
        const auto* thumbs = image.find("thumbnails");
        auto thumb = [&](std::string_view size) {
            return thumbs && thumbs->isObject() ? json_string(thumbs->find(std::string(size)))
                                                : std::string{};
        };
        option.preview_url = thumb("250");
        if (option.preview_url.empty()) option.preview_url = thumb("small");
        option.url = thumb(config_.cover_size);
        if (option.url.empty()) option.url = json_string(image.find("image"));
        if (option.url.empty()) continue;
        out.push_back(std::move(option));
    }
    return out;
}

namespace {

// Visits each track of a release's media in the release's own order, until
// `visit(medium, track)` returns true.
template <typename Visit>
void walk_release_tracks(const Json& release, Visit&& visit) {
    const auto* media = release.find("media");
    if (!media || !media->isArray()) return;
    for (const auto& medium : media->asArray()) {
        const auto* tracks = medium.find("tracks");
        if (!tracks || !tracks->isArray()) continue;
        for (const auto& track : tracks->asArray())
            if (visit(medium, track)) return;
    }
}

std::optional<int64_t> json_length_ms(const Json* value) {
    if (!value || !value->isNumber() || value->asInt64() < 0) return {};
    return value->asInt64();
}

} // namespace

std::vector<ProviderReleaseTrack> MusicBrainzProvider::release_tracks(std::string_view id) {
    const auto release = release_by_id(id);
    std::vector<ProviderReleaseTrack> out;
    if (!release) return out;
    walk_release_tracks(*release, [&](const Json& medium, const Json& release_track) {
        ProviderReleaseTrack track;
        track.disc_number = json_i32(medium.find("position"));
        track.track_number = json_i32(release_track.find("position"));
        track.title = json_string(release_track.find("title"));
        track.length_ms = json_length_ms(release_track.find("length"));
        if (const auto* recording = release_track.find("recording");
            recording && recording->isObject()) {
            track.recording_id = json_string(recording->find("id"));
            if (!track.length_ms) track.length_ms = json_length_ms(recording->find("length"));
        }
        out.push_back(std::move(track));
        return false;
    });
    return out;
}

std::optional<ProviderMatch> MusicBrainzProvider::lookup(const MediaProbe& probe) {
    if (!supports(probe.kind)) return {};

    std::optional<Json> recording_detail;
    std::optional<Json> release;
    auto resolve_recording_first = [&] {
        recording_detail = find_recording(probe);
        if (!recording_detail) return;
        std::string release_id;
        int best_release_score = -1;
        if (auto releases = recording_detail->find("releases"); releases && releases->isArray()) {
            for (const auto& candidate : releases->asArray()) {
                if (!candidate.isObject()) continue;
                const auto candidate_id = json_string(candidate.find("id"));
                if (candidate_id.empty()) continue;
                int score = release_id.empty() ? 1 : 0;
                if (!probe.album.empty())
                    score += title_similarity(probe.album, json_string(candidate.find("title")));
                if (score > best_release_score) {
                    best_release_score = score;
                    release_id = candidate_id;
                }
            }
        }
        if (!release_id.empty()) release = release_by_id(release_id);
    };

    switch (probe.lookup_strategy) {
        case MediaProbeLookupStrategy::music_recording_first:
            resolve_recording_first();
            break;
        case MediaProbeLookupStrategy::music_release_first:
            release = find_release(probe);
            break;
        case MediaProbeLookupStrategy::automatic:
            if (probe.musicbrainz_release_id || (!probe.artist.empty() && !probe.album.empty()))
                release = find_release(probe);
            else
                resolve_recording_first();
            break;
    }
    if (!release) return {};

    const auto release_id = json_string(release->find("id"));
    if (release_id.empty()) return {};
    const auto credited = artist_credit_name(release->find("artist-credit"));
    std::string artist_id = probe.musicbrainz_artist_id.value_or(std::string{});
    std::string artist_name = credited.empty() ? probe.artist : credited;
    if (auto credit = release->find("artist-credit");
        credit && credit->isArray() && !credit->asArray().empty()) {
        auto artist = credit->asArray().front().find("artist");
        if (artist && artist->isObject()) {
            auto canonical_id = json_string(artist->find("id"));
            if (!canonical_id.empty()) artist_id = canonical_id;
            auto canonical = json_string(artist->find("name"));
            if (!canonical.empty()) artist_name = canonical;
        }
    }
    if (artist_name.empty() && recording_detail)
        artist_name = artist_credit_name(recording_detail->find("artist-credit"));
    if (artist_id.empty() && recording_detail) {
        if (auto credit = recording_detail->find("artist-credit");
            credit && credit->isArray() && !credit->asArray().empty()) {
            auto artist = credit->asArray().front().find("artist");
            if (artist && artist->isObject()) artist_id = json_string(artist->find("id"));
        }
    }
    if (artist_name.empty()) return {};
    if (artist_id.empty()) artist_id = normalized(artist_name);

    const Json* recording = nullptr;
    int position = 0;
    const auto wanted_recording_id = probe.musicbrainz_recording_id
        ? *probe.musicbrainz_recording_id
        : recording_detail ? json_string(recording_detail->find("id")) : std::string{};
    // A medium whose matching track carries no recording is left for the next.
    const Json* exhausted_medium = nullptr;
    walk_release_tracks(*release, [&](const Json& medium, const Json& release_track) {
        if (&medium == exhausted_medium) return false;
        if (probe.disc) {
            auto medium_position = json_i32(medium.find("position"));
            if (medium_position && *medium_position != *probe.disc) return false;
        }
        auto number = json_i32(release_track.find("position"));
        const auto title = json_string(release_track.find("title"));
        auto candidate_recording = release_track.find("recording");
        const auto candidate_id = candidate_recording && candidate_recording->isObject()
            ? json_string(candidate_recording->find("id")) : std::string{};
        const bool id_match = !wanted_recording_id.empty() && candidate_id == wanted_recording_id;
        const bool number_match = probe.track && number && *probe.track == *number;
        const bool title_match = !probe.title.empty() && normalized(title) == normalized(probe.title);
        if (!id_match && !number_match && !title_match) return false;
        recording = candidate_recording;
        position = number.value_or(probe.track.value_or(0));
        if (!recording) exhausted_medium = &medium;
        return recording != nullptr;
    });
    if ((!recording || !recording->isObject()) && recording_detail) recording = &*recording_detail;
    if (!recording || !recording->isObject()) return {};
    auto recording_id = json_string(recording->find("id"));
    if (recording_id.empty()) recording_id = wanted_recording_id;
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
    if (album.title.empty()) return {};
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
    if (track.title.empty()) return {};
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


DiscogsProvider::DiscogsProvider(HttpClient& http, CatalogueDiscogsConfig config)
    : http_(http), config_(std::move(config)), token_(read_secret(config_.token_file)) {}

bool DiscogsProvider::supports(MediaProbeKind kind) const {
    return config_.enabled && !token_.empty() && kind == MediaProbeKind::track;
}

Json DiscogsProvider::api(std::string_view path,
                          const std::vector<std::pair<std::string, std::string>>& query) {
    const auto now = std::chrono::steady_clock::now();
    if (unavailable_until_ > now)
        throw ProviderTemporarilyUnavailable("discogs", "Discogs is backing off after a failure");

    // Discogs allows 60 requests/minute; paced here as well as by the scan budget.
    if (last_request_ != std::chrono::steady_clock::time_point{}) {
        const auto due = last_request_ + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < due) {
            if (http_.stop_requested())
                throw std::runtime_error("Discogs request cancelled");
            const auto remaining = due - std::chrono::steady_clock::now();
            const auto slice = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::min(remaining, slice));
        }
    }

    RemoteHttpResponse response;
    const auto user_agent = std::string("Macha/") + std::string(kServerVersion);
    try {
        response = http_.get(query_url("https://api.discogs.com" + std::string(path), query),
                             {"Authorization: Discogs token=" + token_,
                              "Accept: application/json",
                              "User-Agent: " + user_agent});
    } catch (const ProviderBudgetExhausted&) {
        throw;
    } catch (const std::exception& e) {
        unavailable_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        throw ProviderTemporarilyUnavailable(
            "discogs", "Discogs transport unavailable: " + std::string(e.what()));
    }
    last_request_ = std::chrono::steady_clock::now();
    if (transient_provider_status(response.status)) {
        unavailable_until_ = last_request_ + std::chrono::seconds(60);
        throw ProviderTemporarilyUnavailable(
            "discogs", "Discogs returned HTTP " + std::to_string(response.status));
    }
    if (response.status != 200)
        throw std::runtime_error("Discogs returned HTTP " + std::to_string(response.status));
    return Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()),
                                        response.body.size()));
}

std::optional<Json> DiscogsProvider::release_by_id(std::string_view release_id) {
    if (release_id.empty()) return {};
    const auto key = std::string(release_id);
    if (auto it = release_cache_.find(key); it != release_cache_.end()) return it->second;
    auto detail = api("/releases/" + key);
    provider_cache_store(release_cache_, cache_bytes_, key, detail);
    return detail;
}

std::optional<Json> DiscogsProvider::find_release(const MediaProbe& probe) {
    if (probe.artist.empty()) return {};
    const auto recording_first =
        probe.lookup_strategy == MediaProbeLookupStrategy::music_recording_first;
    if (recording_first && probe.title.empty()) return {};
    if (!recording_first && probe.album.empty()) return {};

    const auto key = std::string(recording_first ? "track|" : "release|") +
                     normalized(probe.artist) + "|" + normalized(probe.album) + "|" +
                     normalized(probe.title) + "|" +
                     (probe.year ? std::to_string(*probe.year) : "");
    std::optional<Json> selected;
    if (auto it = search_cache_.find(key); it != search_cache_.end()) {
        selected = it->second;
    } else {
        std::vector<std::pair<std::string, std::string>> query{
            {"type", "release"}, {"per_page", "10"}, {"artist", probe.artist}};
        if (recording_first)
            query.emplace_back("track", probe.title);
        else if (!probe.album.empty())
            query.emplace_back("release_title", probe.album);
        // Album and year score locally in a recording-first lookup;
        // hard provider filters would discard compilation and reissue matches.

        auto root = api("/database/search", query);
        const auto* results = root.find("results");
        const Json* best = nullptr;
        int best_score = -1000;
        size_t rank = 0;
        if (results && results->isArray()) {
            for (const auto& candidate : results->asArray()) {
                if (!candidate.isObject()) {
                    ++rank;
                    continue;
                }
                const auto [candidate_artist, candidate_release] =
                    discogs_result_title(json_string(candidate.find("title")));
                int score = recording_first ? 60 : title_similarity(probe.artist, candidate_artist);
                if (!probe.album.empty()) score += title_similarity(probe.album, candidate_release);
                score += std::max(0, 18 - static_cast<int>(rank) * 3);
                if (probe.year) {
                    auto candidate_year = json_i32(candidate.find("year"));
                    if (candidate_year && *candidate_year == *probe.year)
                        score += 30;
                    else if (candidate_year && std::abs(*candidate_year - *probe.year) == 1)
                        score += 10;
                    else if (candidate_year)
                        score -= 20;
                }
                if (score > best_score) {
                    best_score = score;
                    best = &candidate;
                }
                ++rank;
            }
        }
        const int minimum = recording_first
            ? (probe.album.empty() ? 60 : 100)
            : (probe.album.empty() ? 90 : 145);
        if (best && best_score >= minimum) selected = *best;
        provider_cache_store(search_cache_, cache_bytes_, key, selected);
    }
    if (!selected) return {};
    auto id = json_i32(selected->find("id"));
    if (!id) return {};
    return release_by_id(std::to_string(*id));
}

std::optional<ProviderMatch> DiscogsProvider::lookup(const MediaProbe& probe) {
    if (!supports(probe.kind)) return {};
    auto release = find_release(probe);
    if (!release) return {};

    const auto release_id_value = json_i32(release->find("id"));
    if (!release_id_value) return {};
    const auto release_id = std::to_string(*release_id_value);
    const auto release_title = json_string(release->find("title"));
    const auto release_artist = discogs_artist_name(*release);
    if (release_title.empty() || release_artist.empty()) return {};
    const auto recording_first =
        probe.lookup_strategy == MediaProbeLookupStrategy::music_recording_first;
    if (!recording_first && !probe.artist.empty() &&
        title_similarity(probe.artist, release_artist) < 65)
        return {};
    if (!recording_first && !probe.album.empty() &&
        title_similarity(probe.album, release_title) < 55)
        return {};

    const Json* selected_track = nullptr;
    int selected_score = -1;
    size_t selected_index = 0;
    if (auto tracklist = release->find("tracklist"); tracklist && tracklist->isArray()) {
        size_t index = 0;
        for (const auto& entry : tracklist->asArray()) {
            if (!entry.isObject()) {
                ++index;
                continue;
            }
            const auto type = lower(json_string(entry.find("type_")));
            if (!type.empty() && type != "track") {
                ++index;
                continue;
            }
            const auto title = json_string(entry.find("title"));
            const auto position = json_string(entry.find("position"));
            int score = 0;
            if (!probe.title.empty()) score += title_similarity(probe.title, title);
            if (probe.track) {
                auto number = discogs_track_number(position);
                if (number && *number == *probe.track) score += 80;
            }
            if (score > selected_score) {
                selected_score = score;
                selected_track = &entry;
                selected_index = index;
            }
            ++index;
        }
    }
    if (!selected_track) return {};
    const auto selected_title = json_string(selected_track->find("title"));
    const int selected_title_score = probe.title.empty() ? 0 : title_similarity(probe.title, selected_title);
    if (!probe.title.empty() && selected_title_score < 60) return {};
    if (probe.title.empty() && selected_score < 80) return {};
    if (recording_first && !probe.artist.empty()) {
        auto selected_artist = discogs_artist_name(*selected_track);
        if (selected_artist.empty()) selected_artist = release_artist;
        if (title_similarity(probe.artist, selected_artist) < 65) return {};
    }

    std::string artist_id;
    if (auto artists = release->find("artists"); artists && artists->isArray() && !artists->asArray().empty()) {
        if (auto id = json_i32(artists->asArray().front().find("id"))) artist_id = std::to_string(*id);
    }
    if (artist_id.empty()) artist_id = normalized(release_artist);

    const auto master_id = json_i32(release->find("master_id")).value_or(0);
    const auto album_key = master_id > 0 ? "master:" + std::to_string(master_id)
                                         : "release:" + release_id;
    auto position = json_string(selected_track->find("position"));
    if (position.empty()) position = std::to_string(selected_index + 1);

    CatalogueItem artist;
    artist.id = item_id("discogs", "artist", artist_id);
    artist.kind = CatalogueKind::artist;
    artist.title = release_artist;
    artist.sort_title = artist.title;
    artist.external_ids["discogs"] = artist_id;
    scanner_marker(artist);

    CatalogueItem album;
    album.id = item_id("discogs", "album", album_key);
    album.kind = CatalogueKind::album;
    album.title = release_title;
    album.sort_title = album.title;
    album.parent_id = artist.id;
    album.year = json_i32(release->find("year"));
    album.external_ids["discogs_release"] = release_id;
    if (master_id > 0) album.external_ids["discogs_master"] = std::to_string(master_id);
    scanner_marker(album);

    CatalogueItem track;
    track.id = item_id("discogs", "track", release_id + ":" + position);
    track.kind = CatalogueKind::track;
    track.title = selected_title.empty() ? probe.title : selected_title;
    if (track.title.empty()) return {};
    track.sort_title = track.title;
    track.parent_id = album.id;
    track.disc_number = probe.disc;
    track.track_number = discogs_track_number(position).value_or(
        probe.track.value_or(static_cast<int32_t>(selected_index + 1)));
    track.external_ids["discogs_release"] = release_id;
    track.external_ids["discogs_position"] = position;
    track.media_ids = {probe.media_id};
    scanner_marker(track);

    ProviderMatch result;
    result.items = {artist, album, track};
    add_art(result.artwork, album.id, "cover", discogs_image_url(*release));
    return result;
}

MovieScanProvider::MovieScanProvider(HttpClient& http, CatalogueMovieProviderConfig config)
    : roots_(std::move(config.roots)) {
    if (config.tmdb.enabled) {
        try {
            metadata_ = std::make_unique<TmdbProvider>(http, std::move(config.tmdb));
        } catch (const std::exception& e) {
            Log::warn("catalogue movies metadata disabled: " + std::string(e.what()));
        }
    }
}

bool MovieScanProvider::accepts_path(std::string_view path) const noexcept {
    return video_extension(path_extension(path));
}

MediaProbeFile MovieScanProvider::probe_file(
    FileSystem&, std::string_view root, std::string_view path, const FsEntry& entry) {
    auto candidates = probe_media_candidates(path, entry, root);
    std::erase_if(candidates, [](const auto& candidate) {
        return candidate.probe.kind != MediaProbeKind::movie;
    });
    if (candidates.size() > 4) candidates.resize(4);
    return {std::move(candidates), {}};
}

TvScanProvider::TvScanProvider(HttpClient& http, CatalogueTvProviderConfig config)
    : roots_(std::move(config.roots)) {
    if (config.tmdb.enabled) {
        try {
            metadata_ = std::make_unique<TmdbProvider>(http, std::move(config.tmdb));
        } catch (const std::exception& e) {
            Log::warn("catalogue TV metadata disabled: " + std::string(e.what()));
        }
    }
}

bool TvScanProvider::accepts_path(std::string_view path) const noexcept {
    return video_extension(path_extension(path));
}

MediaProbeFile TvScanProvider::probe_file(
    FileSystem&, std::string_view root, std::string_view path, const FsEntry& entry) {
    auto candidates = probe_media_candidates(path, entry, root);
    std::erase_if(candidates, [](const auto& candidate) {
        return candidate.probe.kind != MediaProbeKind::episode;
    });
    if (candidates.size() > 4) candidates.resize(4);
    return {std::move(candidates), {}};
}

MusicScanProvider::MusicScanProvider(HttpClient& http, CatalogueMusicProviderConfig config,
                                     size_t max_artwork_bytes,
                                     std::shared_ptr<MusicBrainzGate> musicbrainz_gate,
                                     bool interactive)
    : roots_(std::move(config.roots)), max_artwork_bytes_(max_artwork_bytes) {
    if (config.musicbrainz.enabled)
        metadata_.push_back(std::make_unique<MusicBrainzProvider>(
            http, std::move(config.musicbrainz), std::move(musicbrainz_gate), interactive));
    if (config.discogs.enabled) {
        try {
            metadata_.push_back(std::make_unique<DiscogsProvider>(http, std::move(config.discogs)));
        } catch (const std::exception& e) {
            Log::warn("catalogue Discogs metadata disabled: " + std::string(e.what()));
        }
    }
}

bool MusicScanProvider::accepts_path(std::string_view path) const noexcept {
    return audio_extension(path_extension(path));
}

MediaProbeFile MusicScanProvider::probe_file(
    FileSystem& fs, std::string_view root, std::string_view path, const FsEntry& entry) {
    auto embedded = read_music_metadata_probe(fs, path, entry, max_artwork_bytes_);
    if (!embedded) return {};
    MediaProbeContext context{root, path, entry, &embedded->probe};
    auto candidates = probe_media_candidates(context);
    std::erase_if(candidates, [](const auto& candidate) {
        return candidate.probe.kind != MediaProbeKind::track;
    });
    if (candidates.size() > 6) candidates.resize(6);
    return {std::move(candidates), std::move(embedded->artwork)};
}


MetadataProvider* MusicScanProvider::metadata(std::string_view provider) noexcept {
    for (const auto& candidate : metadata_)
        if (candidate->name() == provider) return candidate.get();
    return nullptr;
}

std::optional<ProviderMatch> MusicScanProvider::lookup(const MediaProbe& probe) {
    if (metadata_.empty()) return {};
    bool any_reachable = false;
    bool saw_transient = false;
    std::string last_transient;
    size_t provider_index = 0;
    for (const auto& provider : metadata_) {
        if (!provider->supports(probe.kind)) {
            ++provider_index;
            continue;
        }
        try {
            auto match = provider->lookup(probe);
            any_reachable = true;
            if (match) {
                if (provider_index > 0)
                    Log::debug("catalogue: music metadata matched fallback provider=" +
                               std::string(provider->name()) + " path=" + probe.path);
                return match;
            }
        } catch (const ProviderTemporarilyUnavailable& e) {
            saw_transient = true;
            last_transient = e.what();
            Log::debug("catalogue: music metadata provider unavailable provider=" +
                       e.provider() + " path=" + probe.path + " reason=\"" + e.what() + "\"");
        }
        ++provider_index;
    }
    if (saw_transient && !any_reachable)
        throw ProviderTemporarilyUnavailable("music", last_transient.empty()
            ? "music metadata providers temporarily unavailable" : last_transient);
    return {};
}

CatalogueScanner::CatalogueScanner(NodeRuntime& node, MetadataServer& metadata_server, FileSystem& fs,
                                   CatalogueManager& catalogue, CatalogueHintQueue& hints,
                                   CatalogueScannerConfig config,
                                   std::unique_ptr<HttpClient> http,
                                   std::chrono::milliseconds diagnostic_interval,
                                   std::shared_ptr<MediaEngine> profile_engine,
                                   MediaInformationService* media_information)
    : node_(node), metadata_server_(metadata_server), fs_(fs), catalogue_(catalogue), hints_(hints), config_(std::move(config)),
      http_(http ? std::move(http) : std::make_unique<CurlHttpClient>()),
      provider_http_(std::make_unique<BudgetHttpClient>(*http_)),
      profile_engine_(std::move(profile_engine)),
      media_information_(media_information),
      diagnostic_interval_(diagnostic_interval) {
    configure_providers();
}
CatalogueScanner::~CatalogueScanner() { stop(); }

void CatalogueScanner::configure_providers() {
    auto build = [&](HttpClient& http, bool interactive) MACHA_REQUIRES(config_mutex_) {
        std::vector<std::unique_ptr<CatalogueScanProvider>> out;
        if (config_.movies.enabled)
            out.push_back(std::make_unique<MovieScanProvider>(http, config_.movies));
        if (config_.tv.enabled)
            out.push_back(std::make_unique<TvScanProvider>(http, config_.tv));
        if (config_.music.enabled)
            out.push_back(std::make_unique<MusicScanProvider>(
                http, config_.music, config_.max_artwork_bytes, musicbrainz_gate_, interactive));
        return out;
    };
    providers_ = build(*provider_http_, false);
    for (auto& seat : editor_seats_) {
        Lock editor(seat.mutex);
        seat.providers = build(*http_, true);
    }
}

MetadataProvider* CatalogueScanner::editor_metadata(
    const std::vector<std::unique_ptr<CatalogueScanProvider>>& providers,
    std::string_view scan_provider, std::string_view metadata_provider) {
    for (const auto& provider : providers)
        if (provider->name() == scan_provider) return provider->metadata(metadata_provider);
    return nullptr;
}

bool CatalogueScanner::coordinator() const {
    auto active = node_.membership().active();
    auto self = node_.node_id();
    for (const auto& peer : active) if (peer.id < self) return false;
    return true;
}

void CatalogueScanner::start() {
    Lock lock(config_mutex_);
    if (!config_.enabled || worker_.joinable()) return;
    http_->reset_stop();
    hints_.requeue_processing();
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("catalogue-scanner", stop, [this, stop] { loop(stop); });
    });
}
void CatalogueScanner::request_stop() {
    if (worker_.joinable()) worker_.request_stop();
    http_->request_stop();
}
void CatalogueScanner::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
}
void CatalogueScanner::request_rescan() {
    rescan_requested_.store(true, std::memory_order_relaxed);
}

size_t CatalogueScanner::request_media_rescan(const std::vector<std::string>& media_ids) {
    if (media_ids.empty())
        return 0;
    {
        Lock lock(config_mutex_);
        if (!config_.enabled)
            return 0;
    }

    std::set<std::string> wanted(media_ids.begin(), media_ids.end());
    std::vector<CatalogueHintSubmission> submissions;

    // Enqueue only the paths of the unbound media ids, never a full rescan.
    MetadataSnapshotView view;
    const auto media_files = fs_.media_files(view);
    {
        Lock lock(config_mutex_);
        for (const auto& [path, media_id] : media_files) {
            if (!wanted.contains(media_id))
                continue;
            std::string root;
            auto* provider = provider_for_path(path, root);
            if (!provider || !provider->accepts_path(path))
                continue;
            submissions.push_back({path, "manual", media_id,
                                   CatalogueHintPriority::manual_rescan});
        }
    }

    const auto queued = hints_.submit_many(std::move(submissions)).size();
    Log::debug("catalogue metadata clear targeted rematch media_ids=" +
               std::to_string(wanted.size()) + " queued=" + std::to_string(queued));
    return queued;
}

size_t CatalogueScanner::request_media_profiles(const std::vector<std::string>& media_ids) {
    if (media_information_)
        return media_information_->request(media_ids, MediaInformationPriority::requested,
                                           "media-information-request");
    if (media_ids.empty()) return 0;
    {
        Lock lock(config_mutex_);
        if (!config_.enabled || !profile_engine_ || !profile_engine_->status().available)
            return 0;
    }
    std::set<std::string> wanted(media_ids.begin(), media_ids.end());
    auto available = fs_.available_snapshot_view();
    if (!available) return 0;
    const auto existing = hints_.list();
    std::map<std::string, CatalogueHint, std::less<>> existing_by_path;
    for (const auto& hint : existing)
        existing_by_path.emplace(hint.path, hint);
    std::vector<CatalogueHintSubmission> submissions;
    size_t pending = 0;
    auto pending_nodes = fs_.namespace_nodes();
    {
        Lock lock(config_mutex_);
        for_each_namespace_entry(*available->snapshot, &pending_nodes,
                                 [&](const std::string& path, const FsEntry& entry)
                                     MACHA_REQUIRES(config_mutex_) {
            if (entry.type != EntryType::file || !entry.size) return;
            const auto media_id = file_media_id(entry);
            if (!wanted.contains(media_id)) return;
            std::string root;
            auto* provider = provider_for_path(path, root);
            if (!provider || !provider->accepts_path(path)) return;

            // One background job per media id: a live job counts as pending; a
            // terminal one returns zero so playback uses its engine fallback. A
            // changed media id is a new origin.
            if (auto it = existing_by_path.find(path); it != existing_by_path.end()) {
                const auto& hint = it->second;
                const bool same_profile_request = std::any_of(
                    hint.origins.begin(), hint.origins.end(), [&](const auto& origin) {
                        return origin.source == "media-profile" && origin.source_ref == media_id;
                    });
                if (same_profile_request) {
                    if (hint.state == CatalogueHintState::queued ||
                        hint.state == CatalogueHintState::processing ||
                        hint.state == CatalogueHintState::deferred)
                        ++pending;
                    return;
                }
            }
            submissions.push_back({path, "media-profile", media_id,
                                   CatalogueHintPriority::periodic_scan});
        });
    }
    const auto queued = hints_.submit_many(std::move(submissions)).size();
    pending += queued;
    Log::debug("catalogue media profile requested media_ids=" +
               std::to_string(wanted.size()) + " queued=" + std::to_string(queued) +
               " pending=" + std::to_string(pending));
    return pending;
}


std::vector<MediaProbeCandidate> CatalogueScanner::probe_unmatched(std::string_view hint_id) {
    auto hint = hints_.get(hint_id);
    if (!hint || hint->state != CatalogueHintState::no_match || hint->media_id.empty())
        return {};

    Lock lock(config_mutex_);
    std::string root;
    auto* provider = provider_for_path(hint->path, root);
    if (!provider || !provider->accepts_path(hint->path)) return {};

    auto entry = fs_.getattr(hint->path);
    if (entry.type != EntryType::file || entry.size == 0 || file_media_id(entry) != hint->media_id)
        return {};
    return provider->probe_file(fs_, root, hint->path, entry).candidates;
}

void CatalogueScanner::reconfigure(CatalogueScannerConfig config) {
    stop();
    {
        Lock lock(config_mutex_);
        config_ = std::move(config);
        configure_providers();
    }
    start();
}

std::vector<std::pair<std::string, FsEntry>> catalogue_snapshot_files(
    std::string_view root, const MetadataSnapshot& namespace_snapshot,
    const NamespaceNodeStore* namespace_nodes, std::stop_token stop) {
    const auto normalized = normalize_path(std::string(root));
    // A stat-only read: only the root's type is checked.
    const auto root_entry = namespace_entry(namespace_snapshot, namespace_nodes, normalized, false);
    if (!root_entry)
        throw FsError(ENOENT, "missing");
    if (root_entry->type != EntryType::directory)
        throw FsError(ENOTDIR, "catalogue root is not a directory");

    // A destructive pass reads one namespace generation, so a mutation between
    // directories cannot fake an absence.
    std::vector<std::pair<std::string, FsEntry>> out;
    const auto prefix = normalized == "/" ? std::string("/") : normalized + "/";
    // A prefix query descends only the subtree. Entries are whole, as
    // file_media_id hashes the extent list.
    for_each_namespace_entry_with_prefix(namespace_snapshot, namespace_nodes, prefix,
                                        [&](const std::string& path, const FsEntry& entry) {
        if (stop.stop_requested())
            return;
        if (entry.type == EntryType::file)
            out.emplace_back(path, entry);
    });
    return out;
}

namespace {
// Whether a word (ASCII alphanumeric run) of `relative` equals one of `terms`,
// ignoring case.
bool path_has_ignored_term(std::string_view relative, const std::vector<std::string>& terms) {
    if (terms.empty()) return false;
    const auto equal_ignoring_case = [](std::string_view word, std::string_view term) {
        return word.size() == term.size() &&
               std::equal(word.begin(), word.end(), term.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };
    size_t start = 0;
    for (size_t i = 0; i <= relative.size(); ++i) {
        if (i < relative.size() && std::isalnum(static_cast<unsigned char>(relative[i]))) continue;
        if (i > start) {
            const auto word = relative.substr(start, i - start);
            for (const auto& term : terms)
                if (equal_ignoring_case(word, term)) return true;
        }
        start = i + 1;
    }
    return false;
}
} // namespace

CatalogueScanProvider* CatalogueScanner::provider_for_path(std::string_view path,
                                                           std::string& root) const {
    CatalogueScanProvider* selected = nullptr;
    size_t selected_root_length = 0;
    const auto normalized = normalize_path(std::string(path));
    for (const auto& provider : providers_) {
        for (const auto& candidate_root_value : provider->roots()) {
            const auto candidate_root = normalize_path(candidate_root_value);
            const bool under = normalized == candidate_root ||
                (normalized.size() > candidate_root.size() &&
                 normalized.starts_with(candidate_root) &&
                 normalized[candidate_root.size()] == '/');
            if (!under || candidate_root.size() < selected_root_length) continue;
            selected = provider.get();
            selected_root_length = candidate_root.size();
            root = candidate_root;
        }
    }
    if (selected &&
        path_has_ignored_term(std::string_view(normalized).substr(selected_root_length),
                              config_.ignore_terms))
        return nullptr;
    return selected;
}

ProviderRequestError provider_unavailable(std::string_view provider, std::string_view reason) {
    Log::warn("catalogue provider unavailable provider=" + std::string(provider) + " reason=\"" +
              std::string(reason) + "\"");
    return ProviderRequestError(503, "provider_unavailable", "Provider unavailable");
}

ProviderRequestError provider_unavailable(std::string_view provider, const std::exception& error) {
    auto refused = provider_unavailable(provider, std::string_view(error.what()));
    if (const auto* temporary = dynamic_cast<const ProviderTemporarilyUnavailable*>(&error))
        refused.retry_after = temporary->retry_after();
    return refused;
}

namespace {
void add_artwork(CatalogueItem& item, CatalogueArtwork art) {
    const bool duplicate =
        std::any_of(item.artwork.begin(), item.artwork.end(), [&](const auto& current) {
            return current.role == art.role && current.id == art.id;
        });
    if (!duplicate) item.artwork.push_back(std::move(art));
}
} // namespace

bool CatalogueScanner::stage_remote_artwork(
    ProviderMatch& match, const std::function<bool(std::string_view)>& locked,
    std::stop_token stop, size_t max_artwork_bytes,
    DistributedStore::DurabilityBatch& artwork_batch) {
    struct Fetch {
        const RemoteArtwork* art{};
        CatalogueItem* target{};
        std::optional<RemoteHttpResponse> response;
        std::string error;
    };
    std::vector<Fetch> fetches;
    std::set<std::tuple<std::string, std::string, std::string>> fetched_remote_artwork;
    for (const auto& art : match.artwork) {
        if (!fetched_remote_artwork.emplace(art.item_id, art.role, art.url).second) continue;
        auto target = std::find_if(match.items.begin(), match.items.end(), [&](const auto& item) {
            return item.id == art.item_id;
        });
        if (target == match.items.end()) continue;
        if (locked(art.item_id)) continue;
        // An image this node already stored from the same URL, and still
        // holds, is the same artwork: neither fetched nor written again.
        std::optional<CatalogueArtwork> known;
        {
            Lock lock(remote_artwork_mutex_);
            if (const auto found = remote_artwork_.find(art.url); found != remote_artwork_.end())
                known = found->second;
        }
        if (known && known->role == art.role && catalogue_.artwork_held_here(known->id)) {
            add_artwork(*target, std::move(*known));
            continue;
        }
        fetches.push_back({&art, &*target, {}, {}});
    }

    // The images come down side by side; each is staged in the order asked.
    constexpr size_t side_by_side = 4;
    std::atomic_size_t next{};
    const auto fetch = [&] {
        for (auto i = next.fetch_add(1); i < fetches.size(); i = next.fetch_add(1)) {
            if (stop.stop_requested()) return;
            try {
                fetches[i].response = http_->get(fetches[i].art->url, {}, max_artwork_bytes);
            } catch (const std::exception& e) {
                fetches[i].error = e.what();
            }
        }
    };
    {
        std::vector<std::jthread> others;
        for (size_t i = 1; i < std::min(side_by_side, fetches.size()); ++i)
            others.emplace_back(fetch);
        fetch();
    }
    if (stop.stop_requested()) return false;

    for (auto& fetched : fetches) {
        const auto& art = *fetched.art;
        if (!fetched.response) {
            Log::warn("catalogue artwork failed for " + art.item_id + ": " + fetched.error);
            continue;
        }
        auto& response = *fetched.response;
        if (response.status != 200 || response.body.empty()) continue;
        auto mime = response.content_type;
        if (auto semi = mime.find(';'); semi != std::string::npos) mime.resize(semi);
        if (!mime.starts_with("image/")) continue;
        try {
            auto staged = catalogue_.stage_artwork_deferred(
                art.role, mime, response.body, artwork_batch);
            {
                Lock lock(remote_artwork_mutex_);
                if (remote_artwork_.size() >= remote_artwork_max)
                    remote_artwork_.clear();
                remote_artwork_.insert_or_assign(art.url, staged);
            }
            add_artwork(*fetched.target, std::move(staged));
        } catch (const std::exception& e) {
            Log::warn("catalogue artwork failed for " + art.item_id + ": " + e.what());
        }
    }
    return true;
}

namespace {

struct ProviderRef {
    std::string provider;
    std::string kind;
    std::string id;
};

std::optional<ProviderRef> parse_provider_ref(std::string_view ref) {
    const auto first = ref.find(':');
    if (first == std::string_view::npos) return {};
    const auto second = ref.find(':', first + 1);
    if (second == std::string_view::npos) return {};
    ProviderRef out{std::string(ref.substr(0, first)),
                    std::string(ref.substr(first + 1, second - first - 1)),
                    std::string(ref.substr(second + 1))};
    const auto digits = [](std::string_view value) {
        return !value.empty() && value.size() <= 12 &&
               std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    const auto mbid = [](std::string_view value) {
        if (value.size() != 36) return false;
        for (size_t i = 0; i < value.size(); ++i) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? value[i] != '-' : !std::isxdigit(static_cast<unsigned char>(value[i])))
                return false;
        }
        return true;
    };
    if (out.provider == "tmdb" && (out.kind == "movie" || out.kind == "tv") && digits(out.id))
        return out;
    if (out.provider == "musicbrainz" && out.kind == "release" && mbid(out.id)) return out;
    return {};
}

} // namespace

std::vector<ProviderSearchResult> CatalogueScanner::search_providers(
    const ProviderSearchQuery& query) {
    struct Source {
        std::string_view kind;
        std::string_view scan_provider;
        std::string_view metadata_provider;
        MediaProbeKind probe_kind;
    };
    static constexpr Source sources[] = {
        {"movie", "movies", "tmdb", MediaProbeKind::movie},
        {"show", "tv", "tmdb", MediaProbeKind::episode},
        {"album", "music", "musicbrainz", MediaProbeKind::track},
    };
    const auto source = std::find_if(std::begin(sources), std::end(sources),
                                     [&](const Source& candidate) { return candidate.kind == query.kind; });
    if (source == std::end(sources))
        throw ProviderRequestError(400, "bad_kind", "kind must be movie, show or album");
    const auto scan_provider = source->scan_provider;
    const auto metadata_provider = source->metadata_provider;
    const auto kind = source->probe_kind;
    std::vector<ProviderSearchResult> results;
    {
        auto& seat = editor_seat(std::string(query.kind) + ":" + query.text);
        Lock editor(seat.mutex);
        auto* metadata = editor_metadata(seat.providers, scan_provider, metadata_provider);
        if (!metadata || !metadata->supports(kind))
            throw ProviderRequestError(400, "provider_not_configured",
                                       std::string(metadata_provider) + " is not configured for " +
                                           std::string(scan_provider));
        try {
            results = metadata->search(query);
        } catch (const std::exception& e) {
            throw provider_unavailable(metadata_provider, e);
        }
    }
    for (auto& result : results)
        if (!catalogue_.get(result.catalogue_id)) result.catalogue_id.clear();
    return results;
}

ProviderRefMatch CatalogueScanner::match_unmatched_ref(std::string_view hint_id,
                                                       std::string_view ref,
                                                       const ProviderRefNumbers& numbers) {
    auto hint = hints_.get(hint_id);
    if (!hint || hint->media_id.empty())
        throw ProviderRequestError(404, "not_found", "unmatched file not found");
    const auto parsed = parse_provider_ref(ref);
    if (!parsed)
        throw ProviderRequestError(400, "bad_ref",
                                   "ref must be tmdb:movie:<id>, tmdb:tv:<id> or "
                                   "musicbrainz:release:<mbid>");

    MediaProbe probe;
    probe.path = hint->path;
    probe.media_id = hint->media_id;
    std::string_view scan_provider;
    if (parsed->kind == "movie") {
        probe.kind = MediaProbeKind::movie;
        probe.tmdb_id = parsed->id;
        scan_provider = "movies";
    } else if (parsed->kind == "tv") {
        if (!numbers.season || !numbers.episode)
            throw ProviderRequestError(400, "not_playable_ref",
                                       "a show needs season_number and episode_number");
        probe.kind = MediaProbeKind::episode;
        probe.tmdb_id = parsed->id;
        probe.season = numbers.season;
        probe.episode = numbers.episode;
        scan_provider = "tv";
    } else {
        if (!numbers.track)
            throw ProviderRequestError(400, "not_playable_ref", "a release needs track_number");
        probe.kind = MediaProbeKind::track;
        probe.musicbrainz_release_id = parsed->id;
        probe.track = numbers.track;
        probe.disc = numbers.disc;
        probe.lookup_strategy = MediaProbeLookupStrategy::music_release_first;
        scan_provider = "music";
    }

    size_t max_artwork_bytes = 0;
    {
        Lock lock(config_mutex_);
        max_artwork_bytes = config_.max_artwork_bytes;
    }
    // Where a match's time goes, for its debug line.
    auto stage_started = Clock::now();
    const auto stage_ms = [&] {
        const auto now = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - stage_started).count();
        stage_started = now;
        return std::to_string(ms);
    };
    std::optional<ProviderMatch> match;
    {
        auto& seat = editor_seat(parsed->provider + ":" + parsed->id);
        Lock editor(seat.mutex);
        auto* metadata = editor_metadata(seat.providers, scan_provider, parsed->provider);
        if (!metadata || !metadata->supports(probe.kind))
            throw ProviderRequestError(400, "provider_not_configured",
                                       parsed->provider + " is not configured for " +
                                           std::string(scan_provider));
        try {
            match = metadata->lookup(probe);
        } catch (const ProviderRecordNotFound&) {
            // No match: answered below as provider_not_found.
        } catch (const std::exception& e) {
            throw provider_unavailable(parsed->provider, e);
        }
    }
    if (!match)
        throw ProviderRequestError(404, "provider_not_found",
                                   "the provider has no such record, or no episode or track "
                                   "with those numbers");

    const auto leaf = std::find_if(match->items.begin(), match->items.end(), [&](const auto& item) {
        return std::find(item.media_ids.begin(), item.media_ids.end(), probe.media_id) !=
               item.media_ids.end();
    });
    if (leaf == match->items.end())
        throw ProviderRequestError(404, "provider_not_found", "the record names no playable item");
    const auto leaf_id = leaf->id;

    DistributedStore::DurabilityBatch artwork_batch;
    const auto locked = [&](std::string_view item_id) {
        auto item = catalogue_.get(item_id);
        if (!item) return false;
        const auto lock = item->external_ids.find("macha_metadata_locked");
        return lock != item->external_ids.end() && lock->second == "1";
    };
    const auto lookup_ms = stage_ms();
    (void)stage_remote_artwork(*match, locked, {}, max_artwork_bytes, artwork_batch);
    const auto artwork_ms = stage_ms();
    if (!catalogue_.artwork_durability_barrier(artwork_batch))
        throw CatalogueUnavailable("catalogue artwork durability floor unavailable");
    const auto barrier_ms = stage_ms();
    catalogue_.reconcile_scanner(match->items, {probe.media_id}, false, {}, {});
    if (Log::enabled(LogLevel::debug))
        Log::debug("catalogue match path=" + hint->path + " lookup_ms=" + lookup_ms +
                   " artwork_ms=" + artwork_ms + " images=" +
                   std::to_string(match->artwork.size()) + " barrier_ms=" + barrier_ms +
                   " commit_ms=" + stage_ms());

    ProviderRefMatch out;
    out.leaf_id = leaf_id;
    for (const auto& item : match->items) out.item_ids.push_back(item.id);
    hints_.mark_catalogued(hint->id, parsed->provider, probe.media_id, out.item_ids,
                           "matched_provider_ref");
    if (media_information_ && probe.media_id.starts_with("macha:"))
        (void)media_information_->request({probe.media_id}, MediaInformationPriority::background,
                                          "media-information-catalogue");
    return out;
}

namespace {

std::string_view scan_provider_for(const ProviderRef& ref) {
    if (ref.kind == "movie") return "movies";
    if (ref.kind == "tv") return "tv";
    return "music";
}

// Artwork roles per reference: movie or show poster and backdrop, season
// poster, episode still, release cover.
std::vector<std::string_view> artwork_roles_for(const ProviderRef& ref,
                                                const ProviderRefNumbers& numbers) {
    if (ref.kind == "movie") return {"poster", "backdrop"};
    if (ref.kind == "tv") {
        if (numbers.season && numbers.episode) return {"still"};
        if (numbers.season) return {"poster"};
        return {"poster", "backdrop"};
    }
    return {"cover"};
}

} // namespace

std::vector<ArtworkOption> CatalogueScanner::artwork_options(std::string_view ref,
                                                             std::string_view role,
                                                             const ProviderRefNumbers& numbers) {
    const auto parsed = parse_provider_ref(ref);
    if (!parsed)
        throw ProviderRequestError(400, "bad_ref",
                                   "ref must be tmdb:movie:<id>, tmdb:tv:<id> or "
                                   "musicbrainz:release:<mbid>");
    const auto roles = artwork_roles_for(*parsed, numbers);
    if (std::find(roles.begin(), roles.end(), role) == roles.end()) {
        std::string allowed;
        for (const auto candidate : roles)
            allowed += (allowed.empty() ? "" : ", ") + std::string(candidate);
        throw ProviderRequestError(400, "bad_role", "role must be " + allowed + " for this reference");
    }
    // What the provider last offered for this reference and role is kept for
    // a while: choosing one of the options asks for them again.
    const auto number = [](const std::optional<int32_t>& value) {
        return value ? std::to_string(*value) : std::string("-");
    };
    const auto key = std::string(ref) + "|" + std::string(role) + "|" + number(numbers.season) +
                     "|" + number(numbers.episode);
    {
        Lock lock(artwork_options_mutex_);
        const auto found = artwork_options_.find(key);
        if (found != artwork_options_.end() && Clock::now() < found->second.first)
            return found->second.second;
    }
    const auto scan_provider = scan_provider_for(*parsed);
    auto& seat = editor_seat(parsed->provider + ":" + parsed->id);
    Lock editor(seat.mutex);
    auto* metadata = editor_metadata(seat.providers, scan_provider, parsed->provider);
    if (!metadata)
        throw ProviderRequestError(400, "provider_not_configured",
                                   parsed->provider + " is not configured for " +
                                       std::string(scan_provider));
    try {
        auto options = metadata->artwork_options(parsed->kind, parsed->id, role, numbers);
        Lock lock(artwork_options_mutex_);
        if (artwork_options_.size() >= artwork_options_max)
            artwork_options_.clear();
        artwork_options_[key] = {Clock::now() + artwork_options_kept, options};
        return options;
    } catch (const ProviderRecordNotFound& e) {
        throw ProviderRequestError(404, "provider_not_found", e.what());
    } catch (const std::exception& e) {
        throw provider_unavailable(parsed->provider, e);
    }
}

std::vector<ProviderReleaseTrack> CatalogueScanner::release_tracks(std::string_view provider,
                                                                  std::string_view release_id) {
    const auto parsed =
        parse_provider_ref(std::string(provider) + ":release:" + std::string(release_id));
    if (!parsed) throw ProviderRequestError(400, "bad_ref", "the release id must be an MBID");
    const auto scan_provider = scan_provider_for(*parsed);
    auto& seat = editor_seat(parsed->provider + ":" + parsed->id);
    Lock editor(seat.mutex);
    auto* metadata = editor_metadata(seat.providers, scan_provider, parsed->provider);
    if (!metadata || !metadata->supports(MediaProbeKind::track))
        throw ProviderRequestError(400, "provider_not_configured",
                                   parsed->provider + " is not configured for " +
                                       std::string(scan_provider));
    try {
        return metadata->release_tracks(parsed->id);
    } catch (const ProviderRecordNotFound& e) {
        throw ProviderRequestError(404, "provider_not_found", e.what());
    } catch (const std::exception& e) {
        throw provider_unavailable(parsed->provider, e);
    }
}

CatalogueItem CatalogueScanner::choose_artwork(std::string_view item_id, std::string_view role,
                                               std::string_view option_id,
                                               std::optional<std::string> ref,
                                               ProviderRefNumbers numbers, bool lock) {
    auto item = catalogue_.get(item_id);
    if (!item) throw ProviderRequestError(404, "not_found", "catalogue item not found");

    // An episode or season names its show's record with its own numbers.
    if (item->kind == CatalogueKind::season || item->kind == CatalogueKind::episode) {
        if (!numbers.season) numbers.season = item->season_number;
        if (item->kind == CatalogueKind::episode && !numbers.episode)
            numbers.episode = item->episode_number;
    }
    if (!ref) {
        const auto is_tmdb_show = [](const std::optional<CatalogueItem>& candidate) {
            return candidate && candidate->id.starts_with("tmdb:tv:");
        };
        switch (item->kind) {
            case CatalogueKind::movie:
                if (item->id.starts_with("tmdb:movie:")) ref = item->id;
                break;
            case CatalogueKind::show:
                if (item->id.starts_with("tmdb:tv:")) ref = item->id;
                break;
            case CatalogueKind::season:
                if (item->parent_id) {
                    auto show = catalogue_.get(*item->parent_id);
                    if (is_tmdb_show(show)) ref = show->id;
                }
                break;
            case CatalogueKind::episode:
                if (item->parent_id) {
                    auto season = catalogue_.get(*item->parent_id);
                    if (season && season->parent_id) {
                        auto show = catalogue_.get(*season->parent_id);
                        if (is_tmdb_show(show)) ref = show->id;
                    }
                }
                break;
            case CatalogueKind::album:
                if (auto release = item->external_ids.find("musicbrainz_release");
                    release != item->external_ids.end())
                    ref = "musicbrainz:release:" + release->second;
                break;
            default:
                break;
        }
    }
    if (!ref)
        throw ProviderRequestError(400, "no_provider_ref",
                                   "the item has no provider record; name one with ref");

    const auto options = artwork_options(*ref, role, numbers);
    const auto option = std::find_if(options.begin(), options.end(), [&](const auto& candidate) {
        return candidate.option_id == option_id;
    });
    if (option == options.end())
        throw ProviderRequestError(404, "option_not_found",
                                   "the provider does not list that option for this role");

    size_t max_artwork_bytes = 0;
    {
        Lock config_lock(config_mutex_);
        max_artwork_bytes = config_.max_artwork_bytes;
    }
    RemoteHttpResponse response;
    try {
        response = http_->get(option->url, {}, max_artwork_bytes);
    } catch (const std::exception& e) {
        throw provider_unavailable("artwork", e.what());
    }
    auto mime = response.content_type;
    if (auto semi = mime.find(';'); semi != std::string::npos) mime.resize(semi);
    if (response.status != 200 || response.body.empty() || !mime.starts_with("image/"))
        throw provider_unavailable("artwork",
                                   "the image fetch answered HTTP " + std::to_string(response.status));

    auto art = catalogue_.stage_artwork(std::string(role), mime, response.body);
    std::erase_if(item->artwork, [&](const CatalogueArtwork& existing) {
        return existing.role == art.role;
    });
    item->artwork.push_back(std::move(art));
    if (lock)
        item->external_ids["macha_metadata_locked"] = "1";
    else
        item->external_ids.erase("macha_metadata_locked");
    return catalogue_.upsert(*item, item->revision);
}

std::optional<CatalogueScanner::PreparedHintMatch>
CatalogueScanner::prepare_hint(const CatalogueHint& hint, std::stop_token stop,
                               const MetadataSnapshot& namespace_snapshot,
                               uint64_t snapshot_taken_unix_ms,
                               DistributedStore::DurabilityBatch& artwork_batch) {
    if (stop.stop_requested()) return {};
    CatalogueScannerConfig config;
    std::string root;
    CatalogueScanProvider* provider = nullptr;
    {
        Lock lock(config_mutex_);
        config = config_;
        provider = provider_for_path(hint.path, root);
    }
    if (!provider) {
        hints_.mark_no_match(hint.id, {}, {},
                             path_has_ignored_term(hint.path, config.ignore_terms)
                                 ? "ignored_term"
                                 : "outside_catalogue_roots");
        return {};
    }

    // The batch's snapshot, not getattr(), which may read authoritative
    // metadata per hint.
    const auto path = normalize_path(hint.path);
    auto scan_nodes = fs_.namespace_nodes();
    auto found_entry = namespace_entry(namespace_snapshot, &scan_nodes, path);
    if (!found_entry) {
        // A hint newer than the snapshot may name a file not in it yet: look
        // again next batch.
        if (hint.created_unix_ms >= snapshot_taken_unix_ms) {
            hints_.defer(hint.id, "path_not_yet_visible",
                         "the file is newer than the namespace snapshot this batch read",
                         unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
            return {};
        }
        hints_.fail(hint.id, "path_missing", "namespace path no longer exists");
        return {};
    }
    const auto& entry = *found_entry;
    if (entry.type != EntryType::file) {
        hints_.mark_no_match(hint.id, std::string(provider->name()), {},
                             "not_media_file");
        return {};
    }
    if (entry.size == 0) {
        // A zero-length file may be a shell whose FUSE data is still being
        // published: no media id yet, so not a provider miss. Its content
        // reopens the hint.
        hints_.defer(hint.id, "content_not_committed", "namespace media file has no committed content yet",
                     unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
        return {};
    }

    const auto media_id = file_media_id(entry);
    auto existing = catalogue_.snapshot_view();
    std::vector<std::string> existing_ids;
    for (const auto& [id, item] : existing->items) {
        if (std::find(item.media_ids.begin(), item.media_ids.end(), media_id) != item.media_ids.end())
            existing_ids.push_back(id);
    }

    const bool manual_refresh = std::any_of(
        hint.origins.begin(), hint.origins.end(),
        [](const auto& origin) { return origin.source == "manual"; });

    // The media id derives from the extent manifest: an already bound file has
    // nothing new for a passive retry to find. Manual rescans probe fully.
    if (!existing_ids.empty() && !manual_refresh) {
        return PreparedHintMatch{hint.id, std::string(provider->name()), media_id,
                                 std::move(existing_ids), {},
                                 "already catalogued", hint.attempts, {}};
    }

    auto probed = provider->probe_file(fs_, root, hint.path, entry);
    if (stop.stop_requested()) return {};
    if (probed.candidates.empty()) {
        hints_.mark_no_match(hint.id, std::string(provider->name()), media_id,
                             "no_media_candidate");
        return {};
    }

    // A manual refresh of a bound file may merge embedded artwork; it never
    // needs an online lookup.
    std::optional<CatalogueItem> artwork_target;
    if (!existing_ids.empty()) {
        for (const auto& [id, item] : existing->items) {
            if (std::find(item.media_ids.begin(), item.media_ids.end(), media_id) == item.media_ids.end())
                continue;
            if (!probed.artwork.empty()) {
                if (item.kind == CatalogueKind::track && item.parent_id) {
                    if (auto parent = existing->items.find(*item.parent_id);
                        parent != existing->items.end())
                        artwork_target = parent->second;
                } else {
                    artwork_target = item;
                }
            }
        }
        std::vector<CatalogueItem> updates;
        if (artwork_target) {
            bool changed = false;
            for (const auto& art : probed.artwork) {
                auto staged = catalogue_.stage_artwork_deferred(
                    art.role, art.mime_type, art.bytes, artwork_batch);
                const bool duplicate = std::any_of(
                    artwork_target->artwork.begin(), artwork_target->artwork.end(),
                    [&](const auto& current) {
                        return current.role == staged.role && current.id == staged.id;
                    });
                if (!duplicate) {
                    artwork_target->artwork.push_back(std::move(staged));
                    changed = true;
                }
            }
            if (changed) updates.push_back(std::move(*artwork_target));
        }
        return PreparedHintMatch{hint.id, std::string(provider->name()), media_id,
                                 std::move(existing_ids), std::move(updates),
                                 "already catalogued", hint.attempts, {}};
    }

    constexpr size_t max_candidate_attempts = 5;
    const auto candidate_limit = std::min(max_candidate_attempts, probed.candidates.size());
    if (hint.candidate_cursor >= candidate_limit) {
        std::string result = "no_provider_match";
        hints_.mark_no_match(hint.id, std::string(provider->name()), media_id, std::move(result));
        Log::debug("catalogue hint: no provider match path=" + hint.path +
                   " provider=" + std::string(provider->name()));
        return {};
    }

    // One scheduling turn evaluates one candidate, which may take several
    // requests. The persisted cursor lets the queue yield to another root and
    // resume after a restart without repeating candidates.
    const auto& candidate = probed.candidates[hint.candidate_cursor];
    std::optional<ProviderMatch> selected_match;
    const MediaProbeCandidate* selected_candidate = nullptr;
    try {
        auto match = provider->lookup(candidate.probe);
        if (match) {
            selected_match = std::move(match);
            selected_candidate = &candidate;
        }
    } catch (const ProviderBudgetExhausted&) {
        hints_.defer(hint.id, "provider_budget_exhausted", "provider request budget exhausted",
                     unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
        return {};
    } catch (const ProviderTemporarilyUnavailable& e) {
        // An outage is provider state: defer every hint of the provider, or each
        // would be probed locally only to meet the same backoff.
        const auto retry_delay = std::max(config.provider_batch_delay, e.retry_after());
        const auto retry_at = unix_ms() + static_cast<uint64_t>(retry_delay.count());
        size_t deferred = 0;
        {
            Lock lock(config_mutex_);
            deferred = hints_.defer_matching(
                [&](const CatalogueHint& queued) MACHA_REQUIRES(config_mutex_) {
                    std::string queued_root;
                    return provider_for_path(queued.path, queued_root) == provider;
                },
                "provider_unavailable", e.what(), retry_at);
        }
        Log::warn("catalogue hint provider temporarily unavailable provider=" +
                  std::string(provider->name()) + " path=" + hint.path +
                  " deferred_hints=" + std::to_string(deferred) +
                  " retry_ms=" + std::to_string(retry_delay.count()) + ": " + e.what());
        return {};
    } catch (const std::exception& e) {
        Log::warn("catalogue hint lookup failed provider=" + std::string(provider->name()) +
                  " path=" + hint.path + " candidate=" + candidate.generator + ": " + e.what());
        hints_.record_failure(hint.id, "provider_error", e.what(),
                              unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()),
                              5);
        return {};
    }

    if (!selected_match || !selected_candidate) {
        const auto next_cursor = hint.candidate_cursor + 1;
        if (next_cursor < candidate_limit) {
            hints_.advance_candidate(hint.id, next_cursor);
            return {};
        }
        std::string result = "no_provider_match";
        hints_.mark_no_match(hint.id, std::string(provider->name()), media_id, std::move(result));
        Log::debug("catalogue hint: no provider match path=" + hint.path +
                   " provider=" + std::string(provider->name()));
        return {};
    }

    auto match = std::move(*selected_match);
    const auto& probe = selected_candidate->probe;
    std::string local_artwork_target;
    for (const auto& item : match.items) {
        if (item.kind != CatalogueKind::track) continue;
        if (std::find(item.media_ids.begin(), item.media_ids.end(), probe.media_id) ==
            item.media_ids.end())
            continue;
        local_artwork_target = item.parent_id.value_or(item.id);
        break;
    }
    if (!local_artwork_target.empty() && !probed.artwork.empty()) {
        auto target = std::find_if(match.items.begin(), match.items.end(), [&](const auto& item) {
            return item.id == local_artwork_target;
        });
        if (target != match.items.end()) {
            for (const auto& art : probed.artwork) {
                try {
                    auto staged = catalogue_.stage_artwork_deferred(
                    art.role, art.mime_type, art.bytes, artwork_batch);
                    const bool duplicate = std::any_of(
                        target->artwork.begin(), target->artwork.end(), [&](const auto& current) {
                            return current.role == staged.role && current.id == staged.id;
                        });
                    if (!duplicate) target->artwork.push_back(std::move(staged));
                } catch (const std::exception& e) {
                    Log::warn("catalogue embedded artwork failed for " + hint.path + ": " + e.what());
                }
            }
        }
    }

    const auto locked = [&](std::string_view item_id) {
        auto old = existing->items.find(std::string(item_id));
        if (old == existing->items.end()) return false;
        const auto lock = old->second.external_ids.find("macha_metadata_locked");
        return lock != old->second.external_ids.end() && lock->second == "1";
    };
    if (!stage_remote_artwork(match, locked, stop, config.max_artwork_bytes, artwork_batch))
        return {};

    std::vector<std::string> item_ids;
    item_ids.reserve(match.items.size());
    for (const auto& item : match.items) item_ids.push_back(item.id);
    return PreparedHintMatch{hint.id, std::string(provider->name()), probe.media_id,
                             std::move(item_ids), std::move(match.items),
                             "matched " + selected_candidate->generator, hint.attempts, {}};
}

CatalogueScanner::HintBatchResult
CatalogueScanner::process_hint_batch(std::stop_token stop, size_t max_hints) {
    CatalogueScannerConfig config;
    {
        Lock lock(config_mutex_);
        config = config_;
    }
    auto* budget_http = dynamic_cast<BudgetHttpClient*>(provider_http_.get());
    if (!budget_http) throw std::runtime_error("catalogue provider HTTP budget unavailable");

    HintBatchResult out;
    std::vector<PreparedHintMatch> prepared;
    prepared.reserve(max_hints);
    DistributedStore::DurabilityBatch artwork_batch;

    // One namespace view per batch, taken lazily: an empty queue does no
    // metadata work. Normally a cache read.
    std::optional<MetadataSnapshotView> namespace_view;
    uint64_t namespace_view_taken_unix_ms = 0;

    for (; out.claimed < max_hints && !stop.stop_requested() && !budget_http->exhausted();) {
        auto hint = hints_.claim_next();
        if (!hint) break;
        ++out.claimed;
        try {
            if (!namespace_view) {
                namespace_view_taken_unix_ms = unix_ms();
                namespace_view = fs_.available_snapshot_view();
                if (!namespace_view)
                    namespace_view = fs_.local_snapshot_view();
            }
            if (auto match = prepare_hint(*hint, stop, *namespace_view->snapshot,
                                          namespace_view_taken_unix_ms, artwork_batch)) {
                if (media_information_ && match->media_id.starts_with("macha:")) {
                    (void)media_information_->request(
                        {match->media_id}, MediaInformationPriority::background,
                        "media-information-catalogue");
                } else if (profile_engine_ && profile_engine_->status().available &&
                    match->media_id.starts_with("macha:")) {
                    auto profile_nodes = fs_.namespace_nodes();
                    auto entry = namespace_entry(*namespace_view->snapshot, &profile_nodes,
                                                 normalize_path(hint->path));
                    if (entry && entry->type == EntryType::file && entry->size) {
                        try {
                            const auto media_id = match->media_id;
                            const auto path = normalize_path(hint->path);
                            const auto source_entry = *entry;
                            const auto deadline = Clock::now() +
                                node_.config().streaming.probe_timeout;
                            auto resolved = catalogue_.resolve_media_profile(
                                media_id, deadline, [&] {
                                    MediaSource source{
                                        media_id, path, source_entry.size,
                                        [this, source_entry, path](MediaReadPurpose)
                                            -> std::shared_ptr<MediaInput> {
                                            return std::make_shared<CatalogueMediaInput>(
                                                fs_.open_read(source_entry, path, false,
                                                              catalogue_media_profile_frame_type()),
                                                source_entry.size);
                                        }, {}};
                                    auto remaining = std::max(
                                        std::chrono::milliseconds(1),
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - Clock::now()));
                                    return profile_engine_->probe(source, remaining);
                                });
                            match->media_profile = std::move(resolved.probe);
                        } catch (const std::exception& e) {
                            if (!stop.stop_requested())
                                Log::warn("catalogue media profile deferred media=" +
                                          match->media_id + " path=" + hint->path +
                                          " error=" + e.what());
                        }
                    }
                }
                prepared.push_back(std::move(*match));
            }
        } catch (const CatalogueConflict& e) {
            hints_.defer(hint->id, "catalogue_conflict", e.what(), unix_ms() + 500);
        } catch (const CatalogueUnavailable& e) {
            hints_.defer(hint->id, "catalogue_unavailable", e.what(),
                         unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
        } catch (const std::exception& e) {
            hints_.record_failure(hint->id, "catalogue_error", e.what(),
                                  unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()),
                                  5);
            Log::warn("catalogue hint failed path=" + hint->path + ": " + e.what());
        }
    }
    if (stop.stop_requested()) {
        hints_.requeue_processing();
        return out;
    }
    if (prepared.empty()) return out;

    if (!catalogue_.artwork_durability_barrier(artwork_batch)) {
        const auto retry = unix_ms() +
            static_cast<uint64_t>(config.provider_batch_delay.count());
        for (const auto& match : prepared)
            hints_.defer(match.hint_id, "artwork_durability_unavailable", "catalogue artwork durability floor unavailable", retry);
        Log::debug("catalogue hint batch deferred: artwork durability floor unavailable");
        return out;
    }

    std::vector<CatalogueItem> discovered;
    std::set<std::string> active_media_ids;
    std::map<std::string, MediaProbeResult, std::less<>> media_profiles;
    for (const auto& match : prepared) {
        active_media_ids.insert(match.media_id);
        discovered.insert(discovered.end(), match.items.begin(), match.items.end());
        if (match.media_profile)
            media_profiles[match.media_id] = *match.media_profile;
    }

    try {
        if (!discovered.empty() || !media_profiles.empty())
            catalogue_.reconcile_scanner(discovered, active_media_ids, false, {}, media_profiles);
    } catch (const CatalogueConflict& e) {
        for (const auto& match : prepared)
            hints_.defer(match.hint_id, "catalogue_conflict", e.what(), unix_ms() + 500);
        return out;
    } catch (const CatalogueUnavailable& e) {
        const auto retry = unix_ms() +
            static_cast<uint64_t>(config.provider_batch_delay.count());
        for (const auto& match : prepared)
            hints_.defer(match.hint_id, "catalogue_unavailable", e.what(), retry);
        Log::debug("catalogue hint batch deferred: " + std::string(e.what()));
        return out;
    } catch (const std::exception& e) {
        for (const auto& match : prepared)
            hints_.record_failure(
                match.hint_id, "catalogue_error", e.what(),
                unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()), 5);
        Log::warn("catalogue hint batch reconcile failed: " + std::string(e.what()));
        return out;
    }

    for (auto& match : prepared) {
        hints_.mark_catalogued(match.hint_id, std::move(match.provider), std::move(match.media_id),
                               std::move(match.catalogue_item_ids), std::move(match.result));
        ++out.catalogued;
    }
    if (out.catalogued && max_hints > 1)
        Log::info("catalogue hint batch matched " + std::to_string(out.catalogued) + " media files");
    return out;
}

size_t CatalogueScanner::scan_once() {
    (void)scan_once({}, false, "manual", CatalogueHintPriority::manual_rescan, true);
    CatalogueScannerConfig config;
    {
        Lock lock(config_mutex_);
        config = config_;
    }
    auto* budget_http = dynamic_cast<BudgetHttpClient*>(provider_http_.get());
    if (!budget_http) throw std::runtime_error("catalogue provider HTTP budget unavailable");
    budget_http->reset_budget(config.max_provider_requests_per_scan);
    constexpr size_t max_hint_batch = 64;
    return process_hint_batch({}, max_hint_batch).catalogued;
}

size_t CatalogueScanner::scan_once(std::stop_token stop, bool force,
                                   std::string_view hint_source, int hint_priority,
                                   bool unique_source_ref) {
    CatalogueScannerConfig config;
    std::vector<CatalogueScanProvider*> providers;
    {
        Lock lock(config_mutex_);
        config = config_;
        for (const auto& provider : providers_) providers.push_back(provider.get());
    }
    if (!config.enabled || (!force && !coordinator()) || stop.stop_requested()) return 0;

    struct ProviderFile {
        CatalogueScanProvider* provider{};
        const std::string* path{};
        const std::string* media_id{};
    };
    // Discovery reads the filesystem's media index, which follows each
    // commit: one head, and no walk of the namespace.
    MetadataSnapshotView namespace_view;
    const auto media_files = fs_.media_files(namespace_view);
    const auto& namespace_snapshot = *namespace_view.snapshot;
    std::vector<ProviderFile> files;
    size_t roots_scanned = 0;
    size_t roots_unavailable = 0;
    for (auto* provider : providers) {
        for (const auto& root : provider->roots()) {
            try {
                auto scan_nodes = fs_.namespace_nodes();
                const auto normalized = normalize_path(root);
                const auto root_entry =
                    namespace_entry(namespace_snapshot, &scan_nodes, normalized, false);
                if (!root_entry)
                    throw FsError(ENOENT, "missing");
                if (root_entry->type != EntryType::directory)
                    throw FsError(ENOTDIR, "catalogue root is not a directory");
                if (stop.stop_requested()) return 0;
                ++roots_scanned;
                const auto prefix = normalized == "/" ? normalized : normalized + "/";
                for (auto at = std::lower_bound(media_files.begin(), media_files.end(),
                                                std::pair{prefix, std::string{}});
                     at != media_files.end() && at->first.starts_with(prefix); ++at)
                    files.push_back({provider, &at->first, &at->second});
            } catch (const FsError& e) {
                if (e.code() != ENOENT) throw;
                ++roots_unavailable;
                Log::debug("catalogue scan: provider=" + std::string(provider->name()) +
                           " root unavailable root=" + root + " reason=" + e.what());
            }
        }
    }
    const bool complete_scan = roots_unavailable == 0;
    if (!complete_scan) {
        Log::info("catalogue scan: partial roots_scanned=" + std::to_string(roots_scanned) +
                  " roots_unavailable=" + std::to_string(roots_unavailable) +
                  " files=" + std::to_string(files.size()));
    }

    auto existing = catalogue_.snapshot_view();
    std::set<std::string> bound;
    for (const auto& [_, item] : existing->items)
        bound.insert(item.media_ids.begin(), item.media_ids.end());

    std::set<std::string> active_media_ids;
    std::vector<CatalogueHintSubmission> submissions;
    submissions.reserve(files.size());
    const auto scan_ref = unique_source_ref
        ? std::string(hint_source) + ":" + std::to_string(unix_ms())
        : std::string{};
    for (const auto& file : files) {
        if (stop.stop_requested()) return 0;
        // Zero-length files have no media id yet (often shells awaiting FUSE
        // data) and are not among the media files.
        if (!file.provider->accepts_path(*file.path)) continue;

        // Discovery never opens the file; probing is the hint consumer's.
        const auto& media_id = *file.media_id;
        active_media_ids.insert(media_id);

        // Bound ids are skipped; changed bytes give a new id. Manual rescans
        // queue bound objects too.
        if (!bound.contains(media_id) || force)
            submissions.push_back({*file.path, std::string(hint_source),
                                   unique_source_ref ? scan_ref : media_id,
                                   hint_priority});
    }

    // Manual bindings outside the roots are looked for in the whole namespace,
    // in the same snapshot, and only those found nowhere are dropped.
    std::set<std::string> vanished_media;
    if (complete_scan) {
        for (const auto& [_, item] : existing->items) {
            auto marker = item.external_ids.find("macha_scanner");
            if (marker != item.external_ids.end() && marker->second == "1") continue;
            for (const auto& media : item.media_ids)
                if (media.starts_with("macha:") && !active_media_ids.contains(media))
                    vanished_media.insert(media);
        }
        if (!vanished_media.empty())
            for (const auto& [_, media_id] : media_files)
                vanished_media.erase(media_id);
    }

    const auto ids = hints_.submit_many(std::move(submissions));
    if (stop.stop_requested()) return 0;
    // Discovery is the only destructive source; hint processing is additive.
    catalogue_.reconcile_scanner(
        {}, active_media_ids, complete_scan,
        complete_scan ? std::optional<Hash256>(metadata_namespace_signature(namespace_snapshot))
                      : std::nullopt,
        {}, vanished_media);
    if (!ids.empty())
        Log::info("catalogue scan queued " + std::to_string(ids.size()) + " media hints");
    return ids.size();
}

void CatalogueScanner::loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-catalogue", diagnostic_interval_, true);
    CatalogueScannerConfig initial_config;
    { Lock lock(config_mutex_); initial_config = config_; }
    const auto persisted = load_scanner_state(node_.config().state_path);
    std::optional<Hash256> scanned_namespace = persisted.namespace_signature;
    bool scanner_state_reconciled = !persisted.namespace_signature || persisted.reconciled;
    // Without scanner.state, an existing hints.json shows the library has been
    // scanned: seed the current signature unverified rather than scan at once.
    const bool prior_scan_evidence = std::filesystem::exists(
        node_.config().state_path / "catalogue" / "hints.json");
    std::optional<std::chrono::steady_clock::time_point> mutation_due;
    std::optional<std::chrono::steady_clock::time_point> mutation_first_seen;
    auto observed_generation = metadata_server_.known_generation();
    const auto initial_now = Clock::now();
    auto next_periodic = persisted.next_safety_scan_unix_ms
        ? std::min(steady_due_from_unix_ms(persisted.next_safety_scan_unix_ms),
                   initial_now + initial_config.interval)
        : (prior_scan_evidence ? initial_now + initial_config.interval : initial_now);
    auto next_hint_batch = std::chrono::steady_clock::now();
    auto next_backlog_log = std::chrono::steady_clock::now();
    auto hint_revision = hints_.revision();
    bool provider_budget_open = false;
    bool was_coordinator = false;
    bool initial_signature_checked = false;

    auto namespace_identity = [&]() -> std::pair<Hash256, uint64_t> {
        uint64_t generation = 0;
        if (auto available = fs_.available_namespace_signature(&generation);
            available && generation >= metadata_server_.known_generation())
            return {*available, generation};
        // MetadataManager owns convergence; the strong snapshot path is only
        // for an absent or stale view.
        auto signature = fs_.namespace_signature(&generation);
        return {signature, generation};
    };

    while (!stop.stop_requested()) {
        CatalogueScannerConfig config;
        { Lock lock(config_mutex_); config = config_; }
        const auto now = std::chrono::steady_clock::now();

        // On start, compare the last reconciled namespace signature with the
        // decoded view; becoming coordinator is no reason for a full pass.
        // Without scanner.state but with hints.json, seed it unverified and
        // reconcile at the safety interval.
        if (!initial_signature_checked) {
            uint64_t available_generation = 0;
            if (auto current = fs_.available_namespace_signature(&available_generation);
                current && available_generation >= metadata_server_.known_generation()) {
                // Never from a view known stale; metadata convergence catches up.
                initial_signature_checked = true;
                observed_generation = std::max(observed_generation, available_generation);
                if (scanned_namespace) {
                    if (*current != *scanned_namespace) {
                        mutation_first_seen = now;
                        mutation_due = now + config.rescan_debounce;
                    }
                } else if (prior_scan_evidence) {
                    scanned_namespace = *current;
                    scanner_state_reconciled = false;
                    const auto next_due = next_scan_due_unix_ms(config.interval);
                    next_periodic = steady_due_from_unix_ms(next_due);
                    try {
                        persist_scanner_state(node_.config().state_path, *current, next_due, false);
                    } catch (const std::exception& e) {
                        Log::debug("catalogue scanner state persistence unavailable: " +
                                   std::string(e.what()));
                    }
                }
            }
        }

        // Every node consumes its own hints; catalogue CAS resolves concurrent
        // additive updates. Only destructive reconciliation is the coordinator's.
        if (config.enabled && now >= next_hint_batch) {
            auto* budget_http = dynamic_cast<BudgetHttpClient*>(provider_http_.get());
            if (!budget_http) throw std::runtime_error("catalogue provider HTTP budget unavailable");
            if (!provider_budget_open) {
                budget_http->reset_budget(config.max_provider_requests_per_scan);
                provider_budget_open = true;
            }

            // Background work: measure this thread's CPU per batch and pace to
            // the maintenance CPU target. Blocking I/O counts as quiet time.
            const auto work_started = Clock::now();
            const auto cpu_started = thread_cpu_time_ns();
            constexpr size_t background_hint_batch = 32;
            const auto batch = process_hint_batch(stop, background_hint_batch);
            const auto completed = Clock::now();
            auto cooldown = std::chrono::milliseconds(25);
            const auto cpu_completed = thread_cpu_time_ns();
            const double cpu_target = node_.config().maintenance.cpu_target;
            if (cpu_started && cpu_completed >= cpu_started && cpu_target > 0.0) {
                const auto cpu_ns = cpu_completed - cpu_started;
                const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    completed - work_started).count();
                const auto desired_wall_ns = static_cast<int64_t>(
                    static_cast<double>(cpu_ns) / cpu_target);
                if (desired_wall_ns > wall_ns) {
                    cooldown = std::max(
                        cooldown,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::nanoseconds(desired_wall_ns - wall_ns)));
                }
            }

            if (batch.claimed) {
                if (budget_http->exhausted()) {
                    provider_budget_open = false;
                    next_hint_batch = std::max(completed + cooldown,
                                               completed + config.provider_batch_delay);
                    Log::info("catalogue hint provider budget reached requests=" +
                              std::to_string(budget_http->used()));
                } else {
                    next_hint_batch = completed + cooldown;
                }
            } else if (auto delay = hints_.next_ready_delay()) {
                provider_budget_open = false;
                next_hint_batch = Clock::now() + *delay;
            } else {
                provider_budget_open = false;
                next_hint_batch = Clock::time_point::max();
            }
            hint_revision = hints_.revision();
        }

        const bool is_coordinator = coordinator();
        if (is_coordinator && !was_coordinator && !scanned_namespace && !prior_scan_evidence) {
            // A fresh scanner needs an initial discovery; a hand-off does not.
            mutation_due = now;
            if (!mutation_first_seen) mutation_first_seen = now;
        }
        was_coordinator = is_coordinator;

        const auto generation = metadata_server_.known_generation();
        if (generation != observed_generation) {
            observed_generation = generation;
            if (!mutation_first_seen) mutation_first_seen = now;
            mutation_due = now + config.rescan_debounce;
        }

        const bool periodic_due = now >= next_periodic;
        const bool debounced_mutation_due = mutation_due && now >= *mutation_due;
        const bool max_delayed_mutation_due = mutation_first_seen &&
            now >= *mutation_first_seen + config.rescan_max_delay;
        const bool mutation_rescan_due = debounced_mutation_due || max_delayed_mutation_due;
        const bool explicit_rescan = config.enabled &&
            rescan_requested_.exchange(false, std::memory_order_relaxed);
        const bool scheduled_rescan = is_coordinator && (periodic_due || mutation_rescan_due);
        if (config.enabled && (explicit_rescan || scheduled_rescan)) {
            try {
                const auto [before, before_generation] = namespace_identity();
                const bool namespace_changed = !scanned_namespace || before != *scanned_namespace;
                const bool reconciliation_due = !scanner_state_reconciled;
                if (explicit_rescan || namespace_changed || reconciliation_due) {
                    if (explicit_rescan) {
                        Log::info("catalogue: explicit rescan requested");
                    } else if (reconciliation_due && periodic_due && !namespace_changed) {
                        Log::info("catalogue: persisted scanner-state migration safety reconciliation; discovering");
                    } else if (mutation_rescan_due && namespace_changed && !periodic_due) {
                        if (max_delayed_mutation_due && !debounced_mutation_due)
                            Log::info("catalogue: namespace mutation max rescan delay reached; discovering");
                        else
                            Log::info("catalogue: namespace mutation settled; discovering");
                    }
                    std::string_view hint_source = "scanner";
                    int hint_priority = CatalogueHintPriority::periodic_scan;
                    bool unique_source_ref = false;
                    if (explicit_rescan) {
                        hint_source = "manual";
                        hint_priority = CatalogueHintPriority::manual_rescan;
                        unique_source_ref = true;
                    } else if (mutation_rescan_due && namespace_changed) {
                        hint_source = "namespace";
                        hint_priority = CatalogueHintPriority::namespace_mutation;
                    }
                    (void)scan_once(stop, explicit_rescan, hint_source,
                                    hint_priority, unique_source_ref);
                    if (stop.stop_requested()) break;
                    const auto [after, after_generation] = namespace_identity();
                    if (after != before) {
                        const auto restart = std::chrono::steady_clock::now();
                        mutation_first_seen = restart;
                        mutation_due = restart + config.rescan_debounce;
                    } else {
                        scanned_namespace = after;
                        scanner_state_reconciled = true;
                        mutation_due.reset();
                        mutation_first_seen.reset();
                    }
                    observed_generation = after_generation;
                    const auto next_due = next_scan_due_unix_ms(config.interval);
                    next_periodic = steady_due_from_unix_ms(next_due);
                    if (scanned_namespace) {
                        try {
                            persist_scanner_state(node_.config().state_path,
                                                  *scanned_namespace, next_due,
                                                  scanner_state_reconciled);
                        } catch (const std::exception& e) {
                            Log::debug("catalogue scanner state persistence unavailable: " +
                                       std::string(e.what()));
                        }
                    }
                    // Newly discovered hints are eligible at once.
                    next_hint_batch = std::min(next_hint_batch, std::chrono::steady_clock::now());
                } else {
                    // An unchanged namespace signature since the last
                    // reconciliation has nothing new: just advance the deadline.
                    mutation_due.reset();
                    mutation_first_seen.reset();
                    scanned_namespace = before;
                    observed_generation = before_generation;
                    if (periodic_due) {
                        const auto next_due = next_scan_due_unix_ms(config.interval);
                        next_periodic = steady_due_from_unix_ms(next_due);
                        try {
                            persist_scanner_state(node_.config().state_path, before, next_due, true);
                        } catch (const std::exception& e) {
                            Log::debug("catalogue scanner state persistence unavailable: " +
                                       std::string(e.what()));
                        }
                    }
                }
            } catch (const std::exception& e) {
                Log::warn("catalogue scan: " + std::string(e.what()));
                const auto retry_from = std::chrono::steady_clock::now();
                mutation_first_seen = retry_from;
                mutation_due = retry_from + config.rescan_debounce;
                next_periodic = retry_from + config.interval;
            }
        }
        cpu_reporter.tick();

        const auto log_now = std::chrono::steady_clock::now();
        if (log_now >= next_backlog_log && Log::enabled(LogLevel::debug)) {
            // next_hint_batch==max means no pending work: skip the summary walk.
            if (next_hint_batch != Clock::time_point::max()) {
                const auto backlog = hints_.summary();
                if (backlog.pending != 0) {
                    Log::debug("catalogue backlog pending=" + std::to_string(backlog.pending) +
                               " queued=" + std::to_string(backlog.queued) +
                               " processing=" + std::to_string(backlog.processing) +
                               " deferred=" + std::to_string(backlog.deferred) +
                               " failed=" + std::to_string(backlog.failed) +
                               " catalogued=" + std::to_string(backlog.catalogued) +
                               " no_match=" + std::to_string(backlog.no_match) +
                               " total=" + std::to_string(backlog.total));
                }
            }
            next_backlog_log = log_now + std::chrono::seconds(30);
        }

        // Hints are event-driven; the one-second ceiling only observes the
        // coordinator and metadata generation.
        const auto sleep_from = std::chrono::steady_clock::now();
        auto wake_at = sleep_from + std::chrono::seconds(1);
        if (config.enabled) {
            // Reconciliation is the coordinator's: an overdue deadline must not
            // spin a non-coordinator's idle loop.
            wake_at = std::min(wake_at, next_hint_batch);
            if (is_coordinator) {
                wake_at = std::min(wake_at, next_periodic);
                if (mutation_due) wake_at = std::min(wake_at, *mutation_due);
                if (mutation_first_seen)
                    wake_at = std::min(wake_at, *mutation_first_seen + config.rescan_max_delay);
            }
        }
        auto wait_for = std::chrono::duration_cast<std::chrono::milliseconds>(wake_at - sleep_from);
        if (wait_for < std::chrono::milliseconds(0)) wait_for = std::chrono::milliseconds(0);
        const auto before_revision = hints_.revision();
        if (before_revision != hint_revision) {
            hint_revision = before_revision;
            next_hint_batch = std::min(next_hint_batch, std::chrono::steady_clock::now());
            continue;
        }
        if (hints_.wait_for_change(stop, before_revision, wait_for)) {
            hint_revision = hints_.revision();
            next_hint_batch = std::min(next_hint_batch, std::chrono::steady_clock::now());
        }
    }
}

} // namespace macha
