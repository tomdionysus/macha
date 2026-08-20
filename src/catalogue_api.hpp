// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "http.hpp"

#include <functional>

namespace macha {

class CatalogueApi {
    CatalogueManager& catalogue_;
    std::function<void()> request_rescan_;
  public:
    explicit CatalogueApi(CatalogueManager& catalogue, std::function<void()> request_rescan = {})
        : catalogue_(catalogue), request_rescan_(std::move(request_rescan)) {}
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
