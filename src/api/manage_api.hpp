// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "catalogue/catalogue.hpp"
#include "catalogue/catalogue_hints.hpp"
#include "filesystem/filesystem.hpp"
#include "http/http.hpp"
#include "metadata/metadata_manager.hpp"
#include "catalogue/media_catalogue.hpp"

#include <mutex>
#include <condition_variable>
#include <map>
#include <thread>

namespace macha {

// Human management surface over MachaDFS and catalogue exception state,
// orchestrating the namespace/catalogue primitives rather than keeping a
// second management database.
class ManageApi {
    NodeRuntime& node_;
    MetadataView& metadata_;
    FileSystem& fs_;
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    CatalogueScanner& scanner_;
    Mutex identity_audit_mutex_;
    std::condition_variable_any identity_audit_cv_;
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_audit_pending_
        MACHA_GUARDED_BY(identity_audit_mutex_);
    std::jthread identity_audit_worker_;

    void queue_identity_reset_audit(IdentityAssociationReset);
    void identity_reset_audit_loop(std::stop_token);
    HttpResponse dispatch(const HttpRequest&);

  public:
    ManageApi(NodeRuntime& node, MetadataView& metadata, FileSystem& fs,
              CatalogueManager& catalogue, CatalogueHintQueue& hints, CatalogueScanner& scanner);
    ~ManageApi();
    void request_stop();
    void stop();

    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
