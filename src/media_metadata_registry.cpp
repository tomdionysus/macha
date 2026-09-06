// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_metadata.hpp"

namespace macha {
namespace {
EmbeddedMusicMetadataProvider g_embedded_music_metadata_provider{nullptr, nullptr};
}

void set_embedded_music_metadata_provider(EmbeddedMusicMetadataProvider provider) {
    g_embedded_music_metadata_provider = provider;
}

void apply_embedded_music_metadata(FileSystem& fs, std::string_view path, const FsEntry& entry,
                                   MediaProbe& probe, std::vector<LocalArtworkCandidate>& artwork,
                                   size_t max_artwork_bytes) {
    if (g_embedded_music_metadata_provider.apply)
        g_embedded_music_metadata_provider.apply(fs, path, entry, probe, artwork, max_artwork_bytes);
}

std::optional<MediaProbe> embedded_music_metadata_from_host(const std::filesystem::path& path) {
    return g_embedded_music_metadata_provider.probe_host
               ? g_embedded_music_metadata_provider.probe_host(path)
               : std::nullopt;
}

} // namespace macha
