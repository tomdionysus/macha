// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace macha {

enum class FfmpegLogLevel : unsigned char {
    quiet,
    panic,
    fatal,
    error,
    warning,
    info,
    verbose,
    debug,
    trace,
};

std::string_view ffmpeg_log_level_name(FfmpegLogLevel) noexcept;
FfmpegLogLevel parse_ffmpeg_log_level(std::string_view);

// Installs Macha's process-wide libav log callback and configures libav's own
// admission threshold. Messages admitted here bypass Macha's normal log-level
// filter and are written through the same logger sink with their mapped
// severity. This keeps Macha and FFmpeg verbosity independently configurable.
void configure_ffmpeg_logging(FfmpegLogLevel);

} // namespace macha
