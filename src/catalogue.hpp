// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "distributed_store.hpp"
#include "media_engine.hpp"
#include "metadata_manager.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
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
    auto operator<=>(const CatalogueItem&) const = default;
};

struct CatalogueSnapshot {
    std::map<std::string, CatalogueItem> items;
    struct MediaProfile {
        uint32_t schema_version{1};
        bool complete{true};
        MediaProbeResult probe;
        auto operator<=>(const MediaProfile&) const = default;
    };
    std::map<std::string, MediaProfile, std::less<>> media_profiles;
};

bool valid_catalogue_media_profile(std::string_view media_id,
                                   const CatalogueSnapshot::MediaProfile&);

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
    // DATA objects referenced by the catalogue. These participate in ordinary
    // DHT placement/repair and global reachability GC.
    std::set<ObjectId> live;
    // Catalogue manifest/shards are control-plane objects. They are protected
    // and swept in the dedicated control store, never by DATA placement.
    std::set<ObjectId> control_live;
    std::set<ObjectId> universal; // intentionally empty in the 0.18 storage model
    // False means current catalogue metadata could not be fully converged, so
    // the live set is conservative but incomplete and physical GC must not run.
    bool complete{true};
};

struct CatalogueRetentionObjects {
    std::vector<ObjectId> data;
    std::vector<ObjectId> control;
};

struct CatalogueArtworkContent {
    std::string mime_type;
    Bytes bytes;
};

struct CatalogueClearResult {
    size_t removed_items{};
    std::vector<std::string> media_ids;
};

struct ResolvedMediaProfile {
    MediaProbeResult probe;
    bool generated{};
    bool coalesced{};
};

Bytes encode_catalogue(const CatalogueSnapshot&);
CatalogueSnapshot decode_catalogue(std::span<const uint8_t>);
std::optional<CatalogueSnapshot> merge_catalogue_snapshots(
    const CatalogueSnapshot& base, const CatalogueSnapshot& left,
    const CatalogueSnapshot& right);
std::string catalogue_kind_name(CatalogueKind);
std::optional<CatalogueKind> parse_catalogue_kind(std::string_view);
std::vector<CatalogueArtwork> effective_catalogue_artwork(const CatalogueSnapshot&,
                                                           const CatalogueItem&);

class CatalogueConflict : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// The catalogue content/provider result is not at fault; the cluster cannot
// currently satisfy the control/DATA durability contract. Scanner work catches
// this separately and defers without consuming semantic failure attempts.
class CatalogueUnavailable : public std::runtime_error {
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
    // CONTROL objects for a successor catalogue are staged on metadata replicas
    // before metadata publication can reference them. Track both the current
    // catalogue-root epoch and the epoch in which each unreferenced object was
    // first observed by GC. An object cannot be reclaimed in that same epoch:
    // only observing a later catalogue root proves that it was not merely
    // data-before-metadata staging for the current root. The time point also
    // detects content-addressed objects re-affirmed after the current root.
    Clock::time_point control_gc_root_epoch_{};
    uint64_t control_gc_root_epoch_sequence_{};
    bool control_gc_root_epoch_initialized_{};
    std::map<ObjectId, uint64_t> control_gc_unreferenced_epoch_;
    bool ready_{};
    std::string error_;
    struct MediaProfileFlight {
        std::mutex mutex;
        std::condition_variable cv;
        bool complete{};
        std::optional<MediaProbeResult> result;
        std::exception_ptr error;
    };
    mutable std::mutex media_profile_mutex_;
    std::map<std::string, MediaProbeResult, std::less<>> resolved_media_profiles_;
    std::map<std::string, std::shared_ptr<MediaProfileFlight>, std::less<>> media_profile_flights_;
    LocalStore::Cursor control_gc_cursor_;
    std::optional<ObjectId> control_converged_root_;
    std::vector<NodeId> control_converged_nodes_;
    Clock::time_point control_convergence_retry_{};

    static std::set<ObjectId> artwork_ids(const CatalogueSnapshot&);
    size_t durability_required() const;
    CatalogueSnapshot load_root(const std::optional<ObjectId>&);
    bool converge_control_replicas(const MetadataSnapshot&);
    bool reconcile_catalogue_conflict(const MetadataSnapshotView&);
    void cache(uint64_t metadata_generation, const MetadataSnapshot&, CatalogueSnapshot);
    std::shared_ptr<const CatalogueSnapshot> current_snapshot();
    void commit(const std::optional<ObjectId>& expected_root, const CatalogueSnapshot& next,
                const std::set<ObjectId>& old_artwork,
                std::optional<Hash256> expected_namespace = std::nullopt,
                std::optional<std::pair<std::string, MetadataConflict>> resolved_conflict = {});

  public:
    CatalogueManager(NodeRuntime&, DistributedStore&, MetadataManager&);

    const ClusterKeys& cluster_keys() const noexcept { return node_.keys(); }

    void repair_once();
    bool refresh_needed() const;
    CatalogueStatus status() const;
    CatalogueSnapshot snapshot();
    std::shared_ptr<const CatalogueSnapshot> snapshot_view();
    std::optional<CatalogueItem> get(std::string_view id);
    std::optional<MediaProbeResult> media_profile(std::string_view media_id);
    ResolvedMediaProfile resolve_media_profile(
        std::string media_id, Clock::time_point deadline,
        std::function<MediaProbeResult()> generate);
    void put_media_profile(std::string media_id, MediaProbeResult profile);
    void put_media_profiles(std::map<std::string, MediaProbeResult, std::less<>> profiles);
    size_t prune_media_profiles(const std::set<std::string>& live_media_ids);
    std::vector<CatalogueItem> list(std::optional<CatalogueKind> kind = {},
                                    std::optional<std::string_view> parent = {});
    std::vector<CatalogueItem> search(std::string_view query, size_t limit = 50);
    CatalogueItem upsert(CatalogueItem, std::optional<uint64_t> expected_revision = {});
    std::vector<CatalogueItem> upsert_many(std::vector<CatalogueItem>);
    bool erase(std::string_view id, std::optional<uint64_t> expected_revision = {});
    bool definitely_absent(std::string_view id) const;
    CatalogueClearResult clear_metadata_with_media(
        std::string_view id, std::optional<uint64_t> expected_revision = {});
    size_t clear_metadata(std::string_view id, std::optional<uint64_t> expected_revision = {});
    CatalogueArtwork put_artwork(std::string_view item_id, std::string role,
                                 std::string mime_type, std::span<const uint8_t> bytes,
                                 std::optional<uint64_t> expected_revision = {});
    CatalogueArtwork stage_artwork(std::string role, std::string mime_type,
                                   std::span<const uint8_t> bytes);
    CatalogueArtwork stage_artwork_deferred(std::string role, std::string mime_type,
                                            std::span<const uint8_t> bytes,
                                            DistributedStore::DurabilityBatch& batch);
    bool artwork_durability_barrier(const DistributedStore::DurabilityBatch& batch);
    void reconcile_scanner(const std::vector<CatalogueItem>& discovered,
                           const std::set<std::string>& active_media_ids,
                           bool prune_missing = true,
                           std::optional<Hash256> expected_namespace = std::nullopt,
                           const std::map<std::string, MediaProbeResult, std::less<>>& profiles = {});
    std::optional<CatalogueArtworkContent> artwork(const ObjectId&);
    CatalogueMaintenance maintenance_objects();
    CatalogueRetentionObjects retention_objects(const std::optional<ObjectId>& old_root,
                                                 const std::optional<ObjectId>& new_root);
    size_t control_gc_step(const std::vector<ObjectId>& live,
                           std::chrono::milliseconds grace, size_t operation_budget = 32);
};

} // namespace macha
