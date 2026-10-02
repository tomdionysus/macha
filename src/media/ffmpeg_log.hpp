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

// Installs the process-wide libav log callback and libav's own threshold.
// Admitted messages bypass Macha's log-level filter and reach the same sink
// at mapped severity, keeping the two verbosities independent.
void configure_ffmpeg_logging(FfmpegLogLevel);

} // namespace macha
