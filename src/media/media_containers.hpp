// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace macha {

// The container vocabulary: what a file's name and probed format mean, the
// Content-Type for serving a probed source as-is, and which codecs each
// streaming container can carry as a copy. Facts about file formats only: no
// engine, file access or configuration. Adding a format is one row.

// Lowercased extension including the dot, or empty when the name has none.
std::string path_extension(std::string_view path);

bool video_extension(std::string_view ext);
bool audio_extension(std::string_view ext);

// The container a file's name claims, or empty.
std::string container_for_extension(std::string_view path);

// What the file is, not what it is called: the probed format wins over the
// extension, which only disambiguates a multi-container format
// ("matroska,webm") or stands in when the probe reported nothing. An unknown
// format is reported under libav's own name.
std::string container_for_format(std::string_view format, std::string_view path);

// Content-Type for serving a source unchanged, from its container and whether
// it has a picture (a video stream that is not cover art); then for the
// playlists, segments and cue files a session generates.
std::string direct_mime(std::string_view container, bool picture);
std::string segment_mime(std::string_view name);

// Which codecs each streaming container carries without re-encoding.
bool fmp4_video_copy_supported(std::string_view codec);
bool fmp4_audio_copy_supported(std::string_view codec);
bool mpegts_video_copy_supported(std::string_view codec);
bool mpegts_audio_copy_supported(std::string_view codec);

bool webvtt_subtitle_codec_supported(std::string_view codec);

} // namespace macha
