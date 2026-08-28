// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffmpeg_log.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace macha {

std::string_view ffmpeg_log_level_name(FfmpegLogLevel level) noexcept {
    switch (level) {
    case FfmpegLogLevel::quiet:
        return "QUIET";
    case FfmpegLogLevel::panic:
        return "PANIC";
    case FfmpegLogLevel::fatal:
        return "FATAL";
    case FfmpegLogLevel::error:
        return "ERROR";
    case FfmpegLogLevel::warning:
        return "WARNING";
    case FfmpegLogLevel::info:
        return "INFO";
    case FfmpegLogLevel::verbose:
        return "VERBOSE";
    case FfmpegLogLevel::debug:
        return "DEBUG";
    case FfmpegLogLevel::trace:
        return "TRACE";
    }
    return "?";
}

FfmpegLogLevel parse_ffmpeg_log_level(std::string_view value) {
    std::string level(value);
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (level == "QUIET")
        return FfmpegLogLevel::quiet;
    if (level == "PANIC")
        return FfmpegLogLevel::panic;
    if (level == "FATAL")
        return FfmpegLogLevel::fatal;
    if (level == "ERROR")
        return FfmpegLogLevel::error;
    if (level == "WARN" || level == "WARNING")
        return FfmpegLogLevel::warning;
    if (level == "INFO")
        return FfmpegLogLevel::info;
    if (level == "VERBOSE")
        return FfmpegLogLevel::verbose;
    if (level == "DEBUG")
        return FfmpegLogLevel::debug;
    if (level == "TRACE" || level == "ALL")
        return FfmpegLogLevel::trace;
    throw std::runtime_error("invalid FFmpeg log level: " + std::string(value) +
                             " (expected QUIET, PANIC, FATAL, ERROR, WARN, INFO, VERBOSE, DEBUG or TRACE)");
}


} // namespace macha
