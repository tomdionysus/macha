// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace macha {

// The container vocabulary: the one place that states what a file's name and
// its probed format mean, what to say in Content-Type when bytes are served
// as they are, and which codecs each streaming container can carry as a copy.
//
// These are facts about file formats. They are not facts about a node, a
// session or a client, so nothing here takes an engine type, opens a file or
// consults configuration -- a caller that has probed a source passes the
// probe's format name in and gets a string back. Adding a format is one row.
//
// It exists because the same truths were written down twice and disagreed:
// the catalogue admitted .avi, .wmv, .mpg, .ts and .m2ts as video while the
// playback container table knew none of them, so a session serving an AVI
// reported its container as the empty string (2026-09-07).

// Lowercased extension including the dot, or empty when the name has none.
std::string path_extension(std::string_view path);

// Whether a name is one this project treats as a video or an audio file.
bool video_extension(std::string_view ext);
bool audio_extension(std::string_view ext);

// The container a file's name claims, empty when the name claims nothing.
std::string container_for_extension(std::string_view path);

// What the file actually is, not what it is called. The probed format wins
// over the extension: a Matroska file named .mp4 must not be reported as mp4
// (the name is metadata, the container is fact). The extension only
// disambiguates a format that names several containers ("matroska,webm") and
// stands in when the probe reported nothing. A format this table does not
// know is reported under libav's own name for it rather than as nothing.
std::string container_for_format(std::string_view format, std::string_view path);

// Content-Type for serving a source file unchanged, and for the playlists,
// segments and cue files a session generates.
std::string direct_mime(std::string_view path);
std::string segment_mime(std::string_view name);

// Which codecs each streaming container carries as a copy. Asked of a media's
// streams to say what can be repackaged without re-encoding.
bool fmp4_video_copy_supported(std::string_view codec);
bool fmp4_audio_copy_supported(std::string_view codec);
bool mpegts_video_copy_supported(std::string_view codec);
bool mpegts_audio_copy_supported(std::string_view codec);

// Which subtitle codecs can be converted to WebVTT for a session.
bool webvtt_subtitle_codec_supported(std::string_view codec);

} // namespace macha
