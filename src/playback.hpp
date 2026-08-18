// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "config.hpp"
#include "filesystem.hpp"
#include "http.hpp"
#include "media_engine.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace macha {

class PlaybackManager {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    PlaybackManager(FileSystem&, CatalogueManager&, CatalogueApiConfig, StreamingConfig,
                    std::unique_ptr<MediaEngine> = {});
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
