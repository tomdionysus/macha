// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "media_catalogue.hpp"

namespace macha {

// Concrete container/tag probing is a runtime adapter. Catalogue parsing and
// matching deliberately depend only on these engine-neutral helpers so the
// policy layer remains testable without FFmpeg development files installed.
// The real (FFmpeg-backed) implementation lives in media_metadata_ffmpeg.cpp,
// only linked into executables that carry an FFmpeg dependency; it registers
// itself into macha_core's provider slot at static-init time (see
// set_embedded_music_metadata_provider()) rather than being a link seam left
// for the final executable to resolve, since macha_core is now a shared
// library and must resolve its own symbols. With nothing registered, these
// two functions no-op/return nullopt -- the same behaviour the old
// media_metadata_stub.cpp default provided.
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
