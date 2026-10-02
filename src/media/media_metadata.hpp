// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/media_catalogue.hpp"

namespace macha {

// Engine-neutral metadata probing, so catalogue policy is testable without
// FFmpeg. media_metadata_ffmpeg.cpp registers the real provider at static-init
// time (set_embedded_music_metadata_provider()); with none registered these
// are no-ops returning nullopt.
struct EmbeddedMusicMetadataProvider {
    void (*apply)(FileSystem&, std::string_view, const FsEntry&, MediaProbe&,
                 std::vector<LocalArtworkCandidate>&, size_t max_artwork_bytes);
    std::optional<MediaProbe> (*probe_host)(const std::filesystem::path&);
};
void set_embedded_music_metadata_provider(EmbeddedMusicMetadataProvider);

void apply_embedded_music_metadata(FileSystem&, std::string_view path, const FsEntry&,
                                   MediaProbe&, std::vector<LocalArtworkCandidate>&,
                                   size_t max_artwork_bytes);
std::optional<MediaProbe> embedded_music_metadata_from_host(const std::filesystem::path&);

} // namespace macha
