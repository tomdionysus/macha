// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filesystem/filesystem.hpp"
#include "http/http.hpp"
#include "json.hpp"
#include "service/availability_service.hpp"

#include <string_view>

namespace macha {

// The availability facts of one path, or of nothing surveyed (`facts` null):
// extents, extents_local, extents_unavailable, extents_unknown, availability
// (complete, partial, unknown), surveyed_generation, surveyed_unix_ms. What
// the files resource and the media responses both carry.
void put_availability(Json::Object& out, const AvailabilitySnapshot* snapshot,
                      const PathAvailability* facts);
// The same for a file's content identity, wherever it sits in the namespace.
void put_media_availability(Json::Object& out, const AvailabilitySnapshot* snapshot,
                            std::string_view media_id);

// The namespace as a resource: GET /api/v1/files/<path> is a file or a
// directory with its entries, and GET /api/v1/files?hash=macha:<hash> the
// files with that content. Namespace facts are read from the filesystem;
// availability comes from the last survey and is never computed here.
class FilesApi {
  public:
    FilesApi(FileSystem&, const AvailabilityService&);
    HttpResponse handle(const HttpRequest&);

  private:
    FileSystem& fs_;
    const AvailabilityService& availability_;
};

} // namespace macha
