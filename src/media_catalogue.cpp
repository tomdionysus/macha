// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_catalogue.hpp"
#include "diagnostics.hpp"

#include "crypto.hpp"
#include "log.hpp"
#include "macha_version.hpp"

#include <curl/curl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cctype>
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
    // Matching is deliberately more forgiving than parsing. Filenames and
    // providers disagree routinely on punctuation, apostrophes, ampersands,
    // sequel numerals and abbreviations; none of those differences should
    // force the parser to invent display punctuation that was not present.
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
            // Elide possessive punctuation so "Knight's" and "Knights"
            // compare equally without changing the displayed/local title.
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

std::optional<int32_t> year_from(std::string_view text);

std::string metadata_key(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value)
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::string dictionary_value(AVDictionary* dictionary,
                             std::initializer_list<std::string_view> keys) {
    if (!dictionary) return {};
    std::set<std::string> wanted;
    for (auto key : keys) wanted.insert(metadata_key(key));
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(dictionary, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        if (!entry->key || !entry->value) continue;
        if (wanted.contains(metadata_key(entry->key))) return trim(entry->value);
    }
    return {};
}

std::string metadata_value(AVFormatContext* format,
                           std::initializer_list<std::string_view> keys) {
    if (!format) return {};
    if (auto value = dictionary_value(format->metadata, keys); !value.empty()) return value;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (!format->streams[i]) continue;
        if (auto value = dictionary_value(format->streams[i]->metadata, keys); !value.empty())
            return value;
    }
    return {};
}

std::optional<int32_t> leading_integer(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    int32_t result{};
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end == value.data()) return {};
    return result;
}

struct AudioMetadataRead {
    std::shared_ptr<ReadHandle> handle;
    uint64_t size{};
    uint64_t offset{};
};

int audio_metadata_read(void* opaque, uint8_t* destination, int destination_size) {
    auto& state = *static_cast<AudioMetadataRead*>(opaque);
    if (state.offset >= state.size) return AVERROR_EOF;
    const auto wanted = static_cast<size_t>(std::min<uint64_t>(
        state.size - state.offset, static_cast<uint64_t>(destination_size)));
    try {
        const auto read = state.handle->read(
            state.offset, std::span<uint8_t>(destination, wanted));
        if (!read) return AVERROR_EOF;
        state.offset += read;
        return static_cast<int>(read);
    } catch (...) {
        return AVERROR(EIO);
    }
}

int64_t audio_metadata_seek(void* opaque, int64_t offset, int whence) {
    auto& state = *static_cast<AudioMetadataRead*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(state.size);
    const int base_whence = whence & ~AVSEEK_FORCE;
    int64_t base{};
    if (base_whence == SEEK_SET) base = 0;
    else if (base_whence == SEEK_CUR) base = static_cast<int64_t>(state.offset);
    else if (base_whence == SEEK_END) base = static_cast<int64_t>(state.size);
    else return AVERROR(EINVAL);
    if ((offset < 0 && offset < -base) ||
        (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset))
        return AVERROR(EINVAL);
    const auto next = base + offset;
    if (next < 0 || static_cast<uint64_t>(next) > state.size) return AVERROR(EINVAL);
    state.offset = static_cast<uint64_t>(next);
    return next;
}

std::string embedded_artwork_mime(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        return "image/jpeg";
    static constexpr std::array<uint8_t, 8> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() >= png.size() && std::equal(png.begin(), png.end(), bytes.begin()))
        return "image/png";
    if (bytes.size() >= 12 && std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "RIFF" &&
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 8), 4) == "WEBP")
        return "image/webp";
    if (bytes.size() >= 6) {
        const auto header = std::string_view(reinterpret_cast<const char*>(bytes.data()), 6);
        if (header == "GIF87a" || header == "GIF89a") return "image/gif";
    }
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "image/bmp";
    return {};
}

void apply_format_audio_metadata(AVFormatContext* format, std::string_view path,
                                 MediaProbe& probe,
                                 std::vector<LocalArtworkCandidate>& artwork,
                                 size_t max_artwork_bytes) {
    probe.title = metadata_value(format, {"title"});
    probe.album_artist = metadata_value(format, {"album artist", "album_artist", "albumartist"});
    probe.track_artist = metadata_value(format, {"artist"});
    probe.artist = probe.album_artist.empty() ? probe.track_artist : probe.album_artist;
    probe.album = metadata_value(format, {"album"});
    if (auto value = metadata_value(format, {"track", "tracknumber"}); !value.empty())
        probe.track = leading_integer(value);
    if (auto value = metadata_value(format, {"disc", "discnumber"}); !value.empty())
        probe.disc = leading_integer(value);
    if (auto value = metadata_value(format, {"date", "year"}); !value.empty())
        probe.year = year_from(value);

    auto optional_tag = [&](std::initializer_list<std::string_view> keys)
        -> std::optional<std::string> {
        auto value = metadata_value(format, keys);
        if (value.empty()) return {};
        return value;
    };
    probe.musicbrainz_recording_id = optional_tag(
        {"musicbrainz track id", "musicbrainz_trackid", "musicbrainz recording id",
         "musicbrainz_recordingid"});
    probe.musicbrainz_release_id = optional_tag(
        {"musicbrainz album id", "musicbrainz_albumid", "musicbrainz release id",
         "musicbrainz_releaseid"});
    probe.musicbrainz_artist_id = optional_tag(
        {"musicbrainz artist id", "musicbrainz_artistid"});

    if (max_artwork_bytes == 0) return;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        auto* stream = format->streams[i];
        if (!stream || !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        const auto& picture = stream->attached_pic;
        if (!picture.data || picture.size <= 0) continue;
        const auto size = static_cast<size_t>(picture.size);
        if (size > max_artwork_bytes) {
            Log::debug("catalogue embedded artwork ignored path=" + std::string(path) +
                       " bytes=" + std::to_string(size) + " limit=" +
                       std::to_string(max_artwork_bytes));
            continue;
        }
        auto picture_bytes = std::span<const uint8_t>(picture.data, size);
        auto mime = embedded_artwork_mime(picture_bytes);
        if (mime.empty()) continue;
        Bytes bytes(picture_bytes.begin(), picture_bytes.end());
        const bool duplicate = std::any_of(artwork.begin(), artwork.end(), [&](const auto& existing) {
            return existing.bytes == bytes;
        });
        if (!duplicate) artwork.push_back({"cover", std::move(mime), std::move(bytes)});
    }
}

void apply_audio_metadata(FileSystem& fs, std::string_view path, const FsEntry& entry,
                          MediaProbe& probe, std::vector<LocalArtworkCandidate>& artwork,
                          size_t max_artwork_bytes) {
    auto handle = fs.open_read(entry, std::string(path), false, FrameType::read_ahead);
    AudioMetadataRead state{std::move(handle), entry.size, 0};
    constexpr int buffer_size = 64 * 1024;
    auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
    if (!buffer) throw std::bad_alloc();
    auto* io = avio_alloc_context(buffer, buffer_size, 0, &state,
                                  audio_metadata_read, nullptr, audio_metadata_seek);
    if (!io) {
        av_free(buffer);
        throw std::bad_alloc();
    }
    io->seekable = AVIO_SEEKABLE_NORMAL;
    auto* format = avformat_alloc_context();
    if (!format) {
        avio_context_free(&io);
        throw std::bad_alloc();
    }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    auto* candidate = format;
    const auto opened = avformat_open_input(&candidate, nullptr, nullptr, nullptr);
    format = candidate;
    if (opened < 0) {
        if (format) avformat_free_context(format);
        avio_context_free(&io);
        return;
    }

    apply_format_audio_metadata(format, path, probe, artwork, max_artwork_bytes);
    avformat_close_input(&format);
    avio_context_free(&io);
}

struct MusicMetadataReadResult {
    MediaProbe probe;
    std::vector<LocalArtworkCandidate> artwork;
};

std::optional<MusicMetadataReadResult> read_music_metadata_probe(FileSystem& fs,
                                                                  std::string_view path,
                                                                  const FsEntry& entry,
                                                                  size_t max_artwork_bytes) {
    if (entry.type != EntryType::file || entry.size == 0 ||
        !audio_extension(extension(path)))
        return {};

    MusicMetadataReadResult result;
    auto& probe = result.probe;
    probe.kind = MediaProbeKind::track;
    probe.path = normalize_path(std::string(path));
    probe.media_id = file_media_id(entry);
    try {
        apply_audio_metadata(fs, path, entry, probe, result.artwork, max_artwork_bytes);
    } catch (const std::exception& e) {
        Log::debug("catalogue music tags unavailable path=" + std::string(path) +
                   " reason=" + e.what());
    }
    return result;
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
    // Trailing brackets are ambiguous: release/site tags such as [rartv],
    // [EZTVx.to] and [i_c] are noise, but human episode qualifiers such as
    // [Pilot] are semantic title text. Only remove brackets that look like
    // distribution/source tags rather than every trailing bracketed phrase.
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

    // Collection directories frequently append several structural descriptors,
    // e.g. "Season 1-4 S01-S04" or "Complete Series". Remove the first strong
    // bundle marker and everything after it; those tokens describe the layout,
    // not the provider series title.
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
    // Scene names often encode a possessive apostrophe as a dot because dots
    // are also word separators (for example "Tasty.Tudi.s"). Recover that
    // punctuation before generic release-noise/title cleanup destroys the
    // distinction between a possessive and a standalone letter S.
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
        out.push_back({std::stoi((*it).str()), pos, len});
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
        if (!video_extension(extension(path)) || episode_pattern(stem(path))) return {};
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

        // Release names occasionally append commentary/language/genre metadata
        // after a human-readable " - ". Preserve the ordinary full-title
        // hypothesis, but add a stronger split candidate when the right side
        // contains a release year and obvious distribution metadata.
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
        if (!video_extension(extension(path)) || episode_pattern(stem(path))) return {};
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
        if (!video_extension(extension(path)) || episode_pattern(stem(path))) return {};
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
        if (!video_extension(extension(path))) return {};
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
        if (!video_extension(extension(path))) return {};
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
        if (!audio_extension(extension(path))) return {};
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
            // Generic probe_media_path() has no configured root. Preserve the
            // long-standing Artist/Album/File and Artist/Album/Disc/File
            // fallbacks for tests and direct callers.
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
                // Exact-year agreement on a highly ranked result is useful
                // evidence for aliases/translations whose canonical provider
                // title has little lexical overlap with the release filename.
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
        // Discogs appends numeric disambiguators such as "Artist (2)". They
        // identify the database entity, not the display credit.
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
    if (audio_extension(ext)) {
        AVFormatContext* format = nullptr;
        const auto opened = avformat_open_input(&format, path.string().c_str(), nullptr, nullptr);
        if (opened >= 0 && format) {
            MediaProbe probe;
            probe.kind = MediaProbeKind::track;
            probe.path = path.generic_string();
            std::vector<LocalArtworkCandidate> ignored_artwork;
            // Ingest only needs identity fields for placement. Artwork is read again
            // by the ordinary catalogue scanner after the namespace write.
            apply_format_audio_metadata(format, probe.path, probe, ignored_artwork, 0);
            embedded = std::move(probe);
        }
        if (format) avformat_close_input(&format);
    }
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
    struct curl_slist* raw_headers = nullptr;
    for (const auto& header : headers) raw_headers = curl_slist_append(raw_headers, header.c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_guard(raw_headers, curl_slist_free_all);
    if (raw_headers) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, raw_headers);
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
    const auto key = normalized(probe.series) + "|" + (probe.year ? std::to_string(*probe.year) : "");
    if (auto it = show_cache_.find(key); it != show_cache_.end()) return it->second;
    std::vector<std::pair<std::string, std::string>> q{{"query", probe.series}, {"language", config_.language}};
    // Do not use TMDB's exact first_air_date_year filter here. Local TV
    // libraries often name a series after a pilot/miniseries/production year,
    // while TMDB dates the regular series one year later. Score the year
    // locally instead so +/-1 remains viable evidence rather than a hard miss.
    auto root = api("/search/tv", q);
    const auto* result = best_result(root, probe.series, "name", probe.year,
                                     "first_air_date", true);
    if (!result) {
        show_cache_[key] = std::nullopt;
        return {};
    }
    show_cache_[key] = *result;
    return *result;
}

std::optional<ProviderMatch> TmdbProvider::lookup(const MediaProbe& probe) {
    if (!supports(probe.kind)) return {};
    ProviderMatch match;
    if (probe.kind == MediaProbeKind::movie) {
        const auto key = normalized(probe.title) + "|" +
                         (probe.year ? std::to_string(*probe.year) : "");
        std::optional<Json> cached_detail;
        if (auto it = movie_cache_.find(key); it != movie_cache_.end()) {
            cached_detail = it->second;
        } else {
            std::vector<std::pair<std::string, std::string>> q{
                {"query", probe.title}, {"language", config_.language}};
            if (probe.year) q.emplace_back("primary_release_year", std::to_string(*probe.year));
            auto search = api("/search/movie", q);
            auto found = best_result(search, probe.title, "title", probe.year, "release_date");
            if (!found) {
                movie_cache_[key] = std::nullopt;
                return {};
            }
            auto id_value = json_i32(found->find("id"));
            if (!id_value) {
                movie_cache_[key] = std::nullopt;
                return {};
            }
            const auto tmdb_id = std::to_string(*id_value);
            cached_detail = api("/movie/" + tmdb_id, {{"language", config_.language}});
            movie_cache_[key] = cached_detail;
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
        season_cache_[season_key] = season;
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

    // A common legacy/library convention keeps a pilot/miniseries under the
    // parent show's Specials folder even where TMDB models that exact-year
    // programme as its own one-season show. If the exact-year show has no
    // season zero, try the same episode number in season one before discarding
    // an otherwise strong series/year identity.
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

    // Episode numbers are the primary identity once series + year are known.
    // For yearless fallbacks the title remains the independent corroborator;
    // if numbering differs (notably specials under alternate ordering schemes),
    // remap by a strong title match within the already-resolved TMDB season.
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

MusicBrainzProvider::MusicBrainzProvider(HttpClient& http, CatalogueMusicBrainzConfig config)
    : http_(http), config_(std::move(config)) {}

bool MusicBrainzProvider::supports(MediaProbeKind kind) const {
    return config_.enabled && kind == MediaProbeKind::track;
}

Json MusicBrainzProvider::api(std::string_view path,
                              const std::vector<std::pair<std::string, std::string>>& query) {
    const auto now = std::chrono::steady_clock::now();
    if (unavailable_until_ > now)
        throw ProviderTemporarilyUnavailable("musicbrainz", "MusicBrainz circuit open");

    if (last_request_ != std::chrono::steady_clock::time_point{}) {
        const auto due = last_request_ + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < due) {
            if (http_.stop_requested())
                throw std::runtime_error("MusicBrainz request cancelled");
            const auto remaining = due - std::chrono::steady_clock::now();
            const auto slice = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::min(remaining, slice));
        }
    }
    auto ua = std::string("Macha/") + std::string(kServerVersion) + " (" + config_.contact + ")";
    auto q = query;
    q.emplace_back("fmt", "json");
    RemoteHttpResponse response;
    try {
        response = http_.get(query_url("https://musicbrainz.org/ws/2" + std::string(path), q),
                             {"Accept: application/json", "User-Agent: " + ua});
    } catch (const ProviderBudgetExhausted&) {
        throw;
    } catch (const std::exception& e) {
        unavailable_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        throw ProviderTemporarilyUnavailable(
            "musicbrainz", "MusicBrainz transport unavailable: " + std::string(e.what()));
    }
    last_request_ = std::chrono::steady_clock::now();
    if (transient_provider_status(response.status)) {
        unavailable_until_ = last_request_ + std::chrono::seconds(60);
        throw ProviderTemporarilyUnavailable(
            "musicbrainz", "MusicBrainz returned HTTP " + std::to_string(response.status));
    }
    if (response.status != 200)
        throw std::runtime_error("MusicBrainz returned HTTP " + std::to_string(response.status));
    return Json::parse(std::string_view(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
}

std::optional<Json> MusicBrainzProvider::release_by_id(std::string_view release_id) {
    if (release_id.empty()) return {};
    const auto key = std::string(release_id);
    if (auto it = release_id_cache_.find(key); it != release_id_cache_.end()) return it->second;
    auto detail = api("/release/" + key,
                      {{"inc", "recordings+artist-credits+release-groups"}});
    release_id_cache_[key] = detail;
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
        release_cache_[key] = std::nullopt;
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
        release_cache_[key] = std::nullopt;
        return {};
    }
    auto release_id = json_string(best->find("id"));
    if (release_id.empty()) {
        release_cache_[key] = std::nullopt;
        return {};
    }
    auto detail = release_by_id(release_id);
    release_cache_[key] = detail;
    return detail;
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

    if (recording_id.empty()) {
        auto query = "recording:\"" + lucene_quote(probe.title) + "\" AND artist:\"" +
                     lucene_quote(probe.artist) + "\"";
        auto search = api("/recording", {{"query", query}, {"limit", "10"}});
        auto recordings = search.find("recordings");
        if (!recordings || !recordings->isArray() || recordings->asArray().empty()) {
            recording_cache_[key] = std::nullopt;
            return {};
        }
        const Json* best = &recordings->asArray().front();
        int best_score = -1;
        for (const auto& candidate : recordings->asArray()) {
            int score = 0;
            if (normalized(json_string(candidate.find("title"))) == normalized(probe.title)) score += 100;
            if (normalized(artist_credit_name(candidate.find("artist-credit"))) == normalized(probe.artist)) score += 80;
            if (auto provider_score = json_i32(candidate.find("score"))) score += *provider_score / 10;
            if (score > best_score) { best_score = score; best = &candidate; }
        }
        if (best_score < 120) {
            recording_cache_[key] = std::nullopt;
            return {};
        }
        recording_id = json_string(best->find("id"));
        if (recording_id.empty()) {
            recording_cache_[key] = std::nullopt;
            return {};
        }
    }

    auto detail = api("/recording/" + recording_id,
                      {{"inc", "artist-credits+releases"}});
    recording_cache_[key] = detail;
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
    if (auto media = release->find("media"); media && media->isArray()) {
        for (const auto& medium : media->asArray()) {
            if (probe.disc) {
                auto medium_position = json_i32(medium.find("position"));
                if (medium_position && *medium_position != *probe.disc) continue;
            }
            auto tracks = medium.find("tracks");
            if (!tracks || !tracks->isArray()) continue;
            for (const auto& release_track : tracks->asArray()) {
                auto number = json_i32(release_track.find("position"));
                const auto title = json_string(release_track.find("title"));
                auto candidate_recording = release_track.find("recording");
                const auto candidate_id = candidate_recording && candidate_recording->isObject()
                    ? json_string(candidate_recording->find("id")) : std::string{};
                const bool id_match = !wanted_recording_id.empty() && candidate_id == wanted_recording_id;
                const bool number_match = probe.track && number && *probe.track == *number;
                const bool title_match = !probe.title.empty() && normalized(title) == normalized(probe.title);
                if (!id_match && !number_match && !title_match) continue;
                recording = candidate_recording;
                position = number.value_or(probe.track.value_or(0));
                break;
            }
            if (recording) break;
        }
    }
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
        throw ProviderTemporarilyUnavailable("discogs", "Discogs circuit open");

    // Authenticated Discogs clients are limited to 60 requests/minute. Pace
    // locally as well as obeying Macha's global per-scan HTTP budget.
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
    release_cache_[key] = detail;
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
        // Keep album/year as local scoring evidence for recording-first lookup;
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
        search_cache_[key] = selected;
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
    return video_extension(extension(path));
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
    return video_extension(extension(path));
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
                                     size_t max_artwork_bytes)
    : roots_(std::move(config.roots)), max_artwork_bytes_(max_artwork_bytes) {
    if (config.musicbrainz.enabled)
        metadata_.push_back(std::make_unique<MusicBrainzProvider>(http, std::move(config.musicbrainz)));
    if (config.discogs.enabled) {
        try {
            metadata_.push_back(std::make_unique<DiscogsProvider>(http, std::move(config.discogs)));
        } catch (const std::exception& e) {
            Log::warn("catalogue Discogs metadata disabled: " + std::string(e.what()));
        }
    }
}

bool MusicScanProvider::accepts_path(std::string_view path) const noexcept {
    return audio_extension(extension(path));
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

CatalogueScanner::CatalogueScanner(NodeRuntime& node, FileSystem& fs,
                                   CatalogueManager& catalogue, CatalogueHintQueue& hints,
                                   CatalogueScannerConfig config,
                                   std::unique_ptr<HttpClient> http,
                                   std::chrono::milliseconds diagnostic_interval)
    : node_(node), fs_(fs), catalogue_(catalogue), hints_(hints), config_(std::move(config)),
      http_(http ? std::move(http) : std::make_unique<CurlHttpClient>()),
      provider_http_(std::make_unique<BudgetHttpClient>(*http_)),
      diagnostic_interval_(diagnostic_interval) {
    configure_providers();
}
CatalogueScanner::~CatalogueScanner() { stop(); }

void CatalogueScanner::configure_providers() {
    providers_.clear();
    if (config_.movies.enabled)
        providers_.push_back(std::make_unique<MovieScanProvider>(*provider_http_, config_.movies));
    if (config_.tv.enabled)
        providers_.push_back(std::make_unique<TvScanProvider>(*provider_http_, config_.tv));
    if (config_.music.enabled)
        providers_.push_back(std::make_unique<MusicScanProvider>(*provider_http_, config_.music,
                                                               config_.max_artwork_bytes));
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
    http_->reset_stop();
    hints_.requeue_processing();
    worker_ = std::jthread([this](std::stop_token stop) { loop(stop); });
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
        std::lock_guard lock(config_mutex_);
        if (!config_.enabled)
            return 0;
    }

    std::set<std::string> wanted(media_ids.begin(), media_ids.end());
    std::vector<CatalogueHintSubmission> submissions;

    // Metadata clear already knows exactly which immutable media identities
    // became unbound. Resolve those identities against the already-decoded
    // namespace and enqueue only the affected paths; never turn a one-item
    // mutation into a forced full-library rescan.
    std::optional<MetadataSnapshotView> available = fs_.available_snapshot_view();
    std::optional<MetadataSnapshot> local;
    const MetadataSnapshot* snapshot = nullptr;
    if (available) {
        snapshot = available->snapshot.get();
    } else {
        // This is host-local durable metadata only, not a quorum read. It is a
        // best-effort rematch accelerator; the ordinary namespace/safety scan
        // remains the correctness fallback if the local replica is stale.
        try {
            local = fs_.local_snapshot();
            snapshot = &*local;
        } catch (...) {
            return 0;
        }
    }

    for (const auto& [path, entry] : snapshot->entries) {
        if (entry.type != EntryType::file)
            continue;
        const auto media_id = file_media_id(entry);
        if (!wanted.contains(media_id))
            continue;
        std::string root;
        auto* provider = provider_for_path(path, root);
        if (!provider || !provider->accepts_path(path))
            continue;
        submissions.push_back({path, "manual", media_id,
                               CatalogueHintPriority::manual_rescan});
    }

    const auto queued = hints_.submit_many(std::move(submissions)).size();
    Log::debug("catalogue metadata clear targeted rematch media_ids=" +
               std::to_string(wanted.size()) + " queued=" + std::to_string(queued));
    return queued;
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

std::vector<std::pair<std::string, FsEntry>> catalogue_snapshot_files(
    std::string_view root, const MetadataSnapshot& namespace_snapshot, std::stop_token stop) {
    const auto normalized = normalize_path(std::string(root));
    const auto root_entry = namespace_snapshot.entries.find(normalized);
    if (root_entry == namespace_snapshot.entries.end())
        throw FsError(ENOENT, "missing");
    if (root_entry->second.type != EntryType::directory)
        throw FsError(ENOTDIR, "catalogue root is not a directory");

    // A destructive discovery pass must describe one immutable namespace
    // generation. Enumerating the snapshot directly is both cheaper than a
    // sequence of readdir() calls and prevents a mutation between directories
    // from manufacturing an absence that never existed in any generation.
    std::vector<std::pair<std::string, FsEntry>> out;
    const auto prefix = normalized == "/" ? std::string("/") : normalized + "/";
    auto it = namespace_snapshot.entries.lower_bound(prefix);
    for (; it != namespace_snapshot.entries.end(); ++it) {
        if (stop.stop_requested()) break;
        if (!it->first.starts_with(prefix)) break;
        if (it->second.type == EntryType::file)
            out.emplace_back(it->first, it->second);
    }
    return out;
}

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
    return selected;
}

std::optional<CatalogueScanner::PreparedHintMatch>
CatalogueScanner::prepare_hint(const CatalogueHint& hint, std::stop_token stop,
                               const MetadataSnapshot& namespace_snapshot) {
    if (stop.stop_requested()) return {};
    CatalogueScannerConfig config;
    {
        std::lock_guard lock(config_mutex_);
        config = config_;
    }

    std::string root;
    auto* provider = provider_for_path(hint.path, root);
    if (!provider) {
        hints_.mark_no_match(hint.id, {}, {}, "path is outside configured catalogue roots");
        return {};
    }

    // The batch owns one immutable namespace snapshot. Do not call
    // FileSystem::getattr() here: that path may acquire authoritative metadata
    // and previously rebuilt/read metadata separately for every hint.
    const auto path = normalize_path(hint.path);
    auto entry_it = namespace_snapshot.entries.find(path);
    if (entry_it == namespace_snapshot.entries.end()) {
        hints_.fail(hint.id, "namespace path no longer exists");
        return {};
    }
    const auto& entry = entry_it->second;
    if (entry.type != EntryType::file) {
        hints_.mark_no_match(hint.id, std::string(provider->name()), {},
                             "namespace path is not a media file");
        return {};
    }
    if (entry.size == 0) {
        // A zero-length committed file can be a transient namespace shell while
        // durable FUSE data is still being published. It has no meaningful
        // immutable media identity yet, so do not negative-cache it as a
        // provider miss. A later namespace generation/source_ref will reopen
        // the hint as soon as committed content becomes visible.
        hints_.defer(hint.id, "namespace media file has no committed content yet",
                     unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
        return {};
    }

    const auto media_id = file_media_id(entry);
    auto existing = catalogue_.snapshot();
    std::vector<std::string> existing_ids;
    for (const auto& [id, item] : existing.items) {
        if (std::find(item.media_ids.begin(), item.media_ids.end(), media_id) != item.media_ids.end())
            existing_ids.push_back(id);
    }

    const bool manual_refresh = std::any_of(
        hint.origins.begin(), hint.origins.end(),
        [](const auto& origin) { return origin.source == "manual"; });

    // Exact media identity is content-derived from the immutable extent manifest.
    // If this object is already bound, a passive scanner/ingest retry has no new
    // information to discover. Do not reopen/decrypt it merely to rediscover the
    // same embedded tags. Manual rescans deliberately retain the full probe path.
    if (!existing_ids.empty() && !manual_refresh) {
        return PreparedHintMatch{hint.id, std::string(provider->name()), media_id,
                                 std::move(existing_ids), {},
                                 "already catalogued", hint.attempts};
    }

    auto probed = provider->probe_file(fs_, root, hint.path, entry);
    if (stop.stop_requested()) return {};
    if (probed.candidates.empty()) {
        hints_.mark_no_match(hint.id, std::string(provider->name()), {},
                             "no supported media candidate");
        return {};
    }

    // A manual refresh of an already-bound immutable file may still merge newly
    // supported embedded artwork, but it never needs an online metadata lookup.
    std::optional<CatalogueItem> artwork_target;
    if (!existing_ids.empty()) {
        for (const auto& [id, item] : existing.items) {
            if (std::find(item.media_ids.begin(), item.media_ids.end(), media_id) == item.media_ids.end())
                continue;
            if (!probed.artwork.empty()) {
                if (item.kind == CatalogueKind::track && item.parent_id) {
                    if (auto parent = existing.items.find(*item.parent_id); parent != existing.items.end())
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
                auto staged = catalogue_.stage_artwork(art.role, art.mime_type, art.bytes);
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
                                 "already catalogued", hint.attempts};
    }

    constexpr size_t max_candidate_attempts = 5;
    const auto candidate_limit = std::min(max_candidate_attempts, probed.candidates.size());
    if (hint.candidate_cursor >= candidate_limit) {
        std::string result = "no metadata provider match";
        if (candidate_limit > 1)
            result += " after " + std::to_string(candidate_limit) + " candidates";
        hints_.mark_no_match(hint.id, std::string(provider->name()), media_id, std::move(result));
        Log::debug("catalogue hint: no provider match path=" + hint.path +
                   " provider=" + std::string(provider->name()));
        return {};
    }

    // One scheduling turn evaluates one metadata hypothesis. Some provider
    // lookups legitimately require several HTTP requests (for example a
    // MusicBrainz search followed by release detail), so fairness must be at
    // the candidate-hypothesis boundary rather than at the HTTP-request
    // boundary. Persisting the cursor lets the queue yield to another root and
    // resume the next fallback without repeating earlier hypotheses after a
    // restart.
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
        hints_.defer(hint.id, "provider request budget exhausted",
                     unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
        return {};
    } catch (const ProviderTemporarilyUnavailable& e) {
        // Temporary availability is provider state, not per-media state. The
        // old code deferred only this hint; the next hint for the same scan
        // provider was immediately claimed, reprobed locally and then discovered
        // the exact same already-open provider circuit. A large library therefore
        // turned one remote outage into continuous local media probing.
        const auto retry_delay = std::max(config.provider_batch_delay, e.retry_after());
        const auto retry_at = unix_ms() + static_cast<uint64_t>(retry_delay.count());
        const auto deferred = hints_.defer_matching(
            [&](const CatalogueHint& queued) {
                std::string queued_root;
                return provider_for_path(queued.path, queued_root) == provider;
            },
            e.what(), retry_at);
        Log::warn("catalogue hint provider temporarily unavailable provider=" +
                  std::string(provider->name()) + " path=" + hint.path +
                  " deferred_hints=" + std::to_string(deferred) +
                  " retry_ms=" + std::to_string(retry_delay.count()) + ": " + e.what());
        return {};
    } catch (const std::exception& e) {
        Log::warn("catalogue hint lookup failed provider=" + std::string(provider->name()) +
                  " path=" + hint.path + " candidate=" + candidate.generator + ": " + e.what());
        hints_.record_failure(hint.id, e.what(),
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
        std::string result = "no metadata provider match";
        if (candidate_limit > 1)
            result += " after " + std::to_string(candidate_limit) + " candidates";
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
                    auto staged = catalogue_.stage_artwork(art.role, art.mime_type, art.bytes);
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

    std::set<std::tuple<std::string, std::string, std::string>> fetched_remote_artwork;
    for (const auto& art : match.artwork) {
        if (stop.stop_requested()) return {};
        if (!fetched_remote_artwork.emplace(art.item_id, art.role, art.url).second) continue;
        auto target = std::find_if(match.items.begin(), match.items.end(), [&](const auto& item) {
            return item.id == art.item_id;
        });
        if (target == match.items.end()) continue;
        if (auto old = existing.items.find(art.item_id); old != existing.items.end()) {
            const auto locked = old->second.external_ids.find("macha_metadata_locked");
            if (locked != old->second.external_ids.end() && locked->second == "1") continue;
        }
        try {
            auto response = http_->get(art.url, {}, config.max_artwork_bytes);
            if (response.status != 200 || response.body.empty()) continue;
            auto mime = response.content_type;
            if (auto semi = mime.find(';'); semi != std::string::npos) mime.resize(semi);
            if (!mime.starts_with("image/")) continue;
            auto staged = catalogue_.stage_artwork(art.role, mime, response.body);
            const bool duplicate = std::any_of(
                target->artwork.begin(), target->artwork.end(), [&](const auto& current) {
                    return current.role == staged.role && current.id == staged.id;
                });
            if (!duplicate) target->artwork.push_back(std::move(staged));
        } catch (const std::exception& e) {
            if (stop.stop_requested()) return {};
            Log::warn("catalogue artwork failed for " + art.item_id + ": " + e.what());
        }
    }

    std::vector<std::string> item_ids;
    item_ids.reserve(match.items.size());
    for (const auto& item : match.items) item_ids.push_back(item.id);
    return PreparedHintMatch{hint.id, std::string(provider->name()), probe.media_id,
                             std::move(item_ids), std::move(match.items),
                             "matched " + selected_candidate->generator, hint.attempts};
}

CatalogueScanner::HintBatchResult
CatalogueScanner::process_hint_batch(std::stop_token stop, size_t max_hints) {
    CatalogueScannerConfig config;
    {
        std::lock_guard lock(config_mutex_);
        config = config_;
    }
    auto* budget_http = dynamic_cast<BudgetHttpClient*>(provider_http_.get());
    if (!budget_http) throw std::runtime_error("catalogue provider HTTP budget unavailable");

    HintBatchResult out;
    std::vector<PreparedHintMatch> prepared;
    prepared.reserve(max_hints);

    // Acquire one coherent decoded namespace view lazily for the complete
    // batch. An empty queue therefore causes no metadata work at all. Normally
    // this is a pure cache read; a cold-start batch may populate it once.
    std::optional<MetadataSnapshotView> namespace_view;

    for (; out.claimed < max_hints && !stop.stop_requested() && !budget_http->exhausted();) {
        auto hint = hints_.claim_next();
        if (!hint) break;
        ++out.claimed;
        try {
            if (!namespace_view) {
                namespace_view = fs_.available_snapshot_view();
                if (!namespace_view)
                    namespace_view = fs_.local_snapshot_view();
            }
            if (auto match = prepare_hint(*hint, stop, *namespace_view->snapshot))
                prepared.push_back(std::move(*match));
        } catch (const CatalogueConflict& e) {
            hints_.defer(hint->id, e.what(), unix_ms() + 500);
        } catch (const CatalogueUnavailable& e) {
            hints_.defer(hint->id, e.what(),
                         unix_ms() + static_cast<uint64_t>(config.provider_batch_delay.count()));
        } catch (const std::exception& e) {
            hints_.record_failure(hint->id, e.what(),
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

    std::vector<CatalogueItem> discovered;
    std::set<std::string> active_media_ids;
    for (const auto& match : prepared) {
        active_media_ids.insert(match.media_id);
        discovered.insert(discovered.end(), match.items.begin(), match.items.end());
    }

    try {
        if (!discovered.empty())
            catalogue_.reconcile_scanner(discovered, active_media_ids, false);
    } catch (const CatalogueConflict& e) {
        for (const auto& match : prepared)
            hints_.defer(match.hint_id, e.what(), unix_ms() + 500);
        return out;
    } catch (const CatalogueUnavailable& e) {
        const auto retry = unix_ms() +
            static_cast<uint64_t>(config.provider_batch_delay.count());
        for (const auto& match : prepared)
            hints_.defer(match.hint_id, e.what(), retry);
        Log::debug("catalogue hint batch deferred: " + std::string(e.what()));
        return out;
    } catch (const std::exception& e) {
        for (const auto& match : prepared)
            hints_.record_failure(
                match.hint_id, e.what(),
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
        std::lock_guard lock(config_mutex_);
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
    {
        std::lock_guard lock(config_mutex_);
        config = config_;
    }
    if (!config.enabled || (!force && !coordinator()) || stop.stop_requested()) return 0;

    struct ProviderFile {
        CatalogueScanProvider* provider{};
        std::string root;
        std::string path;
        FsEntry entry;
    };
    const auto namespace_view = fs_.local_snapshot_view();
    const auto& namespace_snapshot = *namespace_view.snapshot;
    std::vector<ProviderFile> files;
    size_t roots_scanned = 0;
    size_t roots_unavailable = 0;
    for (auto& provider : providers_) {
        for (const auto& root : provider->roots()) {
            try {
                auto root_files = catalogue_snapshot_files(root, namespace_snapshot, stop);
                if (stop.stop_requested()) return 0;
                ++roots_scanned;
                for (auto& [path, entry] : root_files)
                    files.push_back({provider.get(), normalize_path(root),
                                     std::move(path), std::move(entry)});
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

    auto existing = catalogue_.snapshot();
    std::set<std::string> bound;
    for (const auto& [_, item] : existing.items)
        bound.insert(item.media_ids.begin(), item.media_ids.end());

    std::set<std::string> active_media_ids;
    std::vector<CatalogueHintSubmission> submissions;
    submissions.reserve(files.size());
    const auto scan_ref = unique_source_ref
        ? std::string(hint_source) + ":" + std::to_string(unix_ms())
        : std::string{};
    for (const auto& file : files) {
        if (stop.stop_requested()) return 0;
        if (!file.provider->accepts_path(file.path)) continue;
        // Zero-length files do not yet have a meaningful immutable media
        // identity. During durable FUSE recovery they are commonly committed
        // namespace shells whose data/extents will appear in a later metadata
        // generation. Do not queue them and, critically, do not collapse every
        // such path onto the shared empty-file hash in active_media_ids.
        if (file.entry.size == 0) continue;

        // Discovery answers only "which immutable media objects exist?". The
        // media id is available directly from FsEntry; opening every file here
        // duplicates the expensive libav/tag probe that the hint consumer must
        // perform for genuinely unbound media. On an old library that turned one
        // repair pass into two complete media reads per item.
        const auto media_id = file_media_id(file.entry);
        active_media_ids.insert(media_id);

        // An exact bound media id is already known. Byte changes produce a new
        // id and therefore queue normal enrichment. Manual rescans intentionally
        // queue bound objects so newly-supported embedded metadata/artwork can be
        // revisited on demand.
        if (!bound.contains(media_id) || force)
            submissions.push_back({file.path, std::string(hint_source),
                                   unique_source_ref ? scan_ref : media_id,
                                   hint_priority});
    }

    const auto ids = hints_.submit_many(std::move(submissions));
    if (stop.stop_requested()) return 0;
    // Discovery is the only destructive catalogue source. Hint processing is
    // additive and cannot infer absence from a single path.
    catalogue_.reconcile_scanner(
        {}, active_media_ids, complete_scan,
        complete_scan ? std::optional<Hash256>(metadata_namespace_signature(namespace_snapshot))
                      : std::nullopt);
    if (!ids.empty())
        Log::info("catalogue scan queued " + std::to_string(ids.size()) + " media hints");
    return ids.size();
}

void CatalogueScanner::loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-catalogue", diagnostic_interval_, true);
    CatalogueScannerConfig initial_config;
    { std::lock_guard lock(config_mutex_); initial_config = config_; }
    const auto persisted = load_scanner_state(node_.config().state_path);
    std::optional<Hash256> scanned_namespace = persisted.namespace_signature;
    bool scanner_state_reconciled = !persisted.namespace_signature || persisted.reconciled;
    // Migration from pre-scanner.state releases: the durable hint-state file is
    // created only once catalogue work has been admitted. It remains present
    // even if all ephemeral successful hints have since been discarded, making
    // it a better "this library has already been through scanner operation"
    // marker than the current in-memory hint count. Migration seeds the current
    // identity without scanning immediately, but marks it unverified so one full
    // reconciliation is still performed at the ordinary safety deadline.
    const bool prior_scan_evidence = std::filesystem::exists(
        node_.config().state_path / "catalogue" / "hints.json");
    std::optional<std::chrono::steady_clock::time_point> mutation_due;
    std::optional<std::chrono::steady_clock::time_point> mutation_first_seen;
    auto observed_generation = node_.known_metadata_generation();
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
            available && generation >= node_.known_metadata_generation())
            return {*available, generation};
        // MetadataManager owns convergence. Only if its decoded immutable view
        // is absent/stale do we fall back to the strong snapshot path. Settled
        // periodic safety checks therefore remain local.
        auto signature = fs_.namespace_signature(&generation);
        return {signature, generation};
    };

    while (!stop.stop_requested()) {
        CatalogueScannerConfig config;
        { std::lock_guard lock(config_mutex_); config = config_; }
        const auto now = std::chrono::steady_clock::now();

        // On restart, compare the last successfully reconciled namespace
        // identity against MetadataManager's already-decoded view. Coordinator
        // election itself is not evidence of a namespace mutation and must not
        // launch a full discovery pass. For the one-time migration from older
        // state (no scanner.state), an existing durable hint-state file is
        // sufficient evidence that this library has already been operated by
        // the scanner: seed the current signature as unverified and schedule one
        // full reconciliation at the normal safety interval instead of reopening
        // the entire historical result set immediately.
        if (!initial_signature_checked) {
            uint64_t available_generation = 0;
            if (auto current = fs_.available_namespace_signature(&available_generation);
                current && available_generation >= node_.known_metadata_generation()) {
                // Do not seed/reconcile scanner state from a decoded snapshot
                // that is already known to be stale. Metadata convergence owns
                // fetching/decoding the advertised generation; this loop will
                // observe the immutable view cheaply once it catches up.
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

        // Every node may consume its own persistent hints. This makes an ingest
        // performed on a non-coordinator responsive without requiring catalogue
        // hint RPC forwarding; catalogue CAS/retry semantics resolve concurrent
        // additive updates. Only the namespace-wide destructive reconciliation
        // pass remains coordinator-owned.
        if (config.enabled && now >= next_hint_batch) {
            auto* budget_http = dynamic_cast<BudgetHttpClient*>(provider_http_.get());
            if (!budget_http) throw std::runtime_error("catalogue provider HTTP budget unavailable");
            if (!provider_budget_open) {
                budget_http->reset_budget(config.max_provider_requests_per_scan);
                provider_budget_open = true;
            }

            // A large old library may legitimately have hundreds of unbound
            // items requiring one real libav/tag/provider repair each. That work
            // is necessary, but it is background work: one expensive item must
            // not monopolise a core continuously. Measure this thread's actual
            // CPU for one scheduling unit and pace subsequent work to the same
            // CPU target used by the maintenance subsystem. Blocking network/I/O
            // time already counts as quiet time and therefore is not penalised.
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
            // A genuinely fresh scanner still needs an initial discovery. A
            // normal coordinator hand-off does not.
            mutation_due = now;
            if (!mutation_first_seen) mutation_first_seen = now;
        }
        was_coordinator = is_coordinator;

        const auto generation = node_.known_metadata_generation();
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
                    // Newly discovered low-priority hints should be eligible
                    // immediately after the reconciliation pass.
                    next_hint_batch = std::min(next_hint_batch, std::chrono::steady_clock::now());
                } else {
                    // A periodic safety pass first verifies the authoritative
                    // namespace identity. If it is byte-for-byte the same as the
                    // last successful reconciliation, walking every catalogue
                    // root cannot discover anything new. Advance the persisted
                    // safety deadline without reopening historical hints.
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
            // next_hint_batch==max is the worker's established no-pending-work
            // state. Do not even walk the terminal hint map merely to suppress a
            // zero-backlog line every 30 seconds.
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

        // Hint submission is event-driven. Keep a one-second ceiling only for
        // cheap coordinator/metadata-generation observation; do not linearly
        // scan the persisted negative-result map ten times per second while idle.
        const auto sleep_from = std::chrono::steady_clock::now();
        auto wake_at = sleep_from + std::chrono::seconds(1);
        if (config.enabled) {
            // Hint work is node-local and may wake every scanner. Periodic and
            // namespace-mutation reconciliation are coordinator-owned; an
            // overdue coordinator deadline on a non-coordinator must not turn
            // its idle loop into a zero-timeout spin. The one-second ceiling
            // remains the bounded coordinator/membership observation interval.
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
