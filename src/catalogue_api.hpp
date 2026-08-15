// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "http.hpp"

namespace macha {

class CatalogueApi {
    CatalogueManager& catalogue_;
  public:
    explicit CatalogueApi(CatalogueManager& catalogue) : catalogue_(catalogue) {}
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
