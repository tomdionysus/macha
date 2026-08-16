// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace macha {

enum class LogLevel : unsigned char {
    all = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
};

std::string_view log_level_name(LogLevel) noexcept;
LogLevel parse_log_level(std::string_view);

class Logger {
  public:
    virtual ~Logger() = default;

    virtual bool enabled(LogLevel) const noexcept = 0;
    virtual void log(LogLevel, const std::string&) = 0;

    void trace(const std::string& message) {
        if (enabled(LogLevel::all))
            log(LogLevel::all, message);
    }
    void debug(const std::string& message) {
        if (enabled(LogLevel::debug))
            log(LogLevel::debug, message);
    }
    void info(const std::string& message) {
        if (enabled(LogLevel::info))
            log(LogLevel::info, message);
    }
    void warn(const std::string& message) {
        if (enabled(LogLevel::warn))
            log(LogLevel::warn, message);
    }
    void error(const std::string& message) {
        if (enabled(LogLevel::error))
            log(LogLevel::error, message);
    }
};

class ConsoleLogger final : public Logger {
    LogLevel level_;
    mutable std::mutex mutex_;

  public:
    explicit ConsoleLogger(LogLevel level = LogLevel::info) : level_(level) {}

    bool enabled(LogLevel) const noexcept override;
    void log(LogLevel, const std::string&) override;
};

// Process-wide logger facade. The application installs its configured Logger
// once configuration has been parsed; the default is ConsoleLogger(INFO) so
// startup/configuration failures are still visible.
class Log {
    static std::mutex mutex_;
    static std::shared_ptr<Logger> logger_;
    static std::atomic<unsigned int> enabled_mask_;
    static std::shared_ptr<Logger> logger();

  public:
    static void set_logger(std::shared_ptr<Logger>);
    static bool enabled(LogLevel);

    static void trace(const std::string& value) {
        if (!enabled(LogLevel::all))
            return;
        logger()->log(LogLevel::all, value);
    }
    static void debug(const std::string& value) {
        if (!enabled(LogLevel::debug))
            return;
        logger()->log(LogLevel::debug, value);
    }
    static void info(const std::string& value) {
        if (!enabled(LogLevel::info))
            return;
        logger()->log(LogLevel::info, value);
    }
    static void warn(const std::string& value) {
        if (!enabled(LogLevel::warn))
            return;
        logger()->log(LogLevel::warn, value);
    }
    static void error(const std::string& value) {
        if (!enabled(LogLevel::error))
            return;
        logger()->log(LogLevel::error, value);
    }
};

} // namespace macha
