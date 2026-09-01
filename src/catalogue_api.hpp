// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "http.hpp"

#include <functional>

namespace macha {

class CatalogueApi {
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    std::function<void(const std::vector<std::string>&)> request_media_rescan_;
    std::function<size_t(const std::vector<std::string>&)> request_media_profiles_;
  public:
    CatalogueApi(
        CatalogueManager& catalogue, CatalogueHintQueue& hints,
        std::function<void(const std::vector<std::string>&)> request_media_rescan = {},
        std::function<size_t(const std::vector<std::string>&)> request_media_profiles = {})
        : catalogue_(catalogue), hints_(hints),
          request_media_rescan_(std::move(request_media_rescan)),
          request_media_profiles_(std::move(request_media_profiles)) {}
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
