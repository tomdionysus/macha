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
constexpr std::array<uint8_t, 8> magic{'M', 'C', 'A', 'T', '0', '0', '0', '1'};

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

void append_garbage(MetadataSnapshot& snapshot, const ObjectId& id) {
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
    }
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

size_t CatalogueManager::durability_required(const MetadataSnapshot& metadata, size_t active) {
    const size_t voters = std::max<size_t>(1, metadata.metadata_voters.size());
    return std::min(active, voters / 2 + 1);
}

CatalogueSnapshot CatalogueManager::load_root(const std::optional<ObjectId>& root) {
    if (!root)
        return {};
    if (!store_.ensure_metadata_local(*root))
        throw std::runtime_error("catalogue root object unavailable");
    auto data = node_.local_store().get(*root);
    if (!data)
        throw std::runtime_error("catalogue root object unavailable locally");
    return decode_catalogue(*data);
}

void CatalogueManager::cache(const MetadataRecord& record, const MetadataSnapshot& metadata,
                             CatalogueSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    cached_ = std::move(snapshot);
    cached_root_ = metadata.catalogue_root;
    cached_metadata_generation_ = record.generation;
    last_sync_unix_ms_ = unix_ms();
    ready_ = true;
    error_.clear();
}

void CatalogueManager::repair_once() {
    try {
        {
            std::lock_guard lock(mutex_);
            if (ready_ && cached_metadata_generation_ >= node_.known_metadata_generation())
                return;
        }
        auto record = metadata_.read_record();
        auto metadata = decode_snapshot(record.payload);
        {
            std::lock_guard lock(mutex_);
            if (ready_ && cached_root_ == metadata.catalogue_root) {
                // Filesystem namespace mutations advance the global metadata
                // generation far more often than the catalogue root changes.
                // The catalogue object itself is immutable/content-addressed,
                // so an unchanged root means the cached snapshot is still
                // exactly current. Record convergence without reloading it.
                cached_metadata_generation_ = record.generation;
                last_sync_unix_ms_ = unix_ms();
                error_.clear();
                return;
            }
        }
        auto snapshot = load_root(metadata.catalogue_root);
        cache(record, metadata, std::move(snapshot));
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        // A failed convergence attempt must not invalidate a catalogue snapshot
        // that was previously loaded successfully. API reads can continue from
        // that immutable root while background maintenance retries convergence.
        error_ = e.what();
        throw;
    }
}

CatalogueSnapshot CatalogueManager::current_snapshot() {
    {
        std::lock_guard lock(mutex_);
        if (ready_)
            return cached_;
    }
    // Only the cold/uninitialised path synchronises inline. Once a catalogue
    // root has been loaded, the service maintenance loop refreshes it in the
    // background instead of making UI reads participate in metadata quorum I/O.
    repair_once();
    std::lock_guard lock(mutex_);
    return cached_;
}

CatalogueStatus CatalogueManager::status() const {
    std::lock_guard lock(mutex_);
    CatalogueStatus status;
    status.enabled = true;
    status.metadata_generation = cached_metadata_generation_;
    status.root = cached_root_;
    status.items = cached_.items.size();
    auto art = artwork_ids(cached_);
    status.artwork_objects = art.size();
    for (const auto& id : art)
        status.local_artwork_objects += node_.local_store().has(id) ? 1 : 0;
    const bool root_local = !cached_root_ || node_.local_store().has(*cached_root_);
    status.ready = ready_ && root_local;
    status.last_sync_unix_ms = last_sync_unix_ms_;
    status.error = error_;
    return status;
}

CatalogueSnapshot CatalogueManager::snapshot() {
    return current_snapshot();
}

std::optional<CatalogueItem> CatalogueManager::get(std::string_view id) {
    auto snapshot = current_snapshot();
    auto it = snapshot.items.find(std::string(id));
    return it == snapshot.items.end() ? std::optional<CatalogueItem>{} : it->second;
}

std::vector<CatalogueItem> CatalogueManager::list(std::optional<CatalogueKind> kind,
                                                  std::optional<std::string_view> parent) {
    auto snapshot = current_snapshot();
    std::vector<CatalogueItem> out;
    for (const auto& [_, item] : snapshot.items) {
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
    for (const auto& [_, item] : snapshot.items) {
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
                              const std::set<ObjectId>& old_artwork) {
    auto encoded = encode_catalogue(next);
    auto root = object_id(encoded);
    auto metadata_record = metadata_.read_record();
    auto metadata_snapshot = decode_snapshot(metadata_record.payload);
    if (metadata_snapshot.catalogue_root != expected_root)
        throw CatalogueConflict("catalogue changed concurrently");

    const auto active = node_.membership().active();
    const auto required = durability_required(metadata_snapshot, active.size());
    auto new_artwork = artwork_ids(next);
    std::set<ObjectId> staged_artwork;
    std::set_difference(new_artwork.begin(), new_artwork.end(), old_artwork.begin(),
                        old_artwork.end(),
                        std::inserter(staged_artwork, staged_artwork.end()));

    auto cleanup_uncommitted = [&] {
        std::optional<ObjectId> live_root;
        std::set<ObjectId> live_artwork;
        try {
            auto latest_record = metadata_.read_record();
            auto latest_metadata = decode_snapshot(latest_record.payload);
            live_root = latest_metadata.catalogue_root;
            if (live_root)
                live_artwork = artwork_ids(load_root(live_root));
        } catch (...) {
            // If current reachability cannot be established, retain staged
            // content rather than risk deleting a concurrently committed object.
            return;
        }
        if (root != expected_root && live_root != root)
            store_.erase_all(root);
        for (const auto& id : staged_artwork) {
            if (!live_artwork.contains(id))
                store_.erase_all(id);
        }
    };

    const auto root_copies = store_.replicate_metadata_all(root, encoded);
    if (root_copies < required) {
        cleanup_uncommitted();
        throw std::runtime_error("catalogue root could not reach metadata durability quorum");
    }

    for (const auto& id : new_artwork) {
        auto data = node_.local_store().get(id);
        if (!data) {
            if (!store_.ensure_local(id, false)) {
                cleanup_uncommitted();
                throw std::runtime_error("referenced artwork object is unavailable: " +
                                         to_string(id));
            }
            data = node_.local_store().get(id);
        }
        if (!data || store_.replicate_all(id, *data, false) < required) {
            cleanup_uncommitted();
            throw std::runtime_error("artwork could not reach metadata durability quorum");
        }
    }

    try {
        metadata_.mutate([&](MetadataSnapshot& metadata) {
            if (metadata.catalogue_root != expected_root)
                throw CatalogueConflict("catalogue changed concurrently");
            if (metadata.catalogue_root && *metadata.catalogue_root != root)
                append_garbage(metadata, *metadata.catalogue_root);
            metadata.catalogue_root = root;
            for (const auto& id : old_artwork) {
                if (!new_artwork.contains(id))
                    append_garbage(metadata, id);
            }
        });
    } catch (...) {
        cleanup_uncommitted();
        throw;
    }

    auto committed_record = metadata_.read_record();
    auto committed_metadata = decode_snapshot(committed_record.payload);
    cache(committed_record, committed_metadata, next);
}

CatalogueItem CatalogueManager::upsert(CatalogueItem item,
                                        std::optional<uint64_t> expected_revision) {
    DiagnosticLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    auto current = current_snapshot();
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
    auto current = current_snapshot();
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

CatalogueArtwork CatalogueManager::stage_artwork(std::string role, std::string mime_type,
                                                   std::span<const uint8_t> bytes) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    CatalogueArtwork art{std::move(role), object_id(bytes), std::move(mime_type)};
    if (!node_.local_store().put(art.id, bytes))
        throw std::runtime_error("cannot stage artwork locally");
    return art;
}

void CatalogueManager::reconcile_scanner(const std::vector<CatalogueItem>& discovered,
                                         const std::set<std::string>& active_media_ids,
                                         bool prune_missing) {
    DiagnosticLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    auto current = current_snapshot();
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
            // Retain downloaded artwork when a provider result does not carry a
            // replacement for that role. Scanner refreshes are metadata-safe and
            // do not churn immutable artwork objects on every pass.
            for (const auto& art : it->second.artwork) {
                const bool replaced = std::any_of(item.artwork.begin(), item.artwork.end(),
                                                  [&](const auto& candidate) {
                                                      return candidate.role == art.role;
                                                  });
                if (!replaced) item.artwork.push_back(art);
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
        commit(expected_root, current, old_art);
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

    auto previous_art = item->artwork;
    std::erase_if(item->artwork, [&](const CatalogueArtwork& existing) {
        return existing.role == art.role;
    });
    item->artwork.push_back(art);
    try {
        (void)upsert(*item, item->revision);
    } catch (...) {
        const bool was_preexisting = std::any_of(previous_art.begin(), previous_art.end(),
                                                 [&](const auto& existing) {
                                                     return existing.id == art.id;
                                                 });
        if (!was_preexisting) {
            bool now_live = false;
            try {
                now_live = artwork_ids(current_snapshot()).contains(art.id);
            } catch (...) {
                now_live = true;
            }
            if (!now_live)
                (void)node_.local_store().remove(art.id);
        }
        throw;
    }
    return art;
}

std::optional<Bytes> CatalogueManager::artwork(const ObjectId& id) {
    auto current = current_snapshot();
    auto ids = artwork_ids(current);
    if (!ids.contains(id))
        return {};
    if (!store_.ensure_local(id, true))
        return {};
    return node_.local_store().get(id);
}

CatalogueMaintenance CatalogueManager::maintenance_objects() {
    // Maintenance liveness must be based on converged catalogue metadata, not
    // the deliberately stale-tolerant API cache returned by current_snapshot().
    // Otherwise an obsolete catalogue root/artwork object can remain marked
    // live indefinitely after a remote catalogue mutation, preventing GC.
    try {
        repair_once();
    } catch (...) {
        // Conservative failure semantics: if convergence is unavailable, retain
        // the last known catalogue objects rather than risk deleting live data.
    }
    std::lock_guard lock(mutex_);
    CatalogueMaintenance out;
    if (cached_root_) {
        out.live.insert(*cached_root_);
        out.universal.insert(*cached_root_);
    }
    for (const auto& id : artwork_ids(cached_)) {
        out.live.insert(id);
        out.universal.insert(id);
    }
    return out;
}

} // namespace macha
