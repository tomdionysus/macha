// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace macha {
namespace {
int rank(LogLevel level) {
    return static_cast<int>(level);
}
} // namespace

std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::all:
        return "ALL";
    case LogLevel::debug:
        return "DEBUG";
    case LogLevel::info:
        return "INFO";
    case LogLevel::warn:
        return "WARN";
    case LogLevel::error:
        return "ERROR";
    }
    return "?";
}

LogLevel parse_log_level(std::string_view value) {
    std::string level(value);
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (level == "ALL")
        return LogLevel::all;
    if (level == "DEBUG")
        return LogLevel::debug;
    if (level == "INFO")
        return LogLevel::info;
    if (level == "WARN" || level == "WARNING")
        return LogLevel::warn;
    if (level == "ERROR")
        return LogLevel::error;
    throw std::runtime_error("invalid log level: " + std::string(value) +
                             " (expected ALL, DEBUG, INFO, WARN or ERROR)");
}

bool ConsoleLogger::enabled(LogLevel level) const noexcept {
    return level_ == LogLevel::all || rank(level) >= rank(level_);
}

void ConsoleLogger::log(LogLevel level, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    // UTC, and it says so. This cluster spans timezones by design -- es-1 runs
    // CEST, fi-1 EEST, gbni-1 BST -- and every cross-node correlation anyone
    // does is a subtraction between two journals, by hand, usually during an
    // incident. Local time with no offset makes that a skill rather than a
    // lookup, and on 2026-09-21 it cost an hour: `18:45:44` on es-1 and
    // `19:45:44` on a client's screen were the same instant, and only a
    // recognisable event sequence made it cheap to spot.
    //
    // The operator's ruling the same day: "Macha absolutely needs to handle
    // multiple timezones across sites. They WILL be in different timezones.
    // We should be using hard Zulu, UTC."
    //
    // The API was already unambiguous -- everything on the wire is
    // `*_unix_ms`. This is the human-readable half catching up.
    gmtime_r(&time, &tm);

    std::lock_guard lock(mutex_);
    std::cerr << std::put_time(&tm, "%F %TZ") << ' ' << log_level_name(level) << ' ' << message
              << '\n';
}

std::mutex Log::mutex_;
std::shared_ptr<Logger> Log::logger_ = std::make_shared<ConsoleLogger>(LogLevel::info);
std::atomic<unsigned int> Log::enabled_mask_{
    (1U << static_cast<unsigned int>(LogLevel::info)) |
    (1U << static_cast<unsigned int>(LogLevel::warn)) |
    (1U << static_cast<unsigned int>(LogLevel::error))};

std::shared_ptr<Logger> Log::logger() {
    std::lock_guard lock(mutex_);
    return logger_;
}

void Log::set_logger(std::shared_ptr<Logger> logger) {
    if (!logger)
        throw std::invalid_argument("logger must not be null");

    unsigned int mask = 0;
    for (auto level : {LogLevel::all, LogLevel::debug, LogLevel::info, LogLevel::warn,
                       LogLevel::error}) {
        if (logger->enabled(level))
            mask |= 1U << static_cast<unsigned int>(level);
    }

    {
        std::lock_guard lock(mutex_);
        logger_ = std::move(logger);
    }
    enabled_mask_.store(mask, std::memory_order_release);
}

void Log::emit(LogLevel level, const std::string& value) {
    logger()->log(level, value);
}

bool Log::enabled(LogLevel level) {
    const auto bit = 1U << static_cast<unsigned int>(level);
    return (enabled_mask_.load(std::memory_order_acquire) & bit) != 0;
}

} // namespace macha
