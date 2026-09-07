// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_containers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <set>

namespace macha {
namespace {

enum class Kind : uint8_t { video, audio };

bool same_token(std::string_view left, std::string_view right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                      [](unsigned char a, unsigned char b) {
                          return std::tolower(a) == std::tolower(b);
                      });
}

// One row per file name we recognise: the container the name claims, what to
// say in Content-Type when those bytes are served unchanged, and whether the
// catalogue should treat the file as video or as audio. An empty container or
// mime means the name does not say and something else must decide -- the
// probe for the container, application/octet-stream for the type.
struct ExtensionFact {
    std::string_view extension;
    std::string_view container;
    std::string_view mime;
    Kind kind;
};

constexpr std::array<ExtensionFact, 23> extension_facts{{
    {".mkv", "matroska", "video/x-matroska", Kind::video},
    {".mka", "matroska", "audio/x-matroska", Kind::audio},
    {".webm", "webm", "video/webm", Kind::video},
    // The mov/mp4/m4a family shares one demuxer and one container token.
    {".mp4", "mp4", "video/mp4", Kind::video},
    {".m4v", "mp4", "video/mp4", Kind::video},
    {".mov", "mp4", "video/mp4", Kind::video},
    {".m4a", "mp4", "audio/mp4", Kind::audio},
    {".avi", "avi", "video/x-msvideo", Kind::video},
    {".wmv", "asf", "video/x-ms-wmv", Kind::video},
    {".wma", "asf", "audio/x-ms-wma", Kind::audio},
    {".mpg", "mpeg", "video/mpeg", Kind::video},
    {".mpeg", "mpeg", "video/mpeg", Kind::video},
    {".ts", "mpegts", "video/mp2t", Kind::video},
    {".m2ts", "mpegts", "video/mp2t", Kind::video},
    {".mp3", "mp3", "audio/mpeg", Kind::audio},
    {".flac", "flac", "audio/flac", Kind::audio},
    {".ogg", "ogg", "audio/ogg", Kind::audio},
    {".oga", "ogg", "audio/ogg", Kind::audio},
    {".opus", "ogg", "audio/ogg", Kind::audio},
    {".aac", "adts", "audio/aac", Kind::audio},
    {".wav", "wav", "audio/wav", Kind::audio},
    {".aiff", "aiff", "audio/aiff", Kind::audio},
    // A name that states a codec and not a container. The probe decides.
    {".alac", "", "", Kind::audio},
}};

// libavformat lists every name a demuxer answers to ("matroska,webm",
// "mov,mp4,m4a,3gp,3g2,mj2", "mp3"), so each token is looked up separately
// and the file name disambiguates a family that names more than one.
struct FormatFact {
    std::string_view format;
    std::string_view container;
};

constexpr std::array<FormatFact, 18> format_facts{{
    {"matroska", "matroska"},
    {"webm", "webm"},
    {"mov", "mp4"},
    {"mp4", "mp4"},
    {"m4a", "mp4"},
    {"3gp", "mp4"},
    {"3g2", "mp4"},
    {"mj2", "mp4"},
    {"avi", "avi"},
    {"asf", "asf"},
    {"mpeg", "mpeg"},
    {"mpegts", "mpegts"},
    {"mp3", "mp3"},
    {"flac", "flac"},
    {"ogg", "ogg"},
    {"aac", "adts"},
    {"wav", "wav"},
    {"aiff", "aiff"},
}};

// What each streaming container carries as a copy.
//
// fMP4 takes AAC and Opus as they have always worked; (E-)AC-3 needs the
// muxer to parse a packet before it can write the dac3/dec3 sample-entry box,
// which is what `delay_moov` does. Measured on this libavformat: without it
// the header write fails "Invalid argument" (the 503s of 2026-09-07).
//
// MPEG-TS predates the fMP4 HLS arrangement and is where these codecs'
// carriage was first defined, so its list is the older, wider one: it is the
// route by which a 2017 television plays copied HEVC and E-AC-3 that it
// refuses in fMP4 (2026-09-07).
struct CarriageFact {
    std::string_view codec;
    Kind kind;
    bool fmp4;
    bool mpegts;
};

constexpr std::array<CarriageFact, 10> carriage_facts{{
    {"h264", Kind::video, true, true},
    {"hevc", Kind::video, true, true},
    {"av1", Kind::video, true, false},
    {"mpeg2video", Kind::video, false, true},
    {"aac", Kind::audio, true, true},
    {"ac3", Kind::audio, true, true},
    {"eac3", Kind::audio, true, true},
    {"opus", Kind::audio, true, false},
    {"mp3", Kind::audio, false, true},
    {"mp2", Kind::audio, false, true},
}};

// Subtitle codecs a session can convert to WebVTT cues.
constexpr std::array<std::string_view, 6> webvtt_source_codecs{
    "ass", "mov_text", "ssa", "subrip", "text", "webvtt"};

// Content-Type for the files a session generates rather than serves.
constexpr std::array<ExtensionFact, 5> generated_facts{{
    {".m3u8", "", "application/vnd.apple.mpegurl", Kind::video},
    {".m4s", "", "video/mp4", Kind::video},
    {".mp4", "", "video/mp4", Kind::video},
    {".ts", "", "video/mp2t", Kind::video},
    {".vtt", "", "text/vtt; charset=utf-8", Kind::video},
}};

const ExtensionFact* extension_fact(std::string_view ext) {
    for (const auto& fact : extension_facts)
        if (fact.extension == ext) return &fact;
    return nullptr;
}

std::string_view container_of_format_token(std::string_view token) {
    for (const auto& fact : format_facts)
        if (same_token(fact.format, token)) return fact.container;
    return {};
}

bool carries(std::string_view codec, Kind kind, bool CarriageFact::*container) {
    for (const auto& fact : carriage_facts)
        if (fact.kind == kind && same_token(fact.codec, codec)) return fact.*container;
    return false;
}

} // namespace

std::string path_extension(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string ext(path.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool video_extension(std::string_view ext) {
    const auto* fact = extension_fact(ext);
    return fact && fact->kind == Kind::video;
}

bool audio_extension(std::string_view ext) {
    const auto* fact = extension_fact(ext);
    return fact && fact->kind == Kind::audio;
}

std::string container_for_extension(std::string_view path) {
    const auto* fact = extension_fact(path_extension(path));
    return fact ? std::string(fact->container) : std::string{};
}

std::string container_for_format(std::string_view format, std::string_view path) {
    const auto by_name = container_for_extension(path);
    std::set<std::string_view> probed;
    std::string_view first_token;
    size_t begin = 0;
    while (begin <= format.size()) {
        const auto end = format.find(',', begin);
        const auto token = format.substr(begin, end == std::string_view::npos
                                                    ? format.size() - begin
                                                    : end - begin);
        if (!token.empty()) {
            if (first_token.empty()) first_token = token;
            if (const auto container = container_of_format_token(token); !container.empty())
                probed.insert(container);
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    if (!probed.empty()) {
        if (!by_name.empty() && probed.contains(by_name)) return by_name;
        return std::string(*probed.begin());
    }
    if (!by_name.empty()) return by_name;
    // Neither table knows this one. libav's own name for it is still a better
    // answer than silence: a client can print it, and it names the thing it
    // was handed.
    return std::string(first_token);
}

std::string direct_mime(std::string_view path) {
    const auto* fact = extension_fact(path_extension(path));
    if (!fact || fact->mime.empty()) return "application/octet-stream";
    return std::string(fact->mime);
}

std::string segment_mime(std::string_view name) {
    const auto ext = path_extension(name);
    for (const auto& fact : generated_facts)
        if (fact.extension == ext) return std::string(fact.mime);
    return "application/octet-stream";
}

bool fmp4_video_copy_supported(std::string_view codec) {
    return carries(codec, Kind::video, &CarriageFact::fmp4);
}

bool fmp4_audio_copy_supported(std::string_view codec) {
    return carries(codec, Kind::audio, &CarriageFact::fmp4);
}

bool mpegts_video_copy_supported(std::string_view codec) {
    return carries(codec, Kind::video, &CarriageFact::mpegts);
}

bool mpegts_audio_copy_supported(std::string_view codec) {
    return carries(codec, Kind::audio, &CarriageFact::mpegts);
}

bool webvtt_subtitle_codec_supported(std::string_view codec) {
    return std::any_of(webvtt_source_codecs.begin(), webvtt_source_codecs.end(),
                       [codec](std::string_view known) { return same_token(known, codec); });
}

} // namespace macha
