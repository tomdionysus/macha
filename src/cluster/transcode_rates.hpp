// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/telemetry.hpp"

#include <compare>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace macha {

// What this node has sustained transcoding each kind of source: the last
// observations of a finished generation's produced / producing media time
// (parked time excluded), kept per class and persisted so a restart does not
// forget what the hardware can do. Measured, never estimated; a class never
// seen is simply absent.
class TranscodeRateBook {
  public:
    static constexpr size_t kept_observations = 16;

    explicit TranscodeRateBook(std::filesystem::path path);

    // One finished generation. `kind` is "video" (keyed by the source video
    // stream) or "audio" (audio-only transcodes, keyed by the audio codec).
    void record(const std::string& kind, const std::string& codec, uint32_t bit_depth,
                uint32_t height_class, double rate, uint32_t concurrent);
    std::vector<TranscodeRate> summary() const;

    // The height class a source falls in: 576, 720, 1080, 1440, 2160, 4320.
    static uint32_t height_class(int height) noexcept;

  private:
    struct Key {
        std::string kind;
        std::string codec;
        uint32_t bit_depth{};
        uint32_t height_class{};
        auto operator<=>(const Key&) const = default;
    };
    struct Observation {
        uint32_t rate_milli{};
        uint32_t concurrent{};
    };
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::map<Key, std::deque<Observation>> observations_;

    void load();
    void persist_locked() const;
};

} // namespace macha
