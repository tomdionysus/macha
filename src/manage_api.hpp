// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "filesystem.hpp"
#include "http.hpp"
#include "metadata_manager.hpp"
#include "media_catalogue.hpp"

#include <mutex>
#include <condition_variable>
#include <map>
#include <thread>

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
    std::mutex identity_audit_mutex_;
    std::condition_variable_any identity_audit_cv_;
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_audit_pending_;
    std::jthread identity_audit_worker_;

    void queue_identity_reset_audit(IdentityAssociationReset);
    void identity_reset_audit_loop(std::stop_token);

  public:
    ManageApi(NodeRuntime& node, MetadataManager& metadata, FileSystem& fs,
              CatalogueManager& catalogue, CatalogueHintQueue& hints, CatalogueScanner& scanner);
    ~ManageApi();
    void request_stop();
    void stop();

    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
