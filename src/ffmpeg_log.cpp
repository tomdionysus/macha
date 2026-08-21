// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffmpeg_log.hpp"

#include "log.hpp"

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <string>
#include <stdexcept>

namespace macha {
namespace {
int to_av_level(FfmpegLogLevel level) noexcept {
    switch (level) {
    case FfmpegLogLevel::quiet:
        return AV_LOG_QUIET;
    case FfmpegLogLevel::panic:
        return AV_LOG_PANIC;
    case FfmpegLogLevel::fatal:
        return AV_LOG_FATAL;
    case FfmpegLogLevel::error:
        return AV_LOG_ERROR;
    case FfmpegLogLevel::warning:
        return AV_LOG_WARNING;
    case FfmpegLogLevel::info:
        return AV_LOG_INFO;
    case FfmpegLogLevel::verbose:
        return AV_LOG_VERBOSE;
    case FfmpegLogLevel::debug:
        return AV_LOG_DEBUG;
    case FfmpegLogLevel::trace:
        return AV_LOG_TRACE;
    }
    return AV_LOG_ERROR;
}

LogLevel to_macha_level(int level) noexcept {
    if (level <= AV_LOG_ERROR)
        return LogLevel::error;
    if (level <= AV_LOG_WARNING)
        return LogLevel::warn;
    if (level <= AV_LOG_INFO)
        return LogLevel::info;
    if (level <= AV_LOG_DEBUG)
        return LogLevel::debug;
    return LogLevel::all;
}

void ffmpeg_log_callback(void* avcl, int level, const char* fmt, va_list vl) {
    // libav applies av_log_get_level() before invoking the callback in normal
    // use. Keep the guard here as well so the bridge has one explicit admission
    // rule even if the callback is invoked directly.
    if (level > av_log_get_level())
        return;

    thread_local int print_prefix = 1;
    char buffer[8192];
    va_list copy;
    va_copy(copy, vl);
    const int used = av_log_format_line2(avcl, level, fmt, copy, buffer, sizeof(buffer),
                                         &print_prefix);
    va_end(copy);
    if (used <= 0)
        return;

    const auto length = std::min<size_t>(static_cast<size_t>(used), sizeof(buffer) - 1);
    std::string formatted(buffer, length);
    size_t offset = 0;
    while (offset < formatted.size()) {
        const auto newline = formatted.find('\n', offset);
        const auto end = newline == std::string::npos ? formatted.size() : newline;
        auto line = formatted.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            Log::emit(to_macha_level(level), "ffmpeg: " + line);
        if (newline == std::string::npos)
            break;
        offset = newline + 1;
    }
}
} // namespace

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

void configure_ffmpeg_logging(FfmpegLogLevel level) {
    av_log_set_callback(ffmpeg_log_callback);
    av_log_set_level(to_av_level(level));
}

} // namespace macha
