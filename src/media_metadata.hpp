// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "media_catalogue.hpp"

namespace macha {

// Concrete container/tag probing is a runtime adapter. Catalogue parsing and
// matching deliberately depend only on these engine-neutral helpers so the
// policy layer remains testable without FFmpeg development files installed.
void apply_embedded_music_metadata(FileSystem&, std::string_view path, const FsEntry&,
                                   MediaProbe&, std::vector<LocalArtworkCandidate>&,
                                   size_t max_artwork_bytes);
std::optional<MediaProbe> embedded_music_metadata_from_host(const std::filesystem::path&);

} // namespace macha
