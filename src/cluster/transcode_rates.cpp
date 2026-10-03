// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/transcode_rates.hpp"

#include "durable_file.hpp"
#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace macha {
namespace {

template <typename T> T median(std::vector<T> values) {
    if (values.empty()) return T{};
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

TranscodeRateBook::TranscodeRateBook(std::filesystem::path path) : path_(std::move(path)) {
    Lock lock(mutex_);
    load();
}

uint32_t TranscodeRateBook::height_class(int height) noexcept {
    for (const uint32_t bound : {576U, 720U, 1080U, 1440U, 2160U})
        if (height <= static_cast<int>(bound)) return bound;
    return 4320;
}

void TranscodeRateBook::record(const std::string& kind, const std::string& codec, uint32_t bit_depth,
                               uint32_t height_class, double rate, uint32_t concurrent) {
    if (!std::isfinite(rate) || rate <= 0.0 || codec.empty()) return;
    Lock lock(mutex_);
    auto& kept = observations_[Key{kind, codec, bit_depth, height_class}];
    const auto rate_milli = std::min(std::round(rate * 1000.0), 1e9);
    kept.push_back(Observation{static_cast<uint32_t>(rate_milli), concurrent});
    while (kept.size() > kept_observations) kept.pop_front();
    persist_locked();
}

std::vector<TranscodeRate> TranscodeRateBook::summary() const {
    Lock lock(mutex_);
    std::vector<TranscodeRate> out;
    for (const auto& [key, kept] : observations_) {
        if (kept.empty()) continue;
        std::vector<uint32_t> rates, concurrency;
        for (const auto& observation : kept) {
            rates.push_back(observation.rate_milli);
            concurrency.push_back(observation.concurrent);
        }
        out.push_back(TranscodeRate{key.kind, key.codec, key.bit_depth, key.height_class,
                                    median(rates), static_cast<uint32_t>(kept.size()),
                                    median(concurrency)});
    }
    return out;
}

void TranscodeRateBook::load() {
    std::ifstream in(path_);
    if (!in) return;
    try {
        std::stringstream text;
        text << in.rdbuf();
        const auto root = Json::parse(text.str());
        const auto* classes = root.find("classes");
        if (!classes || !classes->isArray()) return;
        for (const auto& entry : classes->asArray()) {
            Key key{entry.find("kind")->asString(), entry.find("codec")->asString(),
                    static_cast<uint32_t>(entry.find("bit_depth")->asUInt64()),
                    static_cast<uint32_t>(entry.find("height_class")->asUInt64())};
            auto& kept = observations_[key];
            for (const auto& observation : entry.find("observations")->asArray()) {
                const auto& pair = observation.asArray();
                if (pair.size() != 2) continue;
                kept.push_back(Observation{static_cast<uint32_t>(pair[0].asUInt64()),
                                           static_cast<uint32_t>(pair[1].asUInt64())});
            }
            while (kept.size() > kept_observations) kept.pop_front();
        }
    } catch (const std::exception& error) {
        // Observations are a cache of what the hardware did, not state the
        // node needs: start empty rather than refuse to start.
        Log::warn("transcode rates ignored: " + std::string(error.what()));
        observations_.clear();
    }
}

void TranscodeRateBook::persist_locked() const {
    Json::Array classes;
    for (const auto& [key, kept] : observations_) {
        Json::Array observations;
        for (const auto& observation : kept)
            observations.emplace_back(Json::Array{Json(static_cast<uint64_t>(observation.rate_milli)),
                                                  Json(static_cast<uint64_t>(observation.concurrent))});
        classes.emplace_back(Json::Object{{"kind", key.kind},
                                          {"codec", key.codec},
                                          {"bit_depth", static_cast<uint64_t>(key.bit_depth)},
                                          {"height_class", static_cast<uint64_t>(key.height_class)},
                                          {"observations", Json(std::move(observations))}});
    }
    try {
        std::filesystem::create_directories(path_.parent_path());
        durable_replace_file(path_, Json(Json::Object{{"schema_version", 1},
                                                      {"classes", Json(std::move(classes))}})
                                        .dump());
    } catch (const std::exception& error) {
        Log::warn("transcode rates not persisted: " + std::string(error.what()));
    }
}

} // namespace macha
