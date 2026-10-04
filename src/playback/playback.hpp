// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "config.hpp"
#include "contract/time_source.hpp"
#include "filesystem/filesystem.hpp"
#include "http/http.hpp"
#include "json.hpp"
#include "media/media_engine.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <functional>

namespace macha {

class MediaInformationService;

class PlaybackManager {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    // Adds what else is known of a media file to its entry in the media
    // listing (its availability); reads only, never waits.
    using MediaFacts = std::function<void(Json::Object& entry, std::string_view media_id)>;
    // `time`: what sessions' idle clocks and a failed start's retention are
    // measured by.
    PlaybackManager(FileSystem&, TranscodeRateBook&, RetainedMemoryLedger&, CatalogueManager&,
                    CatalogueApiConfig, StreamingConfig,
                    std::shared_ptr<MediaEngine> = {},
                    std::function<size_t(const std::vector<std::string>&)> request_media_profiles = {},
                    MediaInformationService* media_information = nullptr,
                    MediaFacts media_facts = {},
                    const TimeSource& time = steady_time_source());
    ~PlaybackManager();
    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    void start();
    void request_stop();
    void stop();
    void reconfigure(StreamingConfig);
    HttpResponse handle(const HttpRequest&);
    bool capability_request(const HttpRequest&) const;
};

} // namespace macha
