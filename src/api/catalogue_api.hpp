// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "catalogue/catalogue_hints.hpp"
#include "http/http.hpp"

#include <chrono>
#include <functional>

namespace macha {

class CatalogueApi {
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    std::function<void(const std::vector<std::string>&)> request_media_rescan_;
    std::function<size_t(const std::vector<std::string>&)> request_media_profiles_;
    // Resolves a media profile now, at foreground priority, and persists it:
    // clients need the facts, so "no profile yet" is never an answer. Absent only
    // in fixtures without an engine.
    std::function<std::optional<MediaProbeResult>(const std::string&)> resolve_media_profile_;
    std::chrono::milliseconds artwork_capability_ttl_;
    // The size of a media id's file, when this node can find it.
    std::function<std::optional<uint64_t>(const std::string&)> media_size_;
    // A media id's keyframe byte index, stored or built now; empty when this
    // node cannot find the file.
    std::function<std::optional<Bytes>(const std::string&)> keyframe_index_;
  public:
    CatalogueApi(
        CatalogueManager& catalogue, CatalogueHintQueue& hints,
        std::function<void(const std::vector<std::string>&)> request_media_rescan = {},
        std::function<size_t(const std::vector<std::string>&)> request_media_profiles = {},
        std::function<std::optional<MediaProbeResult>(const std::string&)> resolve_media_profile = {},
        std::chrono::milliseconds artwork_capability_ttl = std::chrono::hours(24 * 30),
        std::function<std::optional<uint64_t>(const std::string&)> media_size = {},
        std::function<std::optional<Bytes>(const std::string&)> keyframe_index = {})
        : catalogue_(catalogue), hints_(hints),
          request_media_rescan_(std::move(request_media_rescan)),
          request_media_profiles_(std::move(request_media_profiles)),
          resolve_media_profile_(std::move(resolve_media_profile)),
          artwork_capability_ttl_(artwork_capability_ttl), media_size_(std::move(media_size)),
          keyframe_index_(std::move(keyframe_index)) {}
    HttpResponse handle(const HttpRequest&);
    // Recognises a self-authorising capability URL (an artwork GET with a valid,
    // unexpired signature) so HttpServer exempts it from the bearer-token check.
    // The signature is verified here, so an unsigned request to the same path
    // falls through to the normal check.
    bool capability_request(const HttpRequest&) const;
};

} // namespace macha
