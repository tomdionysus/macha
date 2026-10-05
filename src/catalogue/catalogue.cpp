// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/catalogue.hpp"
#include "diagnostics.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <cctype>
#include <cmath>
#include <tuple>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> legacy_magic{'M', 'C', 'A', 'T', '0', '0', '1', '8'};
// 0022 adds media indexes after the profiles; 0021 shards are still read.
constexpr std::array<uint8_t, 8> magic{'M', 'C', 'A', 'T', '0', '0', '2', '2'};
constexpr std::array<uint8_t, 8> magic_0021{'M', 'C', 'A', 'T', '0', '0', '2', '1'};
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

// One shard of a snapshot.
CatalogueSnapshot catalogue_shard_of(const CatalogueSnapshot& snapshot, size_t shard) {
    CatalogueSnapshot out;
    for (const auto& [id, item] : snapshot.items)
        if (catalogue_shard(id) == shard)
            out.items.emplace(id, item);
    for (const auto& [id, profile] : snapshot.media_profiles)
        if (catalogue_shard(id) == shard)
            out.media_profiles.emplace(id, profile);
    for (const auto& [id, index] : snapshot.media_indexes)
        if (catalogue_shard(id) == shard)
            out.media_indexes.emplace(id, index);
    return out;
}

// Marks the shard of every key at which two maps differ.
template <typename Map>
void mark_changed_shards(const Map& before, const Map& after,
                         std::array<bool, catalogue_shard_count>& changed) {
    auto a = before.begin();
    auto b = after.begin();
    while (a != before.end() || b != after.end()) {
        if (b == after.end() || (a != before.end() && a->first < b->first)) {
            changed[catalogue_shard(a->first)] = true;
            ++a;
        } else if (a == before.end() || b->first < a->first) {
            changed[catalogue_shard(b->first)] = true;
            ++b;
        } else {
            if (!(a->second == b->second))
                changed[catalogue_shard(a->first)] = true;
            ++a;
            ++b;
        }
    }
}

bool valid_kind(uint8_t value) {
    return value >= static_cast<uint8_t>(CatalogueKind::movie) &&
           value <= static_cast<uint8_t>(CatalogueKind::track);
}

bool valid_stream_type(uint8_t value) {
    return value <= static_cast<uint8_t>(MediaStreamType::other);
}

void encode_media_profile(Writer& w, const CatalogueSnapshot::MediaProfile& profile) {
    w.u32(profile.schema_version);
    w.u8(profile.complete);
    w.string(profile.probe.format);
    w.u64(std::bit_cast<uint64_t>(profile.probe.duration_seconds));
    w.u64(profile.probe.bitrate);
    w.u32(profile.probe.streams.size());
    for (const auto& stream : profile.probe.streams) {
        w.u32(static_cast<uint32_t>(stream.index));
        w.u8(static_cast<uint8_t>(stream.type));
        w.string(stream.codec);
        w.string(stream.profile);
        w.string(stream.language);
        w.u32(static_cast<uint32_t>(stream.width));
        w.u32(static_cast<uint32_t>(stream.height));
        w.u32(static_cast<uint32_t>(stream.channels));
        w.u32(static_cast<uint32_t>(stream.sample_rate));
        w.u32(static_cast<uint32_t>(stream.bit_depth));
        w.u8(stream.default_stream);
        w.u8(stream.forced);
        w.u64(stream.bitrate);
        w.u8(stream.attached_picture);
        if (profile.schema_version >= 2) {
            w.u32(static_cast<uint32_t>(std::max(0, stream.level)));
            w.string(stream.color_transfer);
        }
        if (profile.schema_version >= 3) {
            w.u32(static_cast<uint32_t>(std::max(0, stream.dolby_vision_profile)));
            w.u32(static_cast<uint32_t>(std::max(0, stream.dolby_vision_compatibility)));
        }
    }
}

CatalogueSnapshot::MediaProfile decode_media_profile(Reader& r) {
    CatalogueSnapshot::MediaProfile profile;
    profile.schema_version = r.u32();
    profile.complete = r.u8();
    profile.probe.format = r.string(1024 * 1024);
    profile.probe.duration_seconds = std::bit_cast<double>(r.u64());
    profile.probe.bitrate = r.u64();
    const auto count = r.u32();
    if (count > 100000) throw DecodeError("too many media streams");
    profile.probe.streams.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        MediaStreamInfo stream;
        stream.index = static_cast<int32_t>(r.u32());
        const auto type = r.u8();
        if (!valid_stream_type(type)) throw DecodeError("bad media stream type");
        stream.type = static_cast<MediaStreamType>(type);
        stream.codec = r.string(1024 * 1024);
        stream.profile = r.string(1024 * 1024);
        stream.language = r.string(1024 * 1024);
        stream.width = static_cast<int32_t>(r.u32());
        stream.height = static_cast<int32_t>(r.u32());
        stream.channels = static_cast<int32_t>(r.u32());
        stream.sample_rate = static_cast<int32_t>(r.u32());
        stream.bit_depth = static_cast<int32_t>(r.u32());
        stream.default_stream = r.u8();
        stream.forced = r.u8();
        stream.bitrate = r.u64();
        stream.attached_picture = r.u8();
        if (profile.schema_version >= 2) {
            stream.level = static_cast<int32_t>(r.u32());
            stream.color_transfer = r.string(256);
        }
        if (profile.schema_version >= 3) {
            stream.dolby_vision_profile = static_cast<int32_t>(r.u32());
            stream.dolby_vision_compatibility = static_cast<int32_t>(r.u32());
        }
        profile.probe.streams.push_back(std::move(stream));
    }
    return profile;
}

} // namespace

bool valid_catalogue_media_profile(
    std::string_view media_id, const CatalogueSnapshot::MediaProfile& profile) {
    if (!media_id.starts_with("macha:") || profile.schema_version < 1 ||
        profile.schema_version > catalogue_media_profile_schema || !profile.complete ||
        profile.probe.format.empty() || !std::isfinite(profile.probe.duration_seconds) ||
        profile.probe.duration_seconds < 0.0 || profile.probe.streams.empty())
        return false;
    std::set<int> indexes;
    for (const auto& stream : profile.probe.streams) {
        if (stream.index < 0 || !indexes.insert(stream.index).second || stream.codec.empty() ||
            stream.width < 0 || stream.height < 0 || stream.channels < 0 ||
            stream.sample_rate < 0 || stream.bit_depth < 0 || stream.level < 0)
            return false;
        // An older-schema profile lacks facts clients need for video, so it is
        // stale and regenerated on next playback; audio-only ones stand.
        if (profile.schema_version < catalogue_media_profile_schema &&
            stream.type == MediaStreamType::video && !stream.attached_picture)
            return false;
    }
    return true;
}

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
    w.u32(snapshot.media_profiles.size());
    for (const auto& [media_id, profile] : snapshot.media_profiles) {
        if (media_id.empty()) throw std::runtime_error("invalid catalogue media profile identity");
        w.string(media_id);
        encode_media_profile(w, profile);
    }
    w.u32(snapshot.media_indexes.size());
    for (const auto& [media_id, index] : snapshot.media_indexes) {
        if (media_id.empty()) throw std::runtime_error("invalid catalogue media index identity");
        w.string(media_id);
        w.fixed(index.bytes);
    }
    return w.take();
}

CatalogueSnapshot decode_catalogue(std::span<const uint8_t> data) {
    Reader r(data);
    auto m = r.raw(magic.size());
    const bool legacy = std::equal(m.begin(), m.end(), legacy_magic.begin());
    const bool v0021 = std::equal(m.begin(), m.end(), magic_0021.begin());
    if (!legacy && !v0021 && !std::equal(m.begin(), m.end(), magic.begin()))
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
    if (!legacy) {
        const auto profile_count = r.u32();
        if (profile_count > 1000000) throw DecodeError("too many media profiles");
        for (uint32_t i = 0; i < profile_count; ++i) {
            auto media_id = r.string(1024 * 1024);
            auto profile = decode_media_profile(r);
            if (media_id.empty() ||
                !snapshot.media_profiles.emplace(std::move(media_id), std::move(profile)).second)
                throw DecodeError("invalid or duplicate media profile identity");
        }
    }
    if (!legacy && !v0021) {
        const auto index_count = r.u32();
        if (index_count > 1000000) throw DecodeError("too many media indexes");
        for (uint32_t i = 0; i < index_count; ++i) {
            auto media_id = r.string(1024 * 1024);
            ObjectId index{r.fixed<32>()};
            if (media_id.empty() || !snapshot.media_indexes.emplace(std::move(media_id), index).second)
                throw DecodeError("invalid or duplicate media index identity");
        }
    }
    r.finish();
    return snapshot;
}

std::optional<CatalogueSnapshot> merge_catalogue_snapshots(
    const CatalogueSnapshot& base, const CatalogueSnapshot& left,
    const CatalogueSnapshot& right) {
    std::set<std::string> ids;
    for (const auto* source : {&base.items, &left.items, &right.items})
        for (const auto& [id, _] : *source)
            ids.insert(id);

    auto find_item = [](const auto& items, const std::string& id)
        -> std::optional<CatalogueItem> {
        auto found = items.find(id);
        if (found == items.end())
            return {};
        return found->second;
    };

    CatalogueSnapshot merged;
    for (const auto& id : ids) {
        const auto b = find_item(base.items, id);
        const auto l = find_item(left.items, id);
        const auto r = find_item(right.items, id);
        std::optional<CatalogueItem> selected;
        if (l == r)
            selected = l;
        else if (l == b)
            selected = r;
        else if (r == b)
            selected = l;
        else
            return {};
        if (selected)
            merged.items.emplace(id, std::move(*selected));
    }

    std::set<std::string> media_ids;
    for (const auto* source : {&base.media_profiles, &left.media_profiles, &right.media_profiles})
        for (const auto& [id, _] : *source) media_ids.insert(id);
    for (const auto& id : media_ids) {
        auto find = [&](const auto& profiles) -> std::optional<CatalogueSnapshot::MediaProfile> {
            auto it = profiles.find(id);
            return it == profiles.end() ? std::optional<CatalogueSnapshot::MediaProfile>{} : it->second;
        };
        const auto b = find(base.media_profiles), l = find(left.media_profiles), r = find(right.media_profiles);
        std::optional<CatalogueSnapshot::MediaProfile> selected;
        if (l == r) selected = l;
        else if (l == b) selected = r;
        else if (r == b) selected = l;
        else return {};
        if (selected) merged.media_profiles.emplace(id, std::move(*selected));
    }

    std::set<std::string> indexed;
    for (const auto* source : {&base.media_indexes, &left.media_indexes, &right.media_indexes})
        for (const auto& [id, _] : *source) indexed.insert(id);
    for (const auto& id : indexed) {
        auto find = [&](const auto& indexes) -> std::optional<ObjectId> {
            auto it = indexes.find(id);
            return it == indexes.end() ? std::optional<ObjectId>{} : it->second;
        };
        const auto b = find(base.media_indexes), l = find(left.media_indexes), r = find(right.media_indexes);
        std::optional<ObjectId> selected;
        if (l == r) selected = l;
        else if (l == b) selected = r;
        else if (r == b) selected = l;
        else return {};
        if (selected) merged.media_indexes.emplace(id, *selected);
    }

    // A parent deletion on one branch with a child change on the other is not
    // merged; the root conflict stays durable.
    for (const auto& [_, item] : merged.items)
        if (item.parent_id && !merged.items.contains(*item.parent_id))
            return {};
    return merged;
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

CatalogueManager::CatalogueManager(NodeRuntime& node, LocalState& local,
                                   MetadataServer& metadata_server, DistributedStore& store,
                                   MetadataView& metadata, const ObjectLedger& ledger)
    : node_(node), local_(local), ledger_(ledger), metadata_server_(metadata_server),
      store_(store), metadata_(metadata) {}

std::set<ObjectId> CatalogueManager::data_object_ids(const CatalogueSnapshot& snapshot) {
    std::set<ObjectId> ids;
    for (const auto& [_, item] : snapshot.items)
        for (const auto& art : item.artwork)
            ids.insert(art.id);
    for (const auto& [_, index] : snapshot.media_indexes)
        ids.insert(index);
    return ids;
}

CatalogueSnapshot CatalogueManager::load_root(const std::optional<ObjectId>& root) {
    if (!root)
        return {};
    if (!store_.ensure_control_local(*root))
        throw CatalogueUnavailable("catalogue manifest unavailable");
    auto encoded_manifest = local_.control().get(*root);
    if (!encoded_manifest)
        throw CatalogueUnavailable("catalogue manifest unavailable locally");
    const auto manifest = decode_catalogue_manifest(*encoded_manifest);
    CatalogueSnapshot snapshot;
    for (const auto& shard_id : manifest.shards) {
        if (!shard_id) continue;
        if (!store_.ensure_control_local(*shard_id))
            throw CatalogueUnavailable("catalogue shard unavailable: " + to_string(*shard_id));
        auto encoded_shard = local_.control().get(*shard_id);
        if (!encoded_shard)
            throw CatalogueUnavailable("catalogue shard unavailable locally: " + to_string(*shard_id));
        auto shard = decode_catalogue(*encoded_shard);
        for (auto& [id, item] : shard.items) {
            if (!snapshot.items.emplace(id, std::move(item)).second)
                throw std::runtime_error("catalogue item appears in multiple shards");
        }
        for (auto& [id, profile] : shard.media_profiles) {
            if (!snapshot.media_profiles.emplace(id, std::move(profile)).second)
                throw std::runtime_error("media profile appears in multiple shards");
        }
        for (auto& [id, index] : shard.media_indexes) {
            if (!snapshot.media_indexes.emplace(id, index).second)
                throw std::runtime_error("media index appears in multiple shards");
        }
    }
    return snapshot;
}

bool CatalogueManager::converge_control_replicas(const MetadataSnapshot& metadata) {
    const auto root = metadata.catalogue_root;
    std::vector<NodeId> active_nodes;
    for (const auto& node : node_.membership().active())
        active_nodes.push_back(node.id);
    std::sort(active_nodes.begin(), active_nodes.end());

    if (!root) {
        Lock lock(mutex_);
        control_converged_root_.reset();
        control_converged_nodes_ = std::move(active_nodes);
        control_convergence_retry_ = {};
        return true;
    }

    {
        Lock lock(mutex_);
        if (control_converged_root_ == root && control_converged_nodes_ == active_nodes)
            return true;
        if (Clock::now() < control_convergence_retry_)
            return false;
    }

    try {
        if (!store_.ensure_control_local(*root))
            throw CatalogueUnavailable("catalogue manifest unavailable for control repair");
        auto encoded_manifest = local_.control().get(*root);
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
            auto encoded = local_.control().get(*shard_id);
            if (!encoded)
                throw CatalogueUnavailable("catalogue shard unavailable locally for control repair: " +
                                           to_string(*shard_id));
            objects.push_back({*shard_id, std::move(*encoded)});
        }

        bool complete = true;
        for (const auto& [id, encoded] : objects) {
            if (store_.replicate_control(id, encoded) < active_nodes.size())
                complete = false;
        }

        Lock lock(mutex_);
        if (complete) {
            control_converged_root_ = root;
            control_converged_nodes_ = std::move(active_nodes);
            control_convergence_retry_ = {};
        } else {
            // Publication needs only metadata_write_copies copies; missing
            // replicas are convergence debt, not an invalid root.
            control_converged_root_.reset();
            control_converged_nodes_.clear();
            control_convergence_retry_ = Clock::now() + std::chrono::seconds(5);
        }
        return complete;
    } catch (...) {
        Lock lock(mutex_);
        control_converged_root_.reset();
        control_converged_nodes_.clear();
        control_convergence_retry_ = Clock::now() + std::chrono::seconds(5);
        throw;
    }
}
void CatalogueManager::cache(uint64_t metadata_generation, const MetadataSnapshot& metadata,
                             CatalogueSnapshot snapshot) {
    auto cached = std::make_shared<const CatalogueSnapshot>(std::move(snapshot));
    Lock lock(mutex_);
    const bool root_changed = !control_gc_root_epoch_initialized_ ||
                              cached_root_ != metadata.catalogue_root;
    cached_ = std::move(cached);
    cached_root_ = metadata.catalogue_root;
    cached_metadata_generation_ = metadata_generation;
    cache_until_ = Clock::now() + node_.config().metadata_cache;
    last_sync_unix_ms_ = unix_ms();
    if (root_changed) {
        control_gc_root_epoch_ = Clock::now();
        ++control_gc_root_epoch_sequence_;
        control_gc_root_epoch_initialized_ = true;
    }
    ready_ = true;
    error_.clear();
    error_code_.clear();
}

bool CatalogueManager::reconcile_catalogue_conflict(const MetadataSnapshotView& view) {
    for (const auto& [id, conflict] : view.snapshot->conflicts) {
        if (conflict.kind != MetadataConflictKind::catalogue_root)
            continue;

        auto base_root = conflict.base_catalogue_root;
        if (conflict.later_installed) {
            // A merge of two heads keeps no base: it is the root their common
            // ancestor held, while this node's history still reaches that.
            // Without it the root left in place stands.
            const auto ancestor =
                metadata_.common_ancestor_catalogue_root(conflict.left_head, conflict.right_head);
            if (!ancestor)
                continue;
            base_root = *ancestor;
        }
        const auto base = load_root(base_root);
        const auto left = load_root(conflict.left_catalogue_root);
        const auto right = load_root(conflict.right_catalogue_root);
        auto merged = merge_catalogue_snapshots(base, left, right);
        if (!merged)
            return false;

        commit(view.snapshot->catalogue_root, *merged, data_object_ids(base), {},
               std::make_pair(id, conflict));
        Log::info("catalogue branch conflict reconciled id=" + id +
                  " items=" + std::to_string(merged->items.size()));
        return true;
    }
    return false;
}

void CatalogueManager::repair_once() {
    refresh(true);
}

void CatalogueManager::refresh(bool converge) {
    // Single-flight: of the API workers seeing the same notice or TTL expiry,
    // only one does the replica I/O and decodes the new root.
    Lock refresh_lock(refresh_mutex_);
    try {
        if (!converge) {
            // A write or a read brings the catalogue to this node's own head
            // and offers nothing to the peers: that is maintenance's pass.
            Lock lock(mutex_);
            if (ready_ && cached_metadata_generation_ >= metadata_server_.known_generation() &&
                Clock::now() < cache_until_)
                return;
        } else {
            Lock lock(mutex_);
            const auto now = Clock::now();
            if (ready_ && cached_metadata_generation_ >= metadata_server_.known_generation() &&
                now < cache_until_ && control_converged_root_ == cached_root_) {
                std::vector<NodeId> active_nodes;
                for (const auto& node : node_.membership().active())
                    active_nodes.push_back(node.id);
                std::sort(active_nodes.begin(), active_nodes.end());
                if (control_converged_nodes_ == active_nodes)
                    return;
            }
        }

        // MetadataManager owns replicated metadata reads; this consumes its
        // decoded view. A cold manager may bootstrap it once; after that this
        // path is memory-only.
        auto view = metadata_.current();
        if (!view) {
            (void)metadata_.record();
            view = metadata_.current();
        }
        if (!view)
            throw std::runtime_error("catalogue metadata snapshot unavailable after successful read");

        // A newer generation known but not yet acquired is left to
        // MetadataManager::repair_once(); refresh_needed() stays true, so the
        // view is adopted once metadata maintenance publishes it.
        if (view->generation < metadata_server_.known_generation())
            return;

        // At most one root conflict per pass. Disjoint item merges are
        // automatic; same-item or parent/child collisions stay durable conflicts.
        if (reconcile_catalogue_conflict(*view)) {
            view = metadata_.current();
            if (!view)
                return;
        }

        const auto generation = view->generation;
        const auto& metadata = *view->snapshot;
        const std::optional<bool> control_converged =
            converge ? std::optional<bool>(converge_control_replicas(metadata)) : std::nullopt;
        {
            Lock lock(mutex_);
            if (ready_ && cached_root_ == metadata.catalogue_root) {
                // The root is content-addressed: unchanged, the cached snapshot
                // is current, whatever the generation did.
                cached_metadata_generation_ = generation;
                cache_until_ = Clock::now() + node_.config().metadata_cache;
                last_sync_unix_ms_ = unix_ms();
                if (control_converged && *control_converged) {
                    error_.clear();
                    error_code_.clear();
                } else if (control_converged) {
                    error_code_ = "converging";
                    error_ = "catalogue control replicas are converging";
                }
                return;
            }
        }
        auto snapshot = load_root(metadata.catalogue_root);
        cache(generation, metadata, std::move(snapshot));
    } catch (const std::exception& e) {
        Lock lock(mutex_);
        // A failed attempt keeps the loaded snapshot; warm reads continue on it
        // while a later pass retries.
        error_code_ = "unavailable";
        error_ = e.what();
        throw;
    }
}

std::shared_ptr<const CatalogueSnapshot> CatalogueManager::current_snapshot() {
    {
        Lock lock(mutex_);
        // Warm reads are memory-only: convergence is background work, and a GET
        // never blocks on replica I/O for an expired TTL.
        if (ready_ && cached_)
            return cached_;
    }

    // A cold manager's first read loads a snapshot synchronously.
    refresh(false);

    Lock lock(mutex_);
    if (!ready_ || !cached_)
        throw std::runtime_error("catalogue unavailable");
    return cached_;
}

bool CatalogueManager::refresh_needed() const {
    Lock lock(mutex_);
    if (!ready_ || !cached_)
        return true;
    return cached_metadata_generation_ < metadata_server_.known_generation() ||
           Clock::now() >= cache_until_;
}

CatalogueStatus CatalogueManager::status() const {
    CatalogueStatus status;
    std::shared_ptr<const CatalogueSnapshot> cached;
    {
        Lock lock(mutex_);
        status.enabled = true;
        status.metadata_generation = cached_metadata_generation_;
        status.known_metadata_generation = metadata_server_.known_generation();
        status.root = cached_root_;
        status.items = cached_ ? cached_->items.size() : 0;
        status.ready = ready_;
        status.last_sync_unix_ms = last_sync_unix_ms_;
        status.error_code = error_code_;
        status.error = error_;
        cached = cached_;
    }

    // Artwork walks and existence probes run outside the publication mutex,
    // which readers hold only to take the shared snapshot.
    std::set<ObjectId> art;
    if (cached)
        for (const auto& [_, item] : cached->items)
            for (const auto& artwork : item.artwork) art.insert(artwork.id);
    status.artwork_objects = art.size();
    for (const auto& id : art)
        status.local_artwork_objects += local_.data().has(id) ? 1 : 0;
    const bool root_local = !status.root || local_.control().has(*status.root);
    status.ready = status.ready && root_local;
    return status;
}

CatalogueSnapshot CatalogueManager::snapshot() {
    return *current_snapshot();
}

std::shared_ptr<const CatalogueSnapshot> CatalogueManager::snapshot_view() {
    return current_snapshot();
}

std::shared_ptr<const CatalogueSnapshot>
CatalogueManager::snapshot_view(const WorkContext& context) {
    bool warm = false;
    {
        Lock lock(mutex_);
        warm = ready_ && cached_;
    }
    if (!warm)
        (void)WaitGuard::enter(context, Waits::state_device | Waits::network,
                               "CatalogueManager::snapshot_view (cold)");
    return current_snapshot();
}

std::optional<CatalogueItem> CatalogueManager::get(std::string_view id) {
    auto snapshot = current_snapshot();
    auto it = snapshot->items.find(std::string(id));
    return it == snapshot->items.end() ? std::optional<CatalogueItem>{} : it->second;
}

std::optional<MediaProbeResult> CatalogueManager::media_profile(std::string_view media_id) {
    auto snapshot = current_snapshot();
    auto it = snapshot->media_profiles.find(std::string(media_id));
    if (it == snapshot->media_profiles.end() || !valid_catalogue_media_profile(media_id, it->second))
        return {};
    return it->second.probe;
}

ResolvedMediaProfile CatalogueManager::resolve_media_profile(
    std::string media_id, Clock::time_point deadline,
    std::function<MediaProbeResult()> generate) {
    try {
        if (auto persisted = media_profile(media_id)) return {*persisted, false, false};
    } catch (const std::exception& e) {
        Log::warn("immutable media profile lookup unavailable media=" + media_id +
                  " error=" + e.what() + "; retaining bounded generation fallback");
    }

    std::shared_ptr<MediaProfileFlight> flight;
    bool owner = false;
    {
        Lock lock(media_profile_mutex_);
        if (auto cached = resolved_media_profiles_.find(media_id);
            cached != resolved_media_profiles_.end())
            return {cached->second, false, false};
        auto [it, inserted] = media_profile_flights_.try_emplace(
            media_id, std::make_shared<MediaProfileFlight>());
        flight = it->second;
        owner = inserted;
    }
    if (!owner) {
        Lock lock(flight->mutex);
        if (!flight->cv.wait_until(lock.native(), deadline,
                                   [&]() MACHA_REQUIRES(flight->mutex) { return flight->complete; }))
            throw std::runtime_error("timed out waiting for concurrent immutable media profiling");
        if (flight->error) std::rethrow_exception(flight->error);
        return {*flight->result, false, true};
    }

    auto finish = [&](std::optional<MediaProbeResult> result, std::exception_ptr error = {}) {
        {
            Lock lock(flight->mutex);
            flight->result = result;
            flight->error = error;
            flight->complete = true;
        }
        if (result) {
            Lock lock(media_profile_mutex_);
            resolved_media_profiles_[media_id] = *result;
        }
        flight->cv.notify_all();
        Lock lock(media_profile_mutex_);
        auto it = media_profile_flights_.find(media_id);
        if (it != media_profile_flights_.end() && it->second == flight)
            media_profile_flights_.erase(it);
    };

    try {
        auto result = generate();
        CatalogueSnapshot::MediaProfile profile{catalogue_media_profile_schema, true, result};
        if (!valid_catalogue_media_profile(media_id, profile))
            throw std::runtime_error("generated immutable media profile is incomplete");
        finish(result);
        return {std::move(result), true, false};
    } catch (...) {
        finish({}, std::current_exception());
        throw;
    }
}

void CatalogueManager::put_media_profile(std::string media_id, MediaProbeResult probe) {
    CatalogueSnapshot::MediaProfile profile{catalogue_media_profile_schema, true, std::move(probe)};
    if (!valid_catalogue_media_profile(media_id, profile))
        throw std::invalid_argument("invalid immutable media profile");
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    if (auto it = current.media_profiles.find(media_id);
        it != current.media_profiles.end() && it->second == profile)
        return;
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_art = data_object_ids(current);
    current.media_profiles[std::move(media_id)] = std::move(profile);
    commit(expected_root, current, old_art);
}

void CatalogueManager::put_media_profiles(
    std::map<std::string, MediaProbeResult, std::less<>> profiles) {
    if (profiles.empty()) return;
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    bool changed = false;
    for (auto& [media_id, probe] : profiles) {
        CatalogueSnapshot::MediaProfile profile{catalogue_media_profile_schema, true, std::move(probe)};
        if (!valid_catalogue_media_profile(media_id, profile)) continue;
        auto it = current.media_profiles.find(media_id);
        if (it != current.media_profiles.end() && it->second == profile) continue;
        current.media_profiles[media_id] = std::move(profile);
        changed = true;
    }
    if (!changed) return;
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    commit(expected_root, current, data_object_ids(current));
}

std::optional<Bytes> CatalogueManager::media_index(std::string_view media_id) {
    auto snapshot = current_snapshot();
    auto it = snapshot->media_indexes.find(media_id);
    if (it == snapshot->media_indexes.end()) return {};
    return store_.get(it->second, 0, FrameType::foreground);
}

void CatalogueManager::put_media_index(std::string media_id, std::span<const uint8_t> bytes) {
    if (!media_id.starts_with("macha:") || bytes.empty())
        throw std::invalid_argument("invalid media index");
    const auto id = object_id(bytes);
    if (!store_.put(id, bytes, FrameType::speculative))
        throw CatalogueUnavailable("cannot store media index in distributed DATA storage");
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    if (auto it = current.media_indexes.find(media_id);
        it != current.media_indexes.end() && it->second == id)
        return;
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_data = data_object_ids(current);
    current.media_indexes[std::move(media_id)] = id;
    commit(expected_root, current, old_data);
}

size_t CatalogueManager::prune_media_profiles(
    const std::set<std::string>& live_media_ids) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    const auto before = current.media_profiles.size();
    std::erase_if(current.media_profiles, [&](const auto& item) {
        return !live_media_ids.contains(item.first);
    });
    const auto removed = before - current.media_profiles.size();
    const auto indexes_before = current.media_indexes.size();
    std::erase_if(current.media_indexes, [&](const auto& item) {
        return !live_media_ids.contains(item.first);
    });
    if (!removed && indexes_before == current.media_indexes.size()) return 0;
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    commit(expected_root, current, data_object_ids(current));
    {
        Lock lock(media_profile_mutex_);
        std::erase_if(resolved_media_profiles_, [&](const auto& item) {
            return !live_media_ids.contains(item.first);
        });
    }
    return removed;
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

std::vector<CatalogueItem> CatalogueManager::search(
    std::string_view query, size_t limit, const std::function<bool(const CatalogueItem&)>& keep) {
    auto snapshot = current_snapshot();
    std::vector<std::pair<double, CatalogueItem>> ranked;
    for (const auto& [_, item] : snapshot->items) {
        if (keep && !keep(item)) continue;
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

void CatalogueManager::commit(
    const std::optional<ObjectId>& expected_root, const CatalogueSnapshot& next,
    const std::set<ObjectId>& old_artwork, std::optional<Hash256> expected_namespace,
    std::optional<std::pair<std::string, MetadataConflict>> resolved_conflict) {
    // This node's own head: a catalogue write asks no peer what the head is.
    std::optional<MetadataSnapshotView> head;
    try {
        head = metadata_.local();
    } catch (const std::exception& e) {
        throw CatalogueUnavailable(std::string("catalogue metadata unavailable: ") + e.what());
    }
    const auto& metadata_snapshot = *head->snapshot;
    if (metadata_snapshot.catalogue_root != expected_root)
        throw CatalogueConflict("catalogue changed concurrently");
    if (expected_namespace &&
        metadata_namespace_signature(metadata_snapshot) != *expected_namespace)
        throw CatalogueConflict("namespace changed during catalogue reconciliation");

    const auto new_artwork = data_object_ids(next);

    // Artwork is ordinary immutable DATA. Only new references are validated;
    // unchanged ones were proven by the committed catalogue.
    for (const auto& id : new_artwork) {
        if (old_artwork.contains(id)) continue;
        // Held here is enough; only one that is not is fetched to prove it
        // exists somewhere.
        if (local_.data().has(id)) continue;
        if (!store_.get(id, 0, FrameType::speculative))
            throw CatalogueUnavailable("referenced artwork object is unavailable: " + to_string(id));
    }

    CatalogueManifest old_manifest;
    if (expected_root) {
        if (!store_.ensure_control_local(*expected_root))
            throw CatalogueUnavailable("current catalogue manifest unavailable");
        auto encoded = local_.control().get(*expected_root);
        if (!encoded)
            throw CatalogueUnavailable("current catalogue manifest unavailable locally");
        old_manifest = decode_catalogue_manifest(*encoded);
    }

    // Which shards this write touches: those holding a key at which `next`
    // differs from the catalogue it started from. The rest keep the object
    // the old manifest names and are neither built nor encoded. Without the
    // catalogue at the expected root to compare against, every shard is.
    std::array<bool, catalogue_shard_count> touched;
    touched.fill(true);
    {
        std::shared_ptr<const CatalogueSnapshot> started_from;
        {
            Lock lock(mutex_);
            if (ready_ && cached_ && expected_root && cached_root_ == expected_root)
                started_from = cached_;
        }
        if (started_from) {
            touched.fill(false);
            mark_changed_shards(started_from->items, next.items, touched);
            mark_changed_shards(started_from->media_profiles, next.media_profiles, touched);
            mark_changed_shards(started_from->media_indexes, next.media_indexes, touched);
        }
    }

    CatalogueManifest manifest;
    std::vector<std::pair<ObjectId, Bytes>> changed_control;
    changed_control.reserve(catalogue_shard_count + 1);
    for (size_t i = 0; i < catalogue_shard_count; ++i) {
        if (!touched[i]) {
            manifest.shards[i] = old_manifest.shards[i];
            continue;
        }
        const auto shard = catalogue_shard_of(next, i);
        if (shard.items.empty() && shard.media_profiles.empty() && shard.media_indexes.empty())
            continue;
        auto encoded = encode_catalogue(shard);
        const auto id = object_id(encoded);
        manifest.shards[i] = id;
        if (old_manifest.shards[i] != id)
            changed_control.push_back({id, std::move(encoded)});
    }

    auto encoded_manifest = encode_catalogue_manifest(manifest);
    const auto root = object_id(encoded_manifest);
    // An unchanged root is nothing to commit, unless a conflict is being
    // decided in its favour: that decision is the commit.
    if (expected_root && *expected_root == root && !resolved_conflict) {
        cache(head->generation, metadata_snapshot, next);
        return;
    }

    // A commit may reference a control object once this node holds it. The
    // other nodes are sent it with the commit's claims, and convergence
    // offers it to every active node, whatever its DATA capacity.
    for (const auto& [id, encoded] : changed_control) {
        if (!local_.control().put(id, encoded))
            throw CatalogueUnavailable("catalogue shard could not be stored");
    }
    if (!local_.control().put(root, encoded_manifest))
        throw CatalogueUnavailable("catalogue manifest could not be stored");

    try {
        if (resolved_conflict) {
            // mutate_delta: a tree-backed namespace has no entry map to diff,
            // so a commit declares its change set.
            metadata_.mutate_delta([&](MetadataSnapshot& metadata, MetadataDelta& delta) {
                if (metadata.catalogue_root != expected_root)
                    throw CatalogueConflict("catalogue changed concurrently");
                if (expected_namespace &&
                    metadata_namespace_signature(metadata) != *expected_namespace)
                    throw CatalogueConflict("namespace changed during catalogue reconciliation");
                auto found = metadata.conflicts.find(resolved_conflict->first);
                if (found == metadata.conflicts.end() || found->second != resolved_conflict->second)
                    throw CatalogueConflict("catalogue conflict changed concurrently");
                metadata.catalogue_root = root;
                metadata.conflicts.erase(found);
                delta.catalogue = CatalogueDelta::set;
                delta.catalogue_root = root;
                // The delta carries the conflict set; a replay infers nothing.
                delta.replace_conflicts = metadata.conflicts;
                for (const auto& id : old_artwork)
                    if (!new_artwork.contains(id))
                        record_garbage_upsert(delta, append_garbage(metadata, id));
            });
        } else {
            metadata_.mutate_delta([&](MetadataSnapshot& metadata, MetadataDelta& delta) {
                if (metadata.catalogue_root != expected_root)
                    throw CatalogueConflict("catalogue changed concurrently");
                if (expected_namespace &&
                    metadata_namespace_signature(metadata) != *expected_namespace)
                    throw CatalogueConflict("namespace changed during catalogue reconciliation");
                metadata.catalogue_root = root;
                delta.catalogue = CatalogueDelta::set;
                delta.catalogue_root = root;
                for (const auto& id : old_artwork) {
                    if (!new_artwork.contains(id))
                        record_garbage_upsert(delta, append_garbage(metadata, id));
                }
            });
        }
    } catch (const CatalogueConflict&) {
        throw;
    } catch (const std::exception& e) {
        throw CatalogueUnavailable(std::string("catalogue metadata durability unavailable: ") +
                                   e.what());
    }

    const auto committed = metadata_.local();
    {
        Lock lock(mutex_);
        control_converged_root_.reset();
        control_converged_nodes_.clear();
        control_convergence_retry_ = {};
    }
    cache(committed.generation, *committed.snapshot, next);
}

CatalogueItem CatalogueManager::upsert(CatalogueItem item,
                                        std::optional<uint64_t> expected_revision) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_art = data_object_ids(current);
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

std::vector<CatalogueItem> CatalogueManager::upsert_many(std::vector<CatalogueItem> items) {
    if (items.empty()) return {};
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_art = data_object_ids(current);
    const auto updated_ns = wall_time_ns();
    for (auto& item : items) {
        if (item.id.empty())
            throw std::runtime_error("catalogue item id is required");
        auto it = current.items.find(item.id);
        item.revision = it == current.items.end() ? 1 : it->second.revision + 1;
        item.updated_ns = updated_ns;
        current.items[item.id] = item;
    }
    commit(expected_root, current, old_art);
    return items;
}

bool CatalogueManager::erase(std::string_view id, std::optional<uint64_t> expected_revision) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    auto it = current.items.find(std::string(id));
    if (it == current.items.end())
        return false;
    if (expected_revision && it->second.revision != *expected_revision)
        throw CatalogueConflict("catalogue item revision changed");
    auto old_art = data_object_ids(current);
    current.items.erase(it);
    commit(expected_root, current, old_art);
    return true;
}

bool CatalogueManager::definitely_absent(std::string_view id) const {
    std::optional<ObjectId> cached_root;
    uint64_t cached_generation{};
    {
        Lock lock(mutex_);
        if (!ready_ || !cached_)
            return false;
        if (cached_->items.contains(std::string(id)))
            return false;
        cached_root = cached_root_;
        cached_generation = cached_metadata_generation_;
    }

    // An early-negative test only: a stale snapshot must never cause a false 404.
    const auto known_generation = metadata_server_.known_generation();
    if (cached_generation >= known_generation)
        return true;

    // If MetadataManager has decoded the known generation and the catalogue
    // root is unchanged, the cache is current despite an older generation: a
    // memory-only proof, sparing a replica repair for a definite 404.
    if (auto available = metadata_.current();
        available && available->generation >= known_generation &&
        available->snapshot->catalogue_root == cached_root)
        return true;

    return false;
}

CatalogueClearResult CatalogueManager::clear_metadata_with_media(
    std::string_view id, std::optional<uint64_t> expected_revision) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }

    auto root = current.items.find(std::string(id));
    if (root == current.items.end())
        return {};
    if (expected_revision && root->second.revision != *expected_revision)
        throw CatalogueConflict("catalogue item revision changed");

    // Removes the entity and its descendants, returning their media ids so the
    // caller can re-enrich only those files.
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

    auto old_art = data_object_ids(current);
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
    if (!store_.put_here(art.id, bytes, FrameType::speculative))
        throw std::runtime_error("cannot store artwork in distributed DATA storage");
    return art;
}

CatalogueArtwork CatalogueManager::stage_artwork_deferred(
    std::string role, std::string mime_type, std::span<const uint8_t> bytes,
    DistributedStore::DurabilityBatch& batch) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    CatalogueArtwork art{std::move(role), object_id(bytes), std::move(mime_type)};
    // Written on this node, durable at the batch's barrier; no peer is waited
    // for.
    if (!store_.put_deferred_here(art.id, bytes, batch, FrameType::speculative))
        throw std::runtime_error("cannot stage artwork in distributed DATA storage");
    return art;
}

bool CatalogueManager::artwork_durability_barrier(DistributedStore::DurabilityBatch& batch) {
    return store_.durability_barrier(batch, FrameType::speculative);
}

void CatalogueManager::reconcile_scanner(const std::vector<CatalogueItem>& discovered,
                                         const std::set<std::string>& active_media_ids,
                                         bool prune_missing,
                                         std::optional<Hash256> expected_namespace,
                                         const std::map<std::string, MediaProbeResult, std::less<>>& profiles,
                                         const std::set<std::string>& vanished_media) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    refresh(false);
    auto current = *current_snapshot();
    std::optional<ObjectId> expected_root;
    {
        Lock lock(mutex_);
        expected_root = cached_root_;
    }
    auto old_art = data_object_ids(current);
    bool changed = false;

    for (const auto& [media_id, probe] : profiles) {
        CatalogueSnapshot::MediaProfile profile{catalogue_media_profile_schema, true, probe};
        if (!valid_catalogue_media_profile(media_id, profile)) continue;
        auto it = current.media_profiles.find(media_id);
        if (it != current.media_profiles.end() && it->second == profile) continue;
        current.media_profiles[media_id] = std::move(profile);
        changed = true;
    }

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
                // A user-edited item keeps its descriptive metadata; its media
                // bindings are still reconciled.
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
            // Scanner artwork is a candidate set, not one slot per role: known
            // objects are kept unless resupplied, so an embedded cover and a
            // provider cover coexist.
            for (const auto& art : it->second.artwork) {
                const bool already_present = std::any_of(
                    item.artwork.begin(), item.artwork.end(), [&](const auto& candidate) {
                        return candidate.role == art.role && candidate.id == art.id;
                    });
                if (!already_present) item.artwork.push_back(art);
            }
            // A match may be another file of a known item: keep existing
            // bindings; vanished ones are removed below.
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
        // Only scanner-owned leaf bindings are reconciled; manual entries are
        // never removed.
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

        // A manual item may bind a file outside the catalogue roots, so it
        // loses only files gone from the namespace altogether.
        if (!vanished_media.empty()) {
            for (auto& [_, item] : current.items) {
                auto marker = item.external_ids.find("macha_scanner");
                if (marker != item.external_ids.end() && marker->second == "1") continue;
                const auto before = item.media_ids.size();
                std::erase_if(item.media_ids, [&](const std::string& media) {
                    return vanished_media.contains(media);
                });
                if (item.media_ids.size() != before) {
                    ++item.revision;
                    item.updated_ns = wall_time_ns();
                    changed = true;
                }
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

        // Bottom-up removal of empty scanner-created hierarchy nodes.
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
        // Content-addressed: a concurrent commit may make this hash live, so
        // failed staging is left as an orphan for reachability GC.
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
    // Reading DATA never makes the reader an owner: artwork is read from its
    // owner like any remote extent, cached without using replica quota.
    auto bytes = store_.get(id, 0, FrameType::foreground);
    if (!bytes)
        return {};
    return CatalogueArtworkContent{std::move(*mime_type), std::move(*bytes)};
}


CatalogueRetentionObjects CatalogueManager::retention_objects(
    const std::optional<ObjectId>& old_root, const std::optional<ObjectId>& new_root) {
    CatalogueRetentionObjects out;
    if (!new_root)
        return out;

    if (!store_.ensure_control_local(*new_root))
        throw CatalogueUnavailable("catalogue manifest unavailable for retention publication");
    auto encoded_manifest = local_.control().get(*new_root);
    if (!encoded_manifest)
        throw CatalogueUnavailable("catalogue manifest unavailable locally for retention publication");
    const auto manifest = decode_catalogue_manifest(*encoded_manifest);
    // Only a shard the two roots do not share can hold anything new: the
    // rest are the same object, claimed when it was written. An old root
    // that cannot be read is taken as empty, which claims more, never less.
    CatalogueManifest old_manifest;
    if (old_root && store_.ensure_control_local(*old_root))
        if (const auto encoded = local_.control().get(*old_root))
            old_manifest = decode_catalogue_manifest(*encoded);
    out.control.push_back(*new_root);
    for (size_t i = 0; i < manifest.shards.size(); ++i) {
        const auto& shard = manifest.shards[i];
        if (!shard || shard == old_manifest.shards[i])
            continue;
        if (!store_.ensure_control_local(*shard))
            throw CatalogueUnavailable("catalogue shard unavailable for retention publication: " +
                                       to_string(*shard));
        out.control.push_back(*shard);
    }

    const auto shard_of = [&](const std::optional<ObjectId>& id, bool required) {
        CatalogueSnapshot shard;
        if (!id)
            return shard;
        std::optional<Bytes> encoded;
        if (store_.ensure_control_local(*id))
            encoded = local_.control().get(*id);
        if (!encoded) {
            if (required)
                throw CatalogueUnavailable("catalogue shard unavailable locally: " +
                                           to_string(*id));
            return shard;
        }
        return decode_catalogue(*encoded);
    };
    for (size_t i = 0; i < manifest.shards.size(); ++i) {
        if (manifest.shards[i] == old_manifest.shards[i])
            continue;
        const auto before = shard_of(old_manifest.shards[i], false);
        const auto after = shard_of(manifest.shards[i], true);
        for (const auto& [id, item] : after.items) {
            const auto found = before.items.find(id);
            if (found != before.items.end() && found->second == item)
                continue;
            for (const auto& artwork : item.artwork)
                out.data.push_back(artwork.id);
        }
        for (const auto& [media_id, index] : after.media_indexes) {
            const auto found = before.media_indexes.find(media_id);
            if (found == before.media_indexes.end() || found->second != index)
                out.data.push_back(index);
        }
    }
    std::sort(out.data.begin(), out.data.end());
    out.data.erase(std::unique(out.data.begin(), out.data.end()), out.data.end());
    std::sort(out.control.begin(), out.control.end());
    out.control.erase(std::unique(out.control.begin(), out.control.end()), out.control.end());
    return out;
}

CatalogueMaintenanceHead CatalogueManager::maintenance_head() {
    // Liveness comes from converged metadata, not the stale-tolerant API cache,
    // or obsolete objects could stay live after a remote mutation.
    CatalogueMaintenanceHead head;
    try {
        auto view = metadata_.current();
        if (!view || view->generation < metadata_server_.known_generation()) {
            (void)metadata_.record();
            view = metadata_.current();
        }
        if (view) {
            head.generation = view->generation;
            head.current = view->generation >= metadata_server_.known_generation();
            head.root = view->snapshot->catalogue_root;
            head.roots = metadata_catalogue_root_set(*view->snapshot);
        }
    } catch (...) {
        head.current = false;
    }
    return head;
}

bool CatalogueManager::maintenance_repair() {
    try {
        repair_once();
        return true;
    } catch (...) {
        return false;
    }
}

CatalogueMaintenance CatalogueManager::maintenance_objects(const CatalogueMaintenanceHead& head,
                                                           bool repaired) {
    CatalogueMaintenance out;
    const auto& metadata_root = head.root;
    const auto& metadata_roots = head.roots;
    const uint64_t metadata_generation = head.generation;
    const bool metadata_current = head.current;
    // A root conflict's alternatives stay durable until resolved: protect
    // their roots, manifests, shards and artwork from GC.
    out.control_live.insert(metadata_roots.begin(), metadata_roots.end());
    bool repair_ok = repaired;
    std::optional<ObjectId> root;
    std::shared_ptr<const CatalogueSnapshot> cached;
    {
        Lock lock(mutex_);
        root = cached_root_;
        cached = cached_;
    }

    if (root) {
        out.control_live.insert(*root);
        try {
            if (store_.ensure_control_local(*root)) {
                if (auto encoded = local_.control().get(*root)) {
                    const auto manifest = decode_catalogue_manifest(*encoded);
                    for (const auto& shard : manifest.shards)
                        if (shard) out.control_live.insert(*shard);
                }
            }
        } catch (...) {
            repair_ok = false;
        }
    }
    bool protected_roots_complete = true;
    for (const auto& protected_root : metadata_roots) {
        try {
            if (!store_.ensure_control_local(protected_root)) {
                protected_roots_complete = false;
                continue;
            }
            auto encoded_manifest = local_.control().get(protected_root);
            if (!encoded_manifest) {
                protected_roots_complete = false;
                continue;
            }
            const auto manifest = decode_catalogue_manifest(*encoded_manifest);
            for (const auto& shard_id : manifest.shards) {
                if (!shard_id)
                    continue;
                out.control_live.insert(*shard_id);
                if (!store_.ensure_control_local(*shard_id)) {
                    protected_roots_complete = false;
                    continue;
                }
                auto encoded_shard = local_.control().get(*shard_id);
                if (!encoded_shard) {
                    protected_roots_complete = false;
                    continue;
                }
                const auto shard = decode_catalogue(*encoded_shard);
                for (const auto& id : data_object_ids(shard))
                    out.live.insert(id);
            }
        } catch (...) {
            protected_roots_complete = false;
        }
    }

    {
        Lock lock(mutex_);
        const bool root_converged = cached_root_ == metadata_root &&
                                    cached_metadata_generation_ >= metadata_generation;
        out.complete = metadata_current && repair_ok && root_converged &&
                       protected_roots_complete;
    }
    if (cached) {
        for (const auto& id : data_object_ids(*cached))
            out.live.insert(id);
    }
    return out;
}

size_t CatalogueManager::control_gc_step(std::span<const ObjectId> live,
                                         std::chrono::milliseconds grace,
                                         size_t operation_budget,
                                         UnreferencedSince* sightings, uint64_t now_unix_ms) {
    if (!operation_budget) return 0;

    // Publication is data-before-metadata, so a future root's CONTROL objects
    // are briefly unreferenced. Time is no fence: the root may advance between
    // staging and a pass. An unreferenced object survives the root epoch in
    // which GC first sees (or re-affirms) it; only a later epoch orphans it.
    Clock::time_point root_epoch;
    uint64_t root_epoch_sequence = 0;
    {
        Lock lock(mutex_);
        if (!control_gc_root_epoch_initialized_)
            return 0;
        root_epoch = control_gc_root_epoch_;
        root_epoch_sequence = control_gc_root_epoch_sequence_;
    }
    const auto since_root = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - root_epoch);
    const auto staged_since_root = since_root + std::chrono::milliseconds(1);

    size_t removed = 0;
    bool exhausted = false;
    for (size_t operations = 0; operations < operation_budget && !exhausted; ++operations) {
        auto id = local_.control().next_object(control_gc_cursor_, exhausted);
        if (!id) continue;

        if (std::binary_search(live.begin(), live.end(), *id)) {
            if (sightings)
                sightings->forget(*id);
            Lock lock(mutex_);
            control_gc_unreferenced_epoch_.erase(*id);
            continue;
        }
        if (sightings && ledger_.retained(RetentionClass::control, *id)) {
            sightings->forget(*id);
            continue;
        }
        const bool waited = !sightings || sightings->matured(*id, now_unix_ms, grace);

        // A content-addressed object may be reused by the current publication;
        // put() touches it, so keep any object touched since this root was seen.
        if (!local_.control().older_than(*id, staged_since_root)) {
            Lock lock(mutex_);
            if (control_gc_root_epoch_sequence_ != root_epoch_sequence)
                continue;
            control_gc_unreferenced_epoch_[*id] = root_epoch_sequence;
            continue;
        }

        {
            Lock lock(mutex_);
            if (control_gc_root_epoch_sequence_ != root_epoch_sequence)
                continue;
            auto [seen, inserted] =
                control_gc_unreferenced_epoch_.emplace(*id, root_epoch_sequence);
            if (inserted || seen->second == root_epoch_sequence)
                continue;

            // cache() takes this mutex to publish a new root, so GC never races
            // a root transition and deletes its staging.
            if (!waited || ledger_.retained(RetentionClass::control, *id))
                continue;
            if (local_.control().remove_if_older_than(*id, grace)) {
                control_gc_unreferenced_epoch_.erase(seen);
                if (sightings)
                    sightings->forget(*id);
                ++removed;
            }
        }
    }
    if (exhausted && sightings)
        sightings->pass_complete();
    if (exhausted && removed)
        (void)local_.control().compact_packs();
    return removed;
}

} // namespace macha
