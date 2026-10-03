// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "cluster/telemetry.hpp"

#include <compare>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace macha {

// Transcode rates this node has sustained per source class: the last finished
// generations' produced / producing media time (parked time excluded),
// persisted across restarts. Measured only; an unseen class is absent.
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
    mutable IoMutex mutex_;
    std::map<Key, std::deque<Observation>> observations_ MACHA_GUARDED_BY(mutex_);

    void load() MACHA_REQUIRES(mutex_);
    void persist_locked() const MACHA_REQUIRES(mutex_);
};

} // namespace macha
