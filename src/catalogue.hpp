// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "distributed_store.hpp"
#include "metadata_manager.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace macha {

enum class CatalogueKind : uint8_t {
    movie = 1,
    show = 2,
    season = 3,
    episode = 4,
    artist = 5,
    album = 6,
    track = 7
};

struct CatalogueArtwork {
    std::string role;
    ObjectId id{};
    std::string mime_type;
    auto operator<=>(const CatalogueArtwork&) const = default;
};

struct CatalogueItem {
    std::string id;
    CatalogueKind kind{CatalogueKind::movie};
    std::string title;
    std::string sort_title;
    std::string synopsis;
    std::optional<std::string> parent_id;
    std::optional<int32_t> year;
    std::optional<int32_t> season_number;
    std::optional<int32_t> episode_number;
    std::optional<int32_t> disc_number;
    std::optional<int32_t> track_number;
    std::vector<std::string> aliases;
    std::map<std::string, std::string> external_ids;
    std::vector<std::string> media_ids;
    std::vector<CatalogueArtwork> artwork;
    uint64_t revision{1};
    int64_t updated_ns{};
};

struct CatalogueSnapshot {
    std::map<std::string, CatalogueItem> items;
};

struct CatalogueStatus {
    bool enabled{};
    bool ready{};
    uint64_t metadata_generation{};
    uint64_t known_metadata_generation{};
    std::optional<ObjectId> root;
    size_t items{};
    size_t artwork_objects{};
    size_t local_artwork_objects{};
    uint64_t last_sync_unix_ms{};
    std::string error;
};

struct CatalogueMaintenance {
    std::set<ObjectId> live;
    std::set<ObjectId> universal;
};

struct CatalogueArtworkContent {
    std::string mime_type;
    Bytes bytes;
};

Bytes encode_catalogue(const CatalogueSnapshot&);
CatalogueSnapshot decode_catalogue(std::span<const uint8_t>);
std::string catalogue_kind_name(CatalogueKind);
std::optional<CatalogueKind> parse_catalogue_kind(std::string_view);

class CatalogueConflict : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class CatalogueManager {
    NodeRuntime& node_;
    DistributedStore& store_;
    MetadataManager& metadata_;
    mutable std::mutex mutex_;
    mutable std::mutex refresh_mutex_;
    mutable std::mutex mutation_mutex_;
    std::shared_ptr<const CatalogueSnapshot> cached_;
    std::optional<ObjectId> cached_root_;
    uint64_t cached_metadata_generation_{};
    Clock::time_point cache_until_{};
    uint64_t last_sync_unix_ms_{};
    bool ready_{};
    std::string error_;

    static std::set<ObjectId> artwork_ids(const CatalogueSnapshot&);
    static size_t durability_required(const MetadataSnapshot&, size_t active);
    CatalogueSnapshot load_root(const std::optional<ObjectId>&);
    void cache(const MetadataRecord&, const MetadataSnapshot&, CatalogueSnapshot);
    std::shared_ptr<const CatalogueSnapshot> current_snapshot();
    void commit(const std::optional<ObjectId>& expected_root, const CatalogueSnapshot& next,
                const std::set<ObjectId>& old_artwork);

  public:
    CatalogueManager(NodeRuntime&, DistributedStore&, MetadataManager&);

    void repair_once();
    bool refresh_needed() const;
    CatalogueStatus status() const;
    CatalogueSnapshot snapshot();
    std::optional<CatalogueItem> get(std::string_view id);
    std::vector<CatalogueItem> list(std::optional<CatalogueKind> kind = {},
                                    std::optional<std::string_view> parent = {});
    std::vector<CatalogueItem> search(std::string_view query, size_t limit = 50);
    CatalogueItem upsert(CatalogueItem, std::optional<uint64_t> expected_revision = {});
    bool erase(std::string_view id, std::optional<uint64_t> expected_revision = {});
    CatalogueArtwork put_artwork(std::string_view item_id, std::string role,
                                 std::string mime_type, std::span<const uint8_t> bytes,
                                 std::optional<uint64_t> expected_revision = {});
    CatalogueArtwork stage_artwork(std::string role, std::string mime_type,
                                   std::span<const uint8_t> bytes);
    void reconcile_scanner(const std::vector<CatalogueItem>& discovered,
                           const std::set<std::string>& active_media_ids,
                           bool prune_missing = true);
    std::optional<CatalogueArtworkContent> artwork(const ObjectId&);
    CatalogueMaintenance maintenance_objects();
};

} // namespace macha
