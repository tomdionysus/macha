// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// A stored file this node can read, served as it is.
struct MediaContentSource {
    uint64_t size{};
    // The file's name, for the download's filename and its type.
    std::string name;
    // Reads at an offset; viewer class.
    std::function<size_t(uint64_t, std::span<uint8_t>)> read;
};

// One conversion of one media into one format, on this node.
struct ConversionState {
    enum class Phase { converting, ready, failed };
    Phase phase{Phase::converting};
    std::string media_id;
    std::string format;
    uint64_t size{};
    // When failed: conversion_failed, conversion_timed_out, source_unreadable,
    // source_too_large.
    std::string error_code;
};

inline const char* conversion_phase_name(ConversionState::Phase phase) noexcept {
    switch (phase) {
    case ConversionState::Phase::converting: return "converting";
    case ConversionState::Phase::ready: return "ready";
    case ConversionState::Phase::failed: return "failed";
    }
    return "failed";
}

// Converted copies of books, made on request and kept on this node. A build
// without book support has none, and the API answers conversion_unavailable.
class MediaConversions {
  public:
    virtual ~MediaConversions() = default;
    // The formats this node converts into.
    virtual std::vector<std::string> formats() const = 0;
    // Starts a conversion unless one is ready or running; a failed one is
    // tried again. `source` is the file to convert.
    virtual ConversionState request(const std::string& media_id, const std::string& format,
                                    const MediaContentSource& source) = 0;
    virtual std::optional<ConversionState> state(const std::string& media_id,
                                                 const std::string& format) = 0;
    // The converted file, when ready.
    virtual std::optional<MediaContentSource> content(const std::string& media_id,
                                                      const std::string& format) = 0;
    // Drops a ready or failed conversion; false when there is none.
    virtual bool erase(const std::string& media_id, const std::string& format) = 0;
};

} // namespace macha
