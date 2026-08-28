// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_metadata.hpp"

namespace macha {

void apply_embedded_music_metadata(FileSystem&, std::string_view, const FsEntry&,
                                   MediaProbe&, std::vector<LocalArtworkCandidate>&, size_t) {}

std::optional<MediaProbe> embedded_music_metadata_from_host(const std::filesystem::path&) {
    return std::nullopt;
}

} // namespace macha
