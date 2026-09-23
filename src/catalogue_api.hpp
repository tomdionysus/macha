// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "http.hpp"

#include <chrono>
#include <functional>

namespace macha {

class CatalogueApi {
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    std::function<void(const std::vector<std::string>&)> request_media_rescan_;
    std::function<size_t(const std::vector<std::string>&)> request_media_profiles_;
    // Resolve a media profile now, at foreground priority, and persist it.
    // A client instructs from these facts, so "no profile yet" is not an
    // answer: either the stored copy is returned, or it is produced here
    // (operator, 2026-09-07). Absent only in fixtures without an engine.
    std::function<std::optional<MediaProbeResult>(const std::string&)> resolve_media_profile_;
    std::chrono::milliseconds artwork_capability_ttl_;
  public:
    CatalogueApi(
        CatalogueManager& catalogue, CatalogueHintQueue& hints,
        std::function<void(const std::vector<std::string>&)> request_media_rescan = {},
        std::function<size_t(const std::vector<std::string>&)> request_media_profiles = {},
        std::function<std::optional<MediaProbeResult>(const std::string&)> resolve_media_profile = {},
        std::chrono::milliseconds artwork_capability_ttl = std::chrono::hours(24 * 30))
        : catalogue_(catalogue), hints_(hints),
          request_media_rescan_(std::move(request_media_rescan)),
          request_media_profiles_(std::move(request_media_profiles)),
          resolve_media_profile_(std::move(resolve_media_profile)),
          artwork_capability_ttl_(artwork_capability_ttl) {}
    HttpResponse handle(const HttpRequest&);
    // Recognizes a request as a self-authorizing capability URL (currently:
    // an artwork GET carrying a valid, unexpired signature) so HttpServer can
    // exempt it from the ordinary bearer-token requirement. The signature
    // itself is verified here, not merely the URL shape, so an unsigned
    // request to the same path still falls through to the normal check.
    bool capability_request(const HttpRequest&) const;
};

} // namespace macha
