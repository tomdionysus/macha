// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue.hpp"
#include "diagnostics.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
#include <limits>
#include <cctype>
#include <cmath>
#include <tuple>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> magic{'M', 'C', 'A', 'T', '0', '0', '1', '8'};
constexpr std::array<uint8_t, 8> manifest_magic{'M', 'C', 'R', 'O', 'O', 'T', '1', '8'};
constexpr size_t catalogue_shard_count = 64;

void optional_i32(Writer& w, const std::optional<int32_t>& value) {
    w.u8(value.has_value());
    if (value)
        w.u32(static_cast<uint32_t>(*value));
}

std::optional<int32_t> optional_i32(Reader& r) {
    if (!r.u8())
        return {};
    return static_cast<int32_t>(r.u32());
}

void string_vector(Writer& w, const std::vector<std::string>& values) {
    w.u32(values.size());
    for (const auto& value : values)
        w.string(value);
}

std::vector<std::string> string_vector(Reader& r, uint32_t maximum = 1000000) {
    auto count = r.u32();
    if (count > maximum)
        throw DecodeError("catalogue vector too large");
    std::vector<std::string> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(r.string(16 * 1024 * 1024));
    return out;
}

std::string normalize(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool space = true;
    for (unsigned char c : input) {
        if (c >= 0x80) {
            out.push_back(static_cast<char>(c));
            space = false;
        } else if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            space = false;
        } else if (!space) {
            out.push_back(' ');
            space = true;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

double score(std::string_view query, const CatalogueItem& item) {
    auto q = normalize(query);
    if (q.empty())
        return 0.0;
    auto title = normalize(item.title);
    if (title == q)
        return 1.0;
    if (title.starts_with(q))
        return 0.95;
    if (title.find(q) != std::string::npos)
        return 0.85;
    for (const auto& alias : item.aliases) {
        auto normalized = normalize(alias);
        if (normalized == q)
            return 0.93;
        if (normalized.find(q) != std::string::npos)
            return 0.78;
    }
    auto synopsis = normalize(item.synopsis);
    if (synopsis.find(q) != std::string::npos)
        return 0.55;
    for (const auto& [_, external] : item.external_ids) {
        if (normalize(external) == q)
            return 0.90;
    }
    return 0.0;
}

GarbageRef append_garbage(MetadataSnapshot& snapshot, const ObjectId& id) {
    auto existing = std::find_if(snapshot.garbage.begin(), snapshot.garbage.end(),
                                 [&](const GarbageRef& candidate) { return candidate.id == id; });
    auto retired = wall_time_ns();
    if (existing != snapshot.garbage.end()) {
        if (retired <= existing->retired_at_ns &&
            existing->retired_at_ns < std::numeric_limits<int64_t>::max())
            retired = existing->retired_at_ns + 1;
        existing->retired_at_ns = retired;
        existing->retirement_id = random_node_id();
    } else {
        snapshot.garbage.push_back({id, retired, random_node_id()});
        existing = std::prev(snapshot.garbage.end());
    }
    return *existing;
}

void record_garbage_upsert(MetadataDelta& delta, const GarbageRef& garbage) {
    auto existing = std::find_if(delta.upsert_garbage.begin(), delta.upsert_garbage.end(),
                                 [&](const GarbageRef& value) { return value.id == garbage.id; });
    if (existing == delta.upsert_garbage.end())
        delta.upsert_garbage.push_back(garbage);
    else
        *existing = garbage;
}


struct CatalogueManifest {
    std::array<std::optional<ObjectId>, catalogue_shard_count> shards;
};

size_t catalogue_shard(std::string_view id) {
    const auto hash = sha256({reinterpret_cast<const uint8_t*>(id.data()), id.size()});
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i)
        value = (value << 8) | hash.bytes[i];
    return value % catalogue_shard_count;
}

Bytes encode_catalogue_manifest(const CatalogueManifest& manifest) {
    Writer writer;
    writer.raw(manifest_magic);
    writer.u32(catalogue_shard_count);
    for (const auto& shard : manifest.shards) {
        writer.u8(shard.has_value());
        if (shard) writer.fixed(shard->bytes);
    }
    return writer.take();
}

CatalogueManifest decode_catalogue_manifest(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    const auto magic_bytes = reader.raw(manifest_magic.size());
    if (!std::equal(magic_bytes.begin(), magic_bytes.end(), manifest_magic.begin()))
        throw DecodeError("bad catalogue manifest");
    if (reader.u32() != catalogue_shard_count)
        throw DecodeError("unsupported catalogue shard count");
    CatalogueManifest manifest;
    for (auto& shard : manifest.shards) {
        if (reader.u8()) shard = ObjectId{reader.fixed<32>()};
    }
    reader.finish();
    return manifest;
}

std::array<CatalogueSnapshot, catalogue_shard_count>
shard_catalogue(const CatalogueSnapshot& snapshot) {
    std::array<CatalogueSnapshot, catalogue_shard_count> shards;
    for (const auto& [id, item] : snapshot.items)
        shards[catalogue_shard(id)].items.emplace(id, item);
    return shards;
}

bool valid_kind(uint8_t value) {
    return value >= static_cast<uint8_t>(CatalogueKind::movie) &&
           value <= static_cast<uint8_t>(CatalogueKind::track);
}

} // namespace

Bytes encode_catalogue(const CatalogueSnapshot& snapshot) {
    Writer w;
    w.raw(magic);
    w.u32(snapshot.items.size());
    for (const auto& [key, item] : snapshot.items) {
        if (key != item.id || item.id.empty())
            throw std::runtime_error("invalid catalogue item identity");
        w.string(item.id);
        w.u8(static_cast<uint8_t>(item.kind));
        w.string(item.title);
        w.string(item.sort_title);
        w.string(item.synopsis);
        w.u8(item.parent_id.has_value());
        if (item.parent_id)
            w.string(*item.parent_id);
        optional_i32(w, item.year);
        optional_i32(w, item.season_number);
        optional_i32(w, item.episode_number);
        optional_i32(w, item.disc_number);
        optional_i32(w, item.track_number);
        string_vector(w, item.aliases);
        w.u32(item.external_ids.size());
        for (const auto& [provider, id] : item.external_ids) {
            w.string(provider);
            w.string(id);
        }
        string_vector(w, item.media_ids);
        w.u32(item.artwork.size());
        for (const auto& art : item.artwork) {
            w.string(art.role);
            w.fixed(art.id.bytes);
            w.string(art.mime_type);
        }
        w.u64(item.revision);
        w.i64(item.updated_ns);
    }
    return w.take();
}

CatalogueSnapshot decode_catalogue(std::span<const uint8_t> data) {
    Reader r(data);
    auto m = r.raw(magic.size());
    if (!std::equal(m.begin(), m.end(), magic.begin()))
        throw DecodeError("bad catalogue snapshot");
    auto count = r.u32();
    if (count > 1000000)
        throw DecodeError("too many catalogue items");
    CatalogueSnapshot snapshot;
    for (uint32_t i = 0; i < count; ++i) {
        CatalogueItem item;
        item.id = r.string(1024 * 1024);
        auto kind = r.u8();
        if (!valid_kind(kind))
            throw DecodeError("bad catalogue kind");
        item.kind = static_cast<CatalogueKind>(kind);
        item.title = r.string(16 * 1024 * 1024);
        item.sort_title = r.string(16 * 1024 * 1024);
        item.synopsis = r.string(64 * 1024 * 1024);
        if (r.u8())
            item.parent_id = r.string(1024 * 1024);
        item.year = optional_i32(r);
        item.season_number = optional_i32(r);
        item.episode_number = optional_i32(r);
        item.disc_number = optional_i32(r);
        item.track_number = optional_i32(r);
        item.aliases = string_vector(r);
        auto external_count = r.u32();
        if (external_count > 100000)
            throw DecodeError("too many external ids");
        for (uint32_t j = 0; j < external_count; ++j) {
            auto provider = r.string(1024 * 1024);
            auto external_id = r.string(1024 * 1024);
            item.external_ids.emplace(std::move(provider), std::move(external_id));
        }
        item.media_ids = string_vector(r);
        auto artwork_count = r.u32();
        if (artwork_count > 100000)
            throw DecodeError("too much artwork metadata");
        item.artwork.reserve(artwork_count);
        for (uint32_t j = 0; j < artwork_count; ++j) {
            CatalogueArtwork art;
            art.role = r.string(1024 * 1024);
            art.id.bytes = r.fixed<32>();
            art.mime_type = r.string(1024 * 1024);
            item.artwork.push_back(std::move(art));
        }
        item.revision = r.u64();
        item.updated_ns = r.i64();
        if (item.id.empty() || !item.revision || !snapshot.items.emplace(item.id, item).second)
            throw DecodeError("invalid or duplicate catalogue item");
    }
    r.finish();
    return snapshot;
}

std::string catalogue_kind_name(CatalogueKind kind) {
    switch (kind) {
    case CatalogueKind::movie: return "movie";
    case CatalogueKind::show: return "show";
    case CatalogueKind::season: return "season";
    case CatalogueKind::episode: return "episode";
    case CatalogueKind::artist: return "artist";
    case CatalogueKind::album: return "album";
    case CatalogueKind::track: return "track";
    }
    return "unknown";
}

std::optional<CatalogueKind> parse_catalogue_kind(std::string_view value) {
    if (value == "movie") return CatalogueKind::movie;
    if (value == "show") return CatalogueKind::show;
    if (value == "season") return CatalogueKind::season;
    if (value == "episode") return CatalogueKind::episode;
    if (value == "artist") return CatalogueKind::artist;
    if (value == "album") return CatalogueKind::album;
    if (value == "track") return CatalogueKind::track;
    return {};
}

std::vector<CatalogueArtwork> effective_catalogue_artwork(const CatalogueSnapshot& snapshot,
                                                           const CatalogueItem& item) {
    if (!item.artwork.empty())
        return item.artwork;

    if (item.kind == CatalogueKind::track) {
        if (!item.parent_id)
            return {};
        auto parent = snapshot.items.find(*item.parent_id);
        if (parent == snapshot.items.end() || parent->second.kind != CatalogueKind::album)
            return {};
        return parent->second.artwork;
    }

    if (item.kind != CatalogueKind::artist)
        return {};

    const CatalogueItem* newest = nullptr;
    for (const auto& [_, candidate] : snapshot.items) {
        if (candidate.kind != CatalogueKind::album || !candidate.parent_id ||
            *candidate.parent_id != item.id || candidate.artwork.empty())
            continue;

        if (!newest) {
            newest = &candidate;
            continue;
        }

        const bool candidate_has_year = candidate.year.has_value();
        const bool newest_has_year = newest->year.has_value();
        if (candidate_has_year != newest_has_year) {
            if (candidate_has_year)
                newest = &candidate;
            continue;
        }
        if (candidate_has_year && candidate.year != newest->year) {
            if (*candidate.year > *newest->year)
                newest = &candidate;
            continue;
        }
        if (candidate.id < newest->id)
            newest = &candidate;
    }

    return newest ? newest->artwork : std::vector<CatalogueArtwork>{};
}

CatalogueManager::CatalogueManager(NodeRuntime& node, DistributedStore& store,
                                   MetadataManager& metadata)
    : node_(node), store_(store), metadata_(metadata) {}

std::set<ObjectId> CatalogueManager::artwork_ids(const CatalogueSnapshot& snapshot) {
    std::set<ObjectId> ids;
    for (const auto& [_, item] : snapshot.items)
        for (const auto& art : item.artwork)
            ids.insert(art.id);
    return ids;
}

size_t CatalogueManager::durability_required(const MetadataSnapshot& metadata) {
    if (metadata.metadata_voters.empty())
        throw std::runtime_error("catalogue metadata voter set is empty");
    return metadata.metadata_voters.size() / 2 + 1;
}

CatalogueSnapshot CatalogueManager::load_root(const std::optional<ObjectId>& root) {
    if (!root)
        return {};
    if (!store_.ensure_control_local(*root))
        throw CatalogueUnavailable("catalogue manifest unavailable");
    auto encoded_manifest = node_.control_store().get(*root);
    if (!encoded_manifest)
        throw CatalogueUnavailable("catalogue manifest unavailable locally");
    const auto manifest = decode_catalogue_manifest(*encoded_manifest);
    CatalogueSnapshot snapshot;
    for (const auto& shard_id : manifest.shards) {
        if (!shard_id) continue;
        if (!store_.ensure_control_local(*shard_id))
            throw CatalogueUnavailable("catalogue shard unavailable: " + to_string(*shard_id));
        auto encoded_shard = node_.control_store().get(*shard_id);
        if (!encoded_shard)
            throw CatalogueUnavailable("catalogue shard unavailable locally: " + to_string(*shard_id));
        auto shard = decode_catalogue(*encoded_shard);
        for (auto& [id, item] : shard.items) {
            if (!snapshot.items.emplace(id, std::move(item)).second)
                throw std::runtime_error("catalogue item appears in multiple shards");
        }
    }
    return snapshot;
}

bool CatalogueManager::converge_control_replicas(const MetadataSnapshot& metadata) {
    const auto root = metadata.catalogue_root;
    if (!root) {
        std::lock_guard lock(mutex_);
        control_converged_root_.reset();
        control_converged_voters_ = metadata.metadata_voters;
        control_convergence_retry_ = {};
        return true;
    }

    {
        std::lock_guard lock(mutex_);
        if (control_converged_root_ == root &&
            control_converged_voters_ == metadata.metadata_voters)
            return true;
        if (Clock::now() < control_convergence_retry_)
            return false;
    }

    try {
        if (!store_.ensure_control_local(*root))
            throw CatalogueUnavailable("catalogue manifest unavailable for control repair");
        auto encoded_manifest = node_.control_store().get(*root);
        if (!encoded_manifest)
            throw CatalogueUnavailable("catalogue manifest unavailable locally for control repair");
        const auto manifest = decode_catalogue_manifest(*encoded_manifest);

        std::vector<std::pair<ObjectId, Bytes>> objects;
        objects.reserve(catalogue_shard_count + 1);
        objects.push_back({*root, std::move(*encoded_manifest)});
        for (const auto& shard_id : manifest.shards) {
            if (!shard_id) continue;
            if (!store_.ensure_control_local(*shard_id))
                throw CatalogueUnavailable("catalogue shard unavailable for control repair: " +
                                           to_string(*shard_id));
            auto encoded = node_.control_store().get(*shard_id);
            if (!encoded)
                throw CatalogueUnavailable("catalogue shard unavailable locally for control repair: " +
                                           to_string(*shard_id));
            objects.push_back({*shard_id, std::move(*encoded)});
        }

        bool complete = true;
        for (const auto& [id, encoded] : objects) {
            if (store_.replicate_control(id, encoded, metadata.metadata_voters) <
                metadata.metadata_voters.size())
                complete = false;
        }

        std::lock_guard lock(mutex_);
        if (complete) {
            control_converged_root_ = root;
            control_converged_voters_ = metadata.metadata_voters;
            control_convergence_retry_ = {};
        } else {
            // Publication only needs a metadata-voter majority. Missing/offline
            // voters are convergence debt and must not invalidate a readable
            // committed catalogue. Retry at a bounded cadence.
            control_converged_root_.reset();
            control_converged_voters_.clear();
            control_convergence_retry_ = Clock::now() + std::chrono::seconds(5);
        }
        return complete;
    } catch (...) {
        std::lock_guard lock(mutex_);
        control_converged_root_.reset();
        control_converged_voters_.clear();
        control_convergence_retry_ = Clock::now() + std::chrono::seconds(5);
        throw;
    }
}

void CatalogueManager::cache(uint64_t metadata_generation, const MetadataSnapshot& metadata,
                             CatalogueSnapshot snapshot) {
    auto cached = std::make_shared<const CatalogueSnapshot>(std::move(snapshot));
    std::lock_guard lock(mutex_);
    cached_ = std::move(cached);
    cached_root_ = metadata.catalogue_root;
    cached_metadata_generation_ = metadata_generation;
    cache_until_ = Clock::now() + node_.config().metadata_cache;
    last_sync_unix_ms_ = unix_ms();
    ready_ = true;
    error_.clear();
}

void CatalogueManager::repair_once() {
    // Catalogue refresh is single-flight. API workers can all observe the same
    // generation notice or TTL expiry at once; only one of them should perform
    // metadata quorum I/O and fetch/decode a replacement immutable root.
    std::lock_guard refresh_lock(refresh_mutex_);
    try {
        {
            std::lock_guard lock(mutex_);
            const auto now = Clock::now();
            if (ready_ && cached_metadata_generation_ >= node_.known_metadata_generation() &&
                now < cache_until_ && control_converged_root_ == cached_root_) {
                auto view = metadata_.available_snapshot_view();
                if (view && control_converged_voters_ == view->snapshot->metadata_voters)
                    return;
            }
        }

        // MetadataManager is the owner of authoritative/quorum metadata reads.
        // Catalogue convergence consumes the decoded immutable view it has
        // already established instead of independently repeating the same quorum
        // read whenever the catalogue TTL expires or a generation notice arrives.
        // A genuinely cold CatalogueManager may bootstrap MetadataManager once;
        // after that this path is strictly memory-only.
        auto view = metadata_.available_snapshot_view();
        if (!view) {
            (void)metadata_.read_record();
            view = metadata_.available_snapshot_view();
        }
        if (!view)
            throw std::runtime_error("catalogue metadata snapshot unavailable after successful read");

        // If a newer generation is merely known but has not yet been acquired,
        // leave convergence to MetadataManager::repair_once(). Do not create a
        // second quorum reader from CatalogueManager. refresh_needed() remains
        // true, so the catalogue will adopt the view immediately after metadata
        // maintenance publishes it.
        if (view->generation < node_.known_metadata_generation())
            return;

        const auto generation = view->generation;
        const auto& metadata = *view->snapshot;
        const bool control_converged = converge_control_replicas(metadata);
        {
            std::lock_guard lock(mutex_);
            if (ready_ && cached_root_ == metadata.catalogue_root) {
                // Filesystem namespace mutations advance the global metadata
                // generation far more often than the catalogue root changes.
                // The catalogue object itself is immutable/content-addressed,
                // so an unchanged root means the cached snapshot is still
                // exactly current. Record convergence without reloading it.
                cached_metadata_generation_ = generation;
                cache_until_ = Clock::now() + node_.config().metadata_cache;
                last_sync_unix_ms_ = unix_ms();
                if (control_converged)
                    error_.clear();
                else
                    error_ = "catalogue control replicas are converging";
                return;
            }
        }
        auto snapshot = load_root(metadata.catalogue_root);
        cache(generation, metadata, std::move(snapshot));
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        // A failed convergence attempt must not invalidate a catalogue snapshot
        // that was previously loaded successfully. Warm API reads can continue
        // from that immutable root while the next request/background pass retries.
        error_ = e.what();
        throw;
    }
}

std::shared_ptr<const CatalogueSnapshot> CatalogueManager::current_snapshot() {
    {
        std::lock_guard lock(mutex_);
        // Warm reads are deliberately memory-only. Catalogue convergence is a
        // control-plane/background responsibility; an API GET must never block
        // on metadata quorum I/O merely because a short validation TTL expired.
        if (ready_ && cached_)
            return cached_;
    }

    // A genuinely cold manager has no coherent snapshot to serve, so its first
    // read still has to establish one synchronously. Subsequent reads remain on
    // the immutable shared snapshot while background convergence replaces it.
    repair_once();

    std::lock_guard lock(mutex_);
    if (!ready_ || !cached_)
        throw std::runtime_error("catalogue unavailable");
    return cached_;
}

bool CatalogueManager::refresh_needed() const {
    std::lock_guard lock(mutex_);
    if (!ready_ || !cached_)
        return true;
    return cached_metadata_generation_ < node_.known_metadata_generation() ||
           Clock::now() >= cache_until_;
}

CatalogueStatus CatalogueManager::status() const {
    CatalogueStatus status;
    std::shared_ptr<const CatalogueSnapshot> cached;
    {
        std::lock_guard lock(mutex_);
        status.enabled = true;
        status.metadata_generation = cached_metadata_generation_;
        status.known_metadata_generation = node_.known_metadata_generation();
        status.root = cached_root_;
        status.items = cached_ ? cached_->items.size() : 0;
        status.ready = ready_;
        status.last_sync_unix_ms = last_sync_unix_ms_;
        status.error = error_;
        cached = cached_;
    }

    // Potentially large artwork walks and backend existence probes must not hold
    // the snapshot publication mutex; ordinary API reads only need that mutex
    // long enough to acquire the immutable shared snapshot.
    auto art = cached ? artwork_ids(*cached) : std::set<ObjectId>{};
    status.artwork_objects = art.size();
    for (const auto& id : art)
        status.local_artwork_objects += node_.local_store().has(id) ? 1 : 0;
    const bool root_local = !status.root || node_.control_store().has(*status.root);
    status.ready = status.ready && root_local;
    return status;
}

CatalogueSnapshot CatalogueManager::snapshot() {
    return *current_snapshot();
}

std::shared_ptr<const CatalogueSnapshot> CatalogueManager::snapshot_view() {
    return current_snapshot();
}

std::optional<CatalogueItem> CatalogueManager::get(std::string_view id) {
    auto snapshot = current_snapshot();
    auto it = snapshot->items.find(std::string(id));
    return it == snapshot->items.end() ? std::optional<CatalogueItem>{} : it->second;
}

std::vector<CatalogueItem> CatalogueManager::list(std::optional<CatalogueKind> kind,
                                                  std::optional<std::string_view> parent) {
    auto snapshot = current_snapshot();
    std::vector<CatalogueItem> out;
    for (const auto& [_, item] : snapshot->items) {
        if (kind && item.kind != *kind)
            continue;
        if (parent && (!item.parent_id || *item.parent_id != *parent))
            continue;
        out.push_back(item);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        auto as = a.sort_title.empty() ? a.title : a.sort_title;
        auto bs = b.sort_title.empty() ? b.title : b.sort_title;
        return std::tie(as, a.id) < std::tie(bs, b.id);
    });
    return out;
}

std::vector<CatalogueItem> CatalogueManager::search(std::string_view query, size_t limit) {
    auto snapshot = current_snapshot();
    std::vector<std::pair<double, CatalogueItem>> ranked;
    for (const auto& [_, item] : snapshot->items) {
        auto s = score(query, item);
        if (s > 0.0)
            ranked.emplace_back(s, item);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second.title < b.second.title;
    });
    std::vector<CatalogueItem> out;
    limit = std::min(limit, ranked.size());
    out.reserve(limit);
    for (size_t i = 0; i < limit; ++i)
        out.push_back(std::move(ranked[i].second));
    return out;
}

void CatalogueManager::commit(const std::optional<ObjectId>& expected_root,
                              const CatalogueSnapshot& next,
                              const std::set<ObjectId>& old_artwork,
                              std::optional<Hash256> expected_namespace) {
    MetadataRecord metadata_record;
    try {
        metadata_record = metadata_.read_record();
    } catch (const std::exception& e) {
        throw CatalogueUnavailable(std::string("catalogue metadata unavailable: ") + e.what());
    }
    auto metadata_snapshot = decode_snapshot(metadata_record.payload);
    if (metadata_snapshot.catalogue_root != expected_root)
        throw CatalogueConflict("catalogue changed concurrently");
    if (expected_namespace &&
        metadata_namespace_signature(metadata_snapshot) != *expected_namespace)
        throw CatalogueConflict("namespace changed during catalogue reconciliation");

    const auto required = durability_required(metadata_snapshot);
    const auto new_artwork = artwork_ids(next);

    // Artwork is ordinary immutable DATA. Validate only newly introduced references;
    // unchanged artwork was already proven by the committed catalogue. DHT placement,
    // fallback, replication and repair are exactly the same as for media objects.
    for (const auto& id : new_artwork) {
        if (old_artwork.contains(id)) continue;
        if (!store_.get(id, 0, FrameType::speculative))
            throw CatalogueUnavailable("referenced artwork object is unavailable: " + to_string(id));
    }

    CatalogueManifest old_manifest;
    if (expected_root) {
        if (!store_.ensure_control_local(*expected_root))
            throw CatalogueUnavailable("current catalogue manifest unavailable");
        auto encoded = node_.control_store().get(*expected_root);
        if (!encoded)
            throw CatalogueUnavailable("current catalogue manifest unavailable locally");
        old_manifest = decode_catalogue_manifest(*encoded);
    }

    CatalogueManifest manifest;
    const auto shards = shard_catalogue(next);
    std::vector<std::pair<ObjectId, Bytes>> changed_control;
    changed_control.reserve(catalogue_shard_count + 1);
    for (size_t i = 0; i < catalogue_shard_count; ++i) {
        if (shards[i].items.empty()) continue;
        auto encoded = encode_catalogue(shards[i]);
        const auto id = object_id(encoded);
        manifest.shards[i] = id;
        if (old_manifest.shards[i] != id)
            changed_control.push_back({id, std::move(encoded)});
    }

    auto encoded_manifest = encode_catalogue_manifest(manifest);
    const auto root = object_id(encoded_manifest);
    if (expected_root && *expected_root == root) {
        cache(metadata_record.generation, metadata_snapshot, next);
        return;
    }

    // A catalogue metadata commit may reference a control object only after a
    // majority of the configured metadata voters has durably stored it. Missing
    // voters make the mutation fail; DATA capacity is irrelevant to this path.
    for (const auto& [id, encoded] : changed_control) {
        if (store_.replicate_control(id, encoded, metadata_snapshot.metadata_voters) < required)
            throw CatalogueUnavailable("catalogue shard could not reach metadata durability quorum");
    }
    if (store_.replicate_control(root, encoded_manifest, metadata_snapshot.metadata_voters) < required)
        throw CatalogueUnavailable("catalogue manifest could not reach metadata durability quorum");

    try {
        metadata_.mutate_delta([&](MetadataSnapshot& metadata, MetadataDelta& delta) {
            if (metadata.catalogue_root != expected_root)
                throw CatalogueConflict("catalogue changed concurrently");
            if (expected_namespace &&
                metadata_namespace_signature(metadata) != *expected_namespace)
                throw CatalogueConflict("namespace changed during catalogue reconciliation");
            metadata.catalogue_root = root;
            delta.catalogue = CatalogueDelta::set;
            delta.catalogue_root = root;
            // Obsolete manifest/shard objects are reclaimed by the dedicated control
            // store reachability sweep after the same grace period as DATA orphans.
            for (const auto& id : old_artwork) {
                if (!new_artwork.contains(id))
                    record_garbage_upsert(delta, append_garbage(metadata, id));
            }
        });
    } catch (const CatalogueConflict&) {
        throw;
    } catch (const std::exception& e) {
        throw CatalogueUnavailable(std::string("catalogue metadata durability unavailable: ") +
                                   e.what());
    }

    auto committed_record = metadata_.read_record();
    auto committed_metadata = decode_snapshot(committed_record.payload);
    {
        std::lock_guard lock(mutex_);
        control_converged_root_.reset();
        control_converged_voters_.clear();
        control_convergence_retry_ = {};
    }
    cache(committed_record.generation, committed_metadata, next);
}

CatalogueItem CatalogueManager::upsert(CatalogueItem item,
                                        std::optional<uint64_t> expected_revision) {
    DiagnosticLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    repair_once();
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        std::lock_guard lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_art = artwork_ids(current);
    auto it = current.items.find(item.id);
    if (item.id.empty())
        throw std::runtime_error("catalogue item id is required");
    if (it != current.items.end()) {
        if (expected_revision && it->second.revision != *expected_revision)
            throw CatalogueConflict("catalogue item revision changed");
        item.revision = it->second.revision + 1;
    } else {
        if (expected_revision)
            throw CatalogueConflict("catalogue item does not exist");
        item.revision = 1;
    }
    item.updated_ns = wall_time_ns();
    current.items[item.id] = item;
    commit(expected_root, current, old_art);
    return item;
}

bool CatalogueManager::erase(std::string_view id, std::optional<uint64_t> expected_revision) {
    DiagnosticLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    repair_once();
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        std::lock_guard lock(mutex_);
        expected_root = cached_root_;
    }
    auto it = current.items.find(std::string(id));
    if (it == current.items.end())
        return false;
    if (expected_revision && it->second.revision != *expected_revision)
        throw CatalogueConflict("catalogue item revision changed");
    auto old_art = artwork_ids(current);
    current.items.erase(it);
    commit(expected_root, current, old_art);
    return true;
}

bool CatalogueManager::definitely_absent(std::string_view id) const {
    std::optional<ObjectId> cached_root;
    uint64_t cached_generation{};
    {
        std::lock_guard lock(mutex_);
        if (!ready_ || !cached_)
            return false;
        if (cached_->items.contains(std::string(id)))
            return false;
        cached_root = cached_root_;
        cached_generation = cached_metadata_generation_;
    }

    // This is intentionally only an early-negative test. A stale catalogue
    // snapshot must never turn a potentially valid mutation into a false 404.
    const auto known_generation = node_.known_metadata_generation();
    if (cached_generation >= known_generation)
        return true;

    // Global metadata generations also advance for namespace, garbage and voter
    // changes. If MetadataManager has already decoded the known generation and
    // its immutable catalogue root is unchanged, the cached catalogue is still
    // exactly current even though its bookkeeping generation is older. This is
    // a memory-only proof and avoids a quorum repair for a definite 404.
    if (auto available = metadata_.available_snapshot_view();
        available && available->generation >= known_generation &&
        available->snapshot->catalogue_root == cached_root)
        return true;

    return false;
}

CatalogueClearResult CatalogueManager::clear_metadata_with_media(
    std::string_view id, std::optional<uint64_t> expected_revision) {
    DiagnosticLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    repair_once();
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        std::lock_guard lock(mutex_);
        expected_root = cached_root_;
    }

    auto root = current.items.find(std::string(id));
    if (root == current.items.end())
        return {};
    if (expected_revision && root->second.revision != *expected_revision)
        throw CatalogueConflict("catalogue item revision changed");

    // Clearing metadata is deliberately stronger than editing an item blank.
    // Remove the catalogue entity, and for hierarchy entities remove its
    // descendants as well. Preserve the immutable media identities before the
    // removal so the caller can enqueue only those files for re-enrichment.
    std::set<std::string> removed_ids{root->first};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& [candidate_id, candidate] : current.items) {
            if (removed_ids.contains(candidate_id) || !candidate.parent_id)
                continue;
            if (removed_ids.contains(*candidate.parent_id)) {
                removed_ids.insert(candidate_id);
                grew = true;
            }
        }
    }

    std::set<std::string> media_ids;
    for (const auto& remove_id : removed_ids) {
        auto item = current.items.find(remove_id);
        if (item != current.items.end())
            media_ids.insert(item->second.media_ids.begin(), item->second.media_ids.end());
    }

    auto old_art = artwork_ids(current);
    for (const auto& remove_id : removed_ids)
        current.items.erase(remove_id);
    commit(expected_root, current, old_art);

    CatalogueClearResult result;
    result.removed_items = removed_ids.size();
    result.media_ids.assign(media_ids.begin(), media_ids.end());
    return result;
}

size_t CatalogueManager::clear_metadata(std::string_view id,
                                        std::optional<uint64_t> expected_revision) {
    return clear_metadata_with_media(id, expected_revision).removed_items;
}

CatalogueArtwork CatalogueManager::stage_artwork(std::string role, std::string mime_type,
                                                   std::span<const uint8_t> bytes) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    CatalogueArtwork art{std::move(role), object_id(bytes), std::move(mime_type)};
    if (!store_.put(art.id, bytes))
        throw std::runtime_error("cannot store artwork in distributed DATA storage");
    return art;
}

CatalogueArtwork CatalogueManager::stage_artwork_deferred(
    std::string role, std::string mime_type, std::span<const uint8_t> bytes,
    DistributedStore::DurabilityBatch& batch) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    CatalogueArtwork art{std::move(role), object_id(bytes), std::move(mime_type)};
    if (!store_.put_deferred(art.id, bytes, batch))
        throw std::runtime_error("cannot stage artwork in distributed DATA storage");
    return art;
}

bool CatalogueManager::artwork_durability_barrier(
    const DistributedStore::DurabilityBatch& batch) {
    return store_.durability_barrier(batch);
}

void CatalogueManager::reconcile_scanner(const std::vector<CatalogueItem>& discovered,
                                         const std::set<std::string>& active_media_ids,
                                         bool prune_missing,
                                         std::optional<Hash256> expected_namespace) {
    DiagnosticLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    repair_once();
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        std::lock_guard lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_art = artwork_ids(current);
    bool changed = false;

    auto same_content = [](const CatalogueItem& a, const CatalogueItem& b) {
        return a.id == b.id && a.kind == b.kind && a.title == b.title &&
               a.sort_title == b.sort_title && a.synopsis == b.synopsis &&
               a.parent_id == b.parent_id && a.year == b.year &&
               a.season_number == b.season_number && a.episode_number == b.episode_number &&
               a.disc_number == b.disc_number && a.track_number == b.track_number &&
               a.aliases == b.aliases && a.external_ids == b.external_ids &&
               a.media_ids == b.media_ids && a.artwork == b.artwork;
    };

    for (auto item : discovered) {
        item.external_ids["macha_scanner"] = "1";
        auto it = current.items.find(item.id);
        if (it != current.items.end()) {
            const auto manual = it->second.external_ids.find("macha_metadata_locked");
            const bool metadata_locked =
                manual != it->second.external_ids.end() && manual->second == "1";
            if (metadata_locked) {
                // A user-edited catalogue item remains authoritative for descriptive
                // metadata. Scanner reconciliation still discovers additional local
                // media bindings, and the complete-scan pass below still removes
                // bindings that vanished from the namespace.
                auto preserved = it->second;
                preserved.media_ids.insert(preserved.media_ids.end(), item.media_ids.begin(),
                                           item.media_ids.end());
                std::sort(preserved.media_ids.begin(), preserved.media_ids.end());
                preserved.media_ids.erase(
                    std::unique(preserved.media_ids.begin(), preserved.media_ids.end()),
                    preserved.media_ids.end());
                if (preserved.media_ids == it->second.media_ids)
                    continue;
                ++preserved.revision;
                preserved.updated_ns = wall_time_ns();
                current.items[item.id] = std::move(preserved);
                changed = true;
                continue;
            }
            // Scanner artwork is a candidate set, not one slot per role. Preserve
            // every previously known immutable object unless the provider/local
            // scan already supplied the same role+object again. This allows, for
            // example, an embedded MP3 cover and a provider cover to coexist.
            for (const auto& art : it->second.artwork) {
                const bool already_present = std::any_of(
                    item.artwork.begin(), item.artwork.end(), [&](const auto& candidate) {
                        return candidate.role == art.role && candidate.id == art.id;
                    });
                if (!already_present) item.artwork.push_back(art);
            }
            // A provider match may represent another local file for an item
            // already known to the catalogue. Preserve existing bindings here;
            // the active-media reconciliation below removes vanished ones.
            item.media_ids.insert(item.media_ids.end(), it->second.media_ids.begin(),
                                  it->second.media_ids.end());
            std::sort(item.media_ids.begin(), item.media_ids.end());
            item.media_ids.erase(std::unique(item.media_ids.begin(), item.media_ids.end()),
                                 item.media_ids.end());
            item.revision = it->second.revision;
            item.updated_ns = it->second.updated_ns;
            if (same_content(item, it->second))
                continue;
            item.revision = it->second.revision + 1;
        } else {
            item.revision = 1;
        }
        item.updated_ns = wall_time_ns();
        current.items[item.id] = std::move(item);
        changed = true;
    }

    if (prune_missing) {
        // Only scanner-owned leaf bindings are reconciled against a complete
        // namespace scan. Manually-created catalogue entries are never removed.
        for (auto& [_, item] : current.items) {
            auto marker = item.external_ids.find("macha_scanner");
            if (marker == item.external_ids.end() || marker->second != "1")
                continue;
            if (item.kind != CatalogueKind::movie && item.kind != CatalogueKind::episode &&
                item.kind != CatalogueKind::track)
                continue;
            auto before = item.media_ids.size();
            std::erase_if(item.media_ids, [&](const std::string& media) {
                return !active_media_ids.contains(media);
            });
            if (item.media_ids.size() != before) {
                ++item.revision;
                item.updated_ns = wall_time_ns();
                changed = true;
            }
        }

        for (auto it = current.items.begin(); it != current.items.end();) {
            const auto marker = it->second.external_ids.find("macha_scanner");
            const bool scanner = marker != it->second.external_ids.end() && marker->second == "1";
            const bool leaf = it->second.kind == CatalogueKind::movie ||
                              it->second.kind == CatalogueKind::episode ||
                              it->second.kind == CatalogueKind::track;
            if (scanner && leaf && it->second.media_ids.empty()) {
                it = current.items.erase(it);
                changed = true;
            } else ++it;
        }

        // Remove now-empty scanner-created hierarchy nodes from the bottom up.
        bool removed = true;
        while (removed) {
            removed = false;
            for (auto it = current.items.begin(); it != current.items.end();) {
                const auto marker = it->second.external_ids.find("macha_scanner");
                const bool scanner = marker != it->second.external_ids.end() && marker->second == "1";
                const bool parent_kind = it->second.kind == CatalogueKind::show ||
                                         it->second.kind == CatalogueKind::season ||
                                         it->second.kind == CatalogueKind::artist ||
                                         it->second.kind == CatalogueKind::album;
                if (!scanner || !parent_kind) { ++it; continue; }
                const auto id = it->second.id;
                const bool has_child = std::any_of(current.items.begin(), current.items.end(),
                                                   [&](const auto& pair) {
                                                       return pair.second.parent_id &&
                                                              *pair.second.parent_id == id;
                                                   });
                if (!has_child) {
                    it = current.items.erase(it);
                    changed = removed = true;
                } else ++it;
            }
        }
    }

    if (changed)
        commit(expected_root, current, old_art, expected_namespace);
}

CatalogueArtwork CatalogueManager::put_artwork(std::string_view item_id, std::string role,
                                                std::string mime_type,
                                                std::span<const uint8_t> bytes,
                                                std::optional<uint64_t> expected_revision) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    auto item = get(item_id);
    if (!item)
        throw std::runtime_error("catalogue item not found");
    if (expected_revision && item->revision != *expected_revision)
        throw CatalogueConflict("catalogue item revision changed");

    CatalogueArtwork art = stage_artwork(std::move(role), std::move(mime_type), bytes);

    std::erase_if(item->artwork, [&](const CatalogueArtwork& existing) {
        return existing.role == art.role;
    });
    item->artwork.push_back(art);
    try {
        (void)upsert(*item, item->revision);
    } catch (...) {
        // stage_artwork() is content-addressed. A concurrent successful commit
        // may make this exact hash live at any point after our failed upsert, so
        // even local check-then-delete cleanup is unsafe. Leave failed staging
        // as an unreachable orphan for the grace-period reachability collector.
        throw;
    }
    return art;
}

std::optional<CatalogueArtworkContent> CatalogueManager::artwork(const ObjectId& id) {
    auto current = current_snapshot();
    std::optional<std::string> mime_type;
    for (const auto& [_, item] : current->items) {
        auto it = std::find_if(item.artwork.begin(), item.artwork.end(),
                               [&](const CatalogueArtwork& art) { return art.id == id; });
        if (it != item.artwork.end()) {
            mime_type = it->mime_type;
            break;
        }
    }
    if (!mime_type)
        return {};
    // Reading DATA must never require the reader to become an authoritative
    // owner. A full/small node can serve artwork directly from its DHT owner,
    // exactly as it can read a remotely placed media extent. The normal get()
    // path may use cache opportunistically without consuming DATA replica quota.
    auto bytes = store_.get(id, 0, FrameType::foreground);
    if (!bytes)
        return {};
    return CatalogueArtworkContent{std::move(*mime_type), std::move(*bytes)};
}

CatalogueMaintenance CatalogueManager::maintenance_objects() {
    // Maintenance liveness must be based on converged catalogue metadata, not
    // the deliberately stale-tolerant API cache returned by current_snapshot().
    // Otherwise an obsolete catalogue root/artwork object can remain marked
    // live indefinitely after a remote catalogue mutation, preventing GC.
    CatalogueMaintenance out;
    std::optional<ObjectId> metadata_root;
    uint64_t metadata_generation = 0;
    bool metadata_current = false;
    try {
        auto view = metadata_.available_snapshot_view();
        if (!view || view->generation < node_.known_metadata_generation()) {
            (void)metadata_.read_record();
            view = metadata_.available_snapshot_view();
        }
        if (view) {
            metadata_generation = view->generation;
            metadata_current = view->generation >= node_.known_metadata_generation();
            metadata_root = view->snapshot->catalogue_root;
            if (metadata_root) {
                // The metadata reference itself is unconditionally live even if
                // the immutable root cannot currently be fetched or decoded.
                out.control_live.insert(*metadata_root);
            }
        }
    } catch (...) {
        metadata_current = false;
    }

    bool repair_ok = true;
    try {
        repair_once();
    } catch (...) {
        repair_ok = false;
    }
    std::optional<ObjectId> root;
    std::shared_ptr<const CatalogueSnapshot> cached;
    {
        std::lock_guard lock(mutex_);
        root = cached_root_;
        cached = cached_;
    }

    if (root) {
        out.control_live.insert(*root);
        try {
            if (store_.ensure_control_local(*root)) {
                if (auto encoded = node_.control_store().get(*root)) {
                    const auto manifest = decode_catalogue_manifest(*encoded);
                    for (const auto& shard : manifest.shards)
                        if (shard) out.control_live.insert(*shard);
                }
            }
        } catch (...) {
            repair_ok = false;
        }
    }
    {
        std::lock_guard lock(mutex_);
        const bool root_converged = cached_root_ == metadata_root &&
                                    cached_metadata_generation_ >= metadata_generation;
        out.complete = metadata_current && repair_ok && root_converged;
    }
    if (!cached)
        return out;
    for (const auto& id : artwork_ids(*cached))
        out.live.insert(id);
    return out;
}

size_t CatalogueManager::control_gc_step(const std::vector<ObjectId>& live,
                                         std::chrono::milliseconds grace,
                                         size_t operation_budget) {
    if (!operation_budget) return 0;
    size_t removed = 0;
    bool exhausted = false;
    for (size_t operations = 0; operations < operation_budget && !exhausted; ++operations) {
        auto id = node_.control_store().next_object(control_gc_cursor_, exhausted);
        if (!id) continue;
        if (std::binary_search(live.begin(), live.end(), *id)) continue;
        if (node_.control_store().remove_if_older_than(*id, grace))
            ++removed;
    }
    if (exhausted && removed)
        (void)node_.control_store().compact_packs();
    return removed;
}

} // namespace macha
