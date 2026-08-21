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
    std::function<void()> request_rescan_;
  public:
    CatalogueApi(CatalogueManager& catalogue, CatalogueHintQueue& hints,
                 std::function<void()> request_rescan = {})
        : catalogue_(catalogue), hints_(hints), request_rescan_(std::move(request_rescan)) {}
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
