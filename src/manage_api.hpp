// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "filesystem.hpp"
#include "http.hpp"
#include "metadata_manager.hpp"
#include "media_catalogue.hpp"

#include <mutex>

namespace macha {

// Human management surface over MachaDFS and catalogue exception state.  This
// deliberately orchestrates the existing namespace/catalogue primitives rather
// than maintaining a second management database.
class ManageApi {
    NodeRuntime& node_;
    MetadataManager& metadata_;
    FileSystem& fs_;
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    CatalogueScanner& scanner_;
    std::mutex mutation_mutex_;

  public:
    ManageApi(NodeRuntime& node, MetadataManager& metadata, FileSystem& fs,
              CatalogueManager& catalogue, CatalogueHintQueue& hints, CatalogueScanner& scanner)
        : node_(node), metadata_(metadata), fs_(fs), catalogue_(catalogue), hints_(hints),
          scanner_(scanner) {}

    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
