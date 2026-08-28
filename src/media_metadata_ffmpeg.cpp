// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_metadata.hpp"

#include "log.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace macha {
namespace {

std::string trim(std::string value) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && ws(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && ws(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

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
        result = static_cast<int32_t>(std::stoi((*it).str()));
        break;
    }
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
        const auto read = state.handle->read(state.offset, std::span<uint8_t>(destination, wanted));
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
    if (bytes.size() >= 12 &&
        std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "RIFF" &&
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

} // namespace

void apply_embedded_music_metadata(FileSystem& fs, std::string_view path, const FsEntry& entry,
                                   MediaProbe& probe,
                                   std::vector<LocalArtworkCandidate>& artwork,
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

std::optional<MediaProbe> embedded_music_metadata_from_host(const std::filesystem::path& path) {
    AVFormatContext* format = nullptr;
    const auto opened = avformat_open_input(&format, path.string().c_str(), nullptr, nullptr);
    if (opened < 0 || !format) {
        if (format) avformat_close_input(&format);
        return std::nullopt;
    }
    MediaProbe probe;
    probe.kind = MediaProbeKind::track;
    probe.path = path.generic_string();
    std::vector<LocalArtworkCandidate> ignored_artwork;
    apply_format_audio_metadata(format, probe.path, probe, ignored_artwork, 0);
    avformat_close_input(&format);
    return probe;
}

} // namespace macha
