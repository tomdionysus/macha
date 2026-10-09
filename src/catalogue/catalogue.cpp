// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/catalogue.hpp"
#include "diagnostics.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "resident_bytes.hpp"

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

uint64_t ms_since(Clock::time_point started) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

[[noreturn]] void revision_changed(std::string_view id, uint64_t expected, uint64_t found) {
    throw CatalogueConflict("catalogue item revision changed: " + std::string(id) + " expected " +
                            std::to_string(expected) + ", now " + std::to_string(found));
}

struct CatalogueManifest {
    std::array<std::optional<ObjectId>, catalogue_shard_count> shards;
};


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
// Walks one of a view's maps in key order (a merge of its shards, each of
// which is sorted) against the same map of `after`, marking each slot where
// they differ. A key only `after` holds is hashed to find its slot and kept
// in `added`; the view's own keys carry their slot.
template <class Map>
void diff_slots(const std::array<const Map*, catalogue_shard_count>& before, const Map& after,
                std::array<bool, catalogue_shard_count>& touched,
                std::array<std::vector<typename Map::const_iterator>, catalogue_shard_count>& added) {
    using Iterator = typename Map::const_iterator;
    struct Cursor {
        Iterator at, end;
        size_t slot;
    };
    const auto later = [](const Cursor& x, const Cursor& y) { return y.at->first < x.at->first; };
    std::vector<Cursor> heap;
    heap.reserve(catalogue_shard_count);
    for (size_t slot = 0; slot < catalogue_shard_count; ++slot)
        if (before[slot] && !before[slot]->empty())
            heap.push_back({before[slot]->begin(), before[slot]->end(), slot});
    std::make_heap(heap.begin(), heap.end(), later);
    const auto advance = [&] {
        std::pop_heap(heap.begin(), heap.end(), later);
        if (++heap.back().at == heap.back().end)
            heap.pop_back();
        else
            std::push_heap(heap.begin(), heap.end(), later);
    };
    auto b = after.begin();
    while (!heap.empty() || b != after.end()) {
        if (!heap.empty() && (b == after.end() || heap.front().at->first < b->first)) {
            touched[heap.front().slot] = true;
            advance();
        } else if (heap.empty() || b->first < heap.front().at->first) {
            const auto slot = catalogue_shard(b->first);
            touched[slot] = true;
            added[slot].push_back(b);
            ++b;
        } else {
            if (!(heap.front().at->second == b->second))
                touched[heap.front().slot] = true;
            advance();
            ++b;
        }
    }
}

// One slot of `after`: the keys `before` held there that `after` keeps,
// with `after`'s values, and the keys new to it.
template <class Map>
Map rebuild_slot(const Map* before, const Map& after,
                 const std::vector<typename Map::const_iterator>& added) {
    Map out;
    if (before)
        for (const auto& [key, _] : *before)
            if (const auto found = after.find(key); found != after.end())
                out.emplace_hint(out.end(), key, found->second);
    for (const auto& entry : added)
        out.emplace(entry->first, entry->second);
    return out;
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

size_t catalogue_shard(std::string_view id) {
    const auto hash = sha256({reinterpret_cast<const uint8_t*>(id.data()), id.size()});
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i)
        value = (value << 8) | hash.bytes[i];
    return value % catalogue_shard_count;
}

CatalogueView::CatalogueView(Shards shards, ShardIds ids)
    : shards_(std::move(shards)), ids_(std::move(ids)) {
    for (const auto& shard : shards_)
        if (shard)
            item_count_ += shard->items.size();
}

CatalogueView CatalogueView::of(const CatalogueSnapshot& snapshot) {
    std::array<CatalogueSnapshot, catalogue_shard_count> split;
    for (const auto& [id, item] : snapshot.items)
        split[catalogue_shard(id)].items.emplace(id, item);
    for (const auto& [id, profile] : snapshot.media_profiles)
        split[catalogue_shard(id)].media_profiles.emplace(id, profile);
    for (const auto& [id, index] : snapshot.media_indexes)
        split[catalogue_shard(id)].media_indexes.emplace(id, index);
    Shards shards;
    for (size_t i = 0; i < catalogue_shard_count; ++i) {
        auto& shard = split[i];
        if (!shard.items.empty() || !shard.media_profiles.empty() || !shard.media_indexes.empty())
            shards[i] = std::make_shared<const CatalogueSnapshot>(std::move(shard));
    }
    return CatalogueView(std::move(shards), {});
}

const CatalogueItem* CatalogueView::item(std::string_view id) const {
    const auto& shard = shards_[catalogue_shard(id)];
    if (!shard)
        return nullptr;
    const auto found = shard->items.find(std::string(id));
    return found == shard->items.end() ? nullptr : &found->second;
}

const CatalogueSnapshot::MediaProfile* CatalogueView::media_profile(std::string_view media_id) const {
    const auto& shard = shards_[catalogue_shard(media_id)];
    if (!shard)
        return nullptr;
    const auto found = shard->media_profiles.find(media_id);
    return found == shard->media_profiles.end() ? nullptr : &found->second;
}

const ObjectId* CatalogueView::media_index(std::string_view media_id) const {
    const auto& shard = shards_[catalogue_shard(media_id)];
    if (!shard)
        return nullptr;
    const auto found = shard->media_indexes.find(media_id);
    return found == shard->media_indexes.end() ? nullptr : &found->second;
}

CatalogueSnapshot CatalogueView::merged() const {
    CatalogueSnapshot out;
    for (const auto& shard : shards_) {
        if (!shard)
            continue;
        out.items.insert(shard->items.begin(), shard->items.end());
        out.media_profiles.insert(shard->media_profiles.begin(), shard->media_profiles.end());
        out.media_indexes.insert(shard->media_indexes.begin(), shard->media_indexes.end());
    }
    return out;
}

class CatalogueDraft {
  public:
    explicit CatalogueDraft(std::shared_ptr<const CatalogueView> base) : base_(std::move(base)) {}
    CatalogueDraft(CatalogueDraft&&) = default;
    CatalogueDraft& operator=(CatalogueDraft&&) = default;

    const std::shared_ptr<const CatalogueView>& base() const noexcept { return base_; }

    const CatalogueItem* item(std::string_view id) const {
        const auto* shard = read(catalogue_shard(id));
        if (!shard)
            return nullptr;
        const auto found = shard->items.find(std::string(id));
        return found == shard->items.end() ? nullptr : &found->second;
    }
    // The item, in this draft's own copy of its shard; none if absent.
    CatalogueItem* edit_item(std::string_view id) {
        if (!item(id))
            return nullptr;
        return &write(catalogue_shard(id)).items.at(std::string(id));
    }
    void put(CatalogueItem item) {
        auto& shard = write(catalogue_shard(item.id));
        auto key = item.id;
        shard.items.insert_or_assign(std::move(key), std::move(item));
    }
    bool erase(std::string_view id) {
        if (!item(id))
            return false;
        write(catalogue_shard(id)).items.erase(std::string(id));
        return true;
    }

    const CatalogueSnapshot::MediaProfile* media_profile(std::string_view media_id) const {
        const auto* shard = read(catalogue_shard(media_id));
        if (!shard)
            return nullptr;
        const auto found = shard->media_profiles.find(media_id);
        return found == shard->media_profiles.end() ? nullptr : &found->second;
    }
    void put_media_profile(std::string media_id, CatalogueSnapshot::MediaProfile profile) {
        auto& shard = write(catalogue_shard(media_id));
        shard.media_profiles.insert_or_assign(std::move(media_id), std::move(profile));
    }
    bool erase_media_profile(std::string_view media_id) {
        if (!media_profile(media_id))
            return false;
        auto& profiles = write(catalogue_shard(media_id)).media_profiles;
        profiles.erase(profiles.find(media_id));
        return true;
    }

    const ObjectId* media_index(std::string_view media_id) const {
        const auto* shard = read(catalogue_shard(media_id));
        if (!shard)
            return nullptr;
        const auto found = shard->media_indexes.find(media_id);
        return found == shard->media_indexes.end() ? nullptr : &found->second;
    }
    void put_media_index(std::string media_id, ObjectId index) {
        auto& shard = write(catalogue_shard(media_id));
        shard.media_indexes.insert_or_assign(std::move(media_id), index);
    }
    bool erase_media_index(std::string_view media_id) {
        if (!media_index(media_id))
            return false;
        auto& indexes = write(catalogue_shard(media_id)).media_indexes;
        indexes.erase(indexes.find(media_id));
        return true;
    }

    // Every entry as the draft now has it, shard by shard. The callback may
    // not change the draft; collect, then change.
    template <class F> void for_each_item(F&& f) const {
        for (size_t slot = 0; slot < catalogue_shard_count; ++slot)
            if (const auto* shard = read(slot))
                for (const auto& [id, item] : shard->items)
                    f(id, item);
    }
    template <class F> void for_each_media_profile(F&& f) const {
        for (size_t slot = 0; slot < catalogue_shard_count; ++slot)
            if (const auto* shard = read(slot))
                for (const auto& [id, profile] : shard->media_profiles)
                    f(id, profile);
    }
    template <class F> void for_each_media_index(F&& f) const {
        for (size_t slot = 0; slot < catalogue_shard_count; ++slot)
            if (const auto* shard = read(slot))
                for (const auto& [id, index] : shard->media_indexes)
                    f(id, index);
    }

    bool written(size_t slot) const noexcept { return written_[slot].has_value(); }
    // The written shard, none if the write left it empty.
    std::shared_ptr<const CatalogueSnapshot> take(size_t slot) {
        auto& shard = *written_[slot];
        if (shard.items.empty() && shard.media_profiles.empty() && shard.media_indexes.empty())
            return nullptr;
        return std::make_shared<const CatalogueSnapshot>(std::move(shard));
    }

  private:
    const CatalogueSnapshot* read(size_t slot) const {
        if (written_[slot])
            return &*written_[slot];
        return base_ ? base_->shards()[slot].get() : nullptr;
    }
    CatalogueSnapshot& write(size_t slot) {
        if (!written_[slot]) {
            const auto* base = base_ ? base_->shards()[slot].get() : nullptr;
            written_[slot] = base ? *base : CatalogueSnapshot{};
        }
        return *written_[slot];
    }

    std::shared_ptr<const CatalogueView> base_;
    std::array<std::optional<CatalogueSnapshot>, catalogue_shard_count> written_;
};

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

uint64_t catalogue_resident_bytes(const CatalogueSnapshot& snapshot) {
    uint64_t total = sizeof(CatalogueSnapshot);
    resident::map_nodes(total, snapshot.items);
    for (const auto& [key, item] : snapshot.items) {
        resident::string(total, key);
        resident::string(total, item.id);
        resident::string(total, item.title);
        resident::string(total, item.sort_title);
        resident::string(total, item.synopsis);
        if (item.parent_id)
            resident::string(total, *item.parent_id);
        resident::vector(total, item.aliases);
        for (const auto& alias : item.aliases)
            resident::string(total, alias);
        resident::map_nodes(total, item.external_ids);
        for (const auto& [provider, id] : item.external_ids) {
            resident::string(total, provider);
            resident::string(total, id);
        }
        resident::vector(total, item.media_ids);
        for (const auto& media_id : item.media_ids)
            resident::string(total, media_id);
        resident::vector(total, item.artwork);
        for (const auto& art : item.artwork) {
            resident::string(total, art.role);
            resident::string(total, art.mime_type);
        }
    }
    resident::map_nodes(total, snapshot.media_profiles);
    for (const auto& [media_id, profile] : snapshot.media_profiles) {
        resident::string(total, media_id);
        resident::string(total, profile.probe.format);
        resident::vector(total, profile.probe.streams);
        for (const auto& stream : profile.probe.streams) {
            resident::string(total, stream.codec);
            resident::string(total, stream.profile);
            resident::string(total, stream.language);
            resident::string(total, stream.color_transfer);
        }
    }
    resident::map_nodes(total, snapshot.media_indexes);
    for (const auto& [media_id, _] : snapshot.media_indexes)
        resident::string(total, media_id);
    return total;
}

uint64_t catalogue_resident_bytes(const CatalogueView& view) {
    uint64_t total = sizeof(CatalogueView);
    for (const auto& shard : view.shards())
        if (shard)
            resident::add(total, catalogue_resident_bytes(*shard));
    return total;
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

std::vector<CatalogueArtwork> effective_catalogue_artwork(const CatalogueView& catalogue,
                                                           const CatalogueItem& item) {
    if (!item.artwork.empty())
        return item.artwork;

    if (item.kind == CatalogueKind::track) {
        if (!item.parent_id)
            return {};
        const auto* parent = catalogue.item(*item.parent_id);
        if (!parent || parent->kind != CatalogueKind::album)
            return {};
        return parent->artwork;
    }

    if (item.kind != CatalogueKind::artist)
        return {};

    const CatalogueItem* newest = nullptr;
    for (const auto& [_, candidate] : catalogue.items()) {
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

std::set<ObjectId> CatalogueManager::data_object_ids(const CatalogueView& view) {
    std::set<ObjectId> ids;
    for (const auto& shard : view.shards())
        if (shard) {
            const auto of_shard = data_object_ids(*shard);
            ids.insert(of_shard.begin(), of_shard.end());
        }
    return ids;
}

std::shared_ptr<const CatalogueView>
CatalogueManager::load_view(const std::optional<ObjectId>& root, const CatalogueView* previous,
                            size_t* decoded) {
    if (!root)
        return std::make_shared<const CatalogueView>();
    if (!store_.ensure_control_local(*root))
        throw CatalogueUnavailable("catalogue manifest unavailable");
    auto encoded_manifest = local_.control().get(*root);
    if (!encoded_manifest)
        throw CatalogueUnavailable("catalogue manifest unavailable locally");
    const auto manifest = decode_catalogue_manifest(*encoded_manifest);
    CatalogueView::Shards shards;
    size_t read_shards = 0;
    uint64_t bytes = encoded_manifest->size();
    uint64_t read_ms = 0;
    const auto started = Clock::now();
    for (size_t i = 0; i < catalogue_shard_count; ++i) {
        const auto& shard_id = manifest.shards[i];
        if (!shard_id)
            continue;
        // A shard the previous view decoded under the same id is that shard.
        if (previous && previous->shard_ids()[i] == shard_id) {
            shards[i] = previous->shards()[i];
            continue;
        }
        const auto read_started = Clock::now();
        if (!store_.ensure_control_local(*shard_id))
            throw CatalogueUnavailable("catalogue shard unavailable: " + to_string(*shard_id));
        auto encoded_shard = local_.control().get(*shard_id);
        if (!encoded_shard)
            throw CatalogueUnavailable("catalogue shard unavailable locally: " + to_string(*shard_id));
        read_ms += ms_since(read_started);
        ++read_shards;
        bytes += encoded_shard->size();
        auto shard = decode_catalogue(*encoded_shard);
        // Every key must hash to the slot that holds it, or lookups miss it.
        const auto misplaced = [&](const auto& map) {
            return std::any_of(map.begin(), map.end(),
                               [&](const auto& entry) { return catalogue_shard(entry.first) != i; });
        };
        if (misplaced(shard.items) || misplaced(shard.media_profiles) ||
            misplaced(shard.media_indexes))
            throw std::runtime_error("catalogue entry in the wrong shard");
        shards[i] = std::make_shared<const CatalogueSnapshot>(std::move(shard));
    }
    auto view = std::make_shared<const CatalogueView>(std::move(shards), manifest.shards);
    if (decoded)
        *decoded = read_shards;
    const auto total_ms = ms_since(started);
    Log::debug("catalogue loaded root=" + to_string(*root).substr(0, 12) +
               " shards=" + std::to_string(read_shards) + " bytes=" + std::to_string(bytes) +
               " items=" + std::to_string(view->item_count()) +
               " read_ms=" + std::to_string(read_ms) +
               " decode_ms=" + std::to_string(total_ms - std::min(total_ms, read_ms)));
    return view;
}

CatalogueSnapshot CatalogueManager::load_root(const std::optional<ObjectId>& root) {
    return load_view(root, nullptr)->merged();
}

bool CatalogueManager::converge_control_replicas(const std::optional<ObjectId>& root) {
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

        std::vector<ObjectId> objects{*root};
        objects.reserve(catalogue_shard_count + 1);
        for (const auto& shard_id : manifest.shards) {
            if (!shard_id) continue;
            if (!store_.ensure_control_local(*shard_id))
                throw CatalogueUnavailable("catalogue shard unavailable for control repair: " +
                                           to_string(*shard_id));
            objects.push_back(*shard_id);
        }

        // Each node present is asked what it lacks and sent only that.
        const auto offer_started = Clock::now();
        const auto offer = store_.offer_control(objects);
        const bool complete = offer.held_by >= active_nodes.size();
        Log::debug("catalogue control convergence root=" + to_string(*root).substr(0, 12) +
                   " objects=" + std::to_string(objects.size()) +
                   " sent=" + std::to_string(offer.objects_sent) +
                   " bytes=" + std::to_string(offer.bytes_sent) +
                   " nodes=" + std::to_string(active_nodes.size()) +
                   " complete=" + (complete ? "yes" : "no") +
                   " ms=" + std::to_string(ms_since(offer_started)));

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
void CatalogueManager::install_head(std::shared_ptr<const CatalogueView> known,
                                    const std::optional<ObjectId>& known_root) {
    Lock install_lock(install_mutex_);
    const auto started = Clock::now();
    try {
        const auto head = metadata_.local();
        if (!head.snapshot)
            throw CatalogueUnavailable("catalogue metadata snapshot unavailable");
        const auto& root = head.snapshot->catalogue_root;
        {
            // The root is content-addressed: unchanged, the view is current,
            // whatever the generation did.
            Lock lock(mutex_);
            if (ready_ && cached_root_ == root) {
                cached_metadata_generation_ = std::max(cached_metadata_generation_, head.generation);
                last_sync_unix_ms_ = unix_ms();
                return;
            }
        }
        std::shared_ptr<const CatalogueView> installed;
        std::shared_ptr<const CatalogueView> previous;
        if (known && known_root == root) {
            installed = std::move(known);
        } else {
            Lock lock(mutex_);
            if (staged_ && staged_root_ == root)
                installed = staged_;
            previous = cached_;
        }
        const bool loaded = !installed;
        size_t decoded = 0;
        if (loaded)
            installed = load_view(root, previous.get(), &decoded);
        Lock lock(mutex_);
        if (loaded)
            ++loads_;
        const bool root_changed = !control_gc_root_epoch_initialized_ || cached_root_ != root;
        cached_ = std::move(installed);
        cached_root_ = root;
        cached_metadata_generation_ = head.generation;
        last_sync_unix_ms_ = unix_ms();
        ++installs_;
        if (root_changed) {
            control_gc_root_epoch_ = Clock::now();
            ++control_gc_root_epoch_sequence_;
            control_gc_root_epoch_initialized_ = true;
        }
        ready_ = true;
        error_.clear();
        error_code_.clear();
        if (Log::enabled(LogLevel::debug)) {
            const auto view = cached_;
            const auto installs = installs_;
            const auto loads = loads_;
            lock.unlock();
            const auto install_ms = ms_since(started);
            const auto count_started = Clock::now();
            const auto resident = catalogue_resident_bytes(*view);
            Log::debug("catalogue installed root=" +
                       (root ? to_string(*root).substr(0, 12) : std::string("none")) +
                       " generation=" + std::to_string(head.generation) +
                       " source=" + (loaded ? "loaded" : "commit") +
                       " items=" + std::to_string(view->item_count()) +
                       " decoded_shards=" + std::to_string(decoded) +
                       " resident_bytes=" + std::to_string(resident) +
                       " install_ms=" + std::to_string(install_ms) +
                       " count_ms=" + std::to_string(ms_since(count_started)) +
                       " installs=" + std::to_string(installs) +
                       " loads=" + std::to_string(loads));
        }
    } catch (const std::exception& e) {
        // The previous view keeps serving; the next head change retries.
        Lock lock(mutex_);
        error_code_ = "unavailable";
        error_ = e.what();
        throw;
    }
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

void CatalogueManager::follow_head() {
    install_head();
}

void CatalogueManager::repair_once() {
    // At most one root conflict per pass. Disjoint item merges are automatic;
    // same-item or parent/child collisions stay durable conflicts. The merge
    // is a commit, which installs, so it is serialised with every other.
    const auto started = Clock::now();
    bool reconciled = false;
    {
        TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
        reconciled = reconcile_catalogue_conflict(metadata_.local());
    }
    const auto reconcile_ms = ms_since(started);
    if (!reconciled)
        install_head();
    const auto install_ms = ms_since(started) - reconcile_ms;
    std::optional<ObjectId> root;
    {
        Lock lock(mutex_);
        root = cached_root_;
    }
    const bool converged = converge_control_replicas(root);
    const auto total_ms = ms_since(started);
    if (total_ms >= 1000)
        Log::debug("catalogue repair reconcile_ms=" + std::to_string(reconcile_ms) +
                   " install_ms=" + std::to_string(install_ms) +
                   " converge_ms=" + std::to_string(total_ms - reconcile_ms - install_ms) +
                   " reconciled=" + (reconciled ? "yes" : "no"));
    Lock lock(mutex_);
    if (converged) {
        error_.clear();
        error_code_.clear();
    } else {
        error_code_ = "converging";
        error_ = "catalogue control replicas are converging";
    }
}

bool CatalogueManager::convergence_needed() const {
    std::vector<NodeId> active_nodes;
    for (const auto& node : node_.membership().active())
        active_nodes.push_back(node.id);
    std::sort(active_nodes.begin(), active_nodes.end());
    Lock lock(mutex_);
    if (!ready_)
        return true;
    if (control_converged_root_ == cached_root_ && control_converged_nodes_ == active_nodes)
        return false;
    return Clock::now() >= control_convergence_retry_;
}

uint64_t CatalogueManager::installs() const {
    Lock lock(mutex_);
    return installs_;
}

uint64_t CatalogueManager::loads() const {
    Lock lock(mutex_);
    return loads_;
}

std::shared_ptr<const CatalogueView> CatalogueManager::current_snapshot() {
    {
        Lock lock(mutex_);
        // Warm reads are memory-only: the view follows the head behind them.
        if (ready_ && cached_)
            return cached_;
    }

    // A cold manager's first read installs synchronously.
    install_head();

    Lock lock(mutex_);
    if (!ready_ || !cached_)
        throw std::runtime_error("catalogue unavailable");
    return cached_;
}

CatalogueStatus CatalogueManager::status() const {
    CatalogueStatus status;
    std::shared_ptr<const CatalogueView> cached;
    {
        Lock lock(mutex_);
        status.enabled = true;
        status.metadata_generation = cached_metadata_generation_;
        status.known_metadata_generation = metadata_server_.known_generation();
        status.root = cached_root_;
        status.items = cached_ ? cached_->item_count() : 0;
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
        for (const auto& [_, item] : cached->items())
            for (const auto& artwork : item.artwork) art.insert(artwork.id);
    status.artwork_objects = art.size();
    for (const auto& id : art)
        status.local_artwork_objects += local_.data().has(id) ? 1 : 0;
    const bool root_local = !status.root || local_.control().has(*status.root);
    status.ready = status.ready && root_local;
    return status;
}

CatalogueSnapshot CatalogueManager::snapshot() {
    return current_snapshot()->merged();
}

std::shared_ptr<const CatalogueView> CatalogueManager::snapshot_view() {
    return current_snapshot();
}

std::shared_ptr<const CatalogueView>
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
    const auto* found = snapshot->item(id);
    return found ? std::optional<CatalogueItem>{*found} : std::nullopt;
}

std::optional<MediaProbeResult> CatalogueManager::media_profile(std::string_view media_id) {
    auto snapshot = current_snapshot();
    const auto* found = snapshot->media_profile(media_id);
    if (!found || !valid_catalogue_media_profile(media_id, *found))
        return {};
    return found->probe;
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
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    if (const auto* found = current.media_profile(media_id); found && *found == profile)
        return;
    current.put_media_profile(std::move(media_id), std::move(profile));
    commit(expected_root, std::move(current));
}

void CatalogueManager::put_media_profiles(
    std::map<std::string, MediaProbeResult, std::less<>> profiles) {
    if (profiles.empty()) return;
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    bool changed = false;
    for (auto& [media_id, probe] : profiles) {
        CatalogueSnapshot::MediaProfile profile{catalogue_media_profile_schema, true, std::move(probe)};
        if (!valid_catalogue_media_profile(media_id, profile)) continue;
        if (const auto* found = current.media_profile(media_id); found && *found == profile)
            continue;
        current.put_media_profile(media_id, std::move(profile));
        changed = true;
    }
    if (!changed) return;
    commit(expected_root, std::move(current));
}

std::optional<Bytes> CatalogueManager::media_index(std::string_view media_id) {
    auto snapshot = current_snapshot();
    const auto* found = snapshot->media_index(media_id);
    if (!found) return {};
    return store_.get(*found, 0, FrameType::foreground);
}

void CatalogueManager::put_media_index(std::string media_id, std::span<const uint8_t> bytes) {
    if (!media_id.starts_with("macha:") || bytes.empty())
        throw std::invalid_argument("invalid media index");
    const auto id = object_id(bytes);
    if (!store_.put(id, bytes, FrameType::speculative))
        throw CatalogueUnavailable("cannot store media index in distributed DATA storage");
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    if (const auto* found = current.media_index(media_id); found && *found == id)
        return;
    current.put_media_index(std::move(media_id), id);
    commit(expected_root, std::move(current));
}

size_t CatalogueManager::prune_media_profiles(
    const std::set<std::string>& live_media_ids) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    std::vector<std::string> profiles;
    std::vector<std::string> indexes;
    current.for_each_media_profile([&](const std::string& media_id, const auto&) {
        if (!live_media_ids.contains(media_id))
            profiles.push_back(media_id);
    });
    current.for_each_media_index([&](const std::string& media_id, const auto&) {
        if (!live_media_ids.contains(media_id))
            indexes.push_back(media_id);
    });
    if (profiles.empty() && indexes.empty()) return 0;
    for (const auto& media_id : profiles)
        current.erase_media_profile(media_id);
    for (const auto& media_id : indexes)
        current.erase_media_index(media_id);
    commit(expected_root, std::move(current));
    {
        Lock lock(media_profile_mutex_);
        std::erase_if(resolved_media_profiles_, [&](const auto& item) {
            return !live_media_ids.contains(item.first);
        });
    }
    return profiles.size();
}

std::vector<CatalogueItem> CatalogueManager::list(std::optional<CatalogueKind> kind,
                                                  std::optional<std::string_view> parent) {
    auto snapshot = current_snapshot();
    std::vector<CatalogueItem> out;
    for (const auto& [_, item] : snapshot->items()) {
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
    for (const auto& [_, item] : snapshot->items()) {
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

std::pair<std::optional<ObjectId>, std::shared_ptr<const CatalogueView>>
CatalogueManager::installed() const {
    Lock lock(mutex_);
    return {cached_root_, cached_};
}

CatalogueManager::PreparedCommit CatalogueManager::prepare(CatalogueDraft&& draft) {
    PreparedCommit out;
    const auto& base = draft.base();
    std::set<ObjectId> before;
    std::set<ObjectId> after;
    for (size_t slot = 0; slot < catalogue_shard_count; ++slot) {
        const auto& was = base ? base->shards()[slot] : nullptr;
        if (!draft.written(slot)) {
            out.shards[slot] = was;
            continue;
        }
        out.touched[slot] = true;
        if (was) {
            const auto ids = data_object_ids(*was);
            before.insert(ids.begin(), ids.end());
        }
        out.shards[slot] = draft.take(slot);
        if (out.shards[slot]) {
            const auto ids = data_object_ids(*out.shards[slot]);
            after.insert(ids.begin(), ids.end());
        }
    }
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::inserter(out.added, out.added.end()));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::inserter(out.released, out.released.end()));
    // An object a touched shard let go of is released only if no untouched
    // shard still refers to it.
    if (!out.released.empty())
        for (size_t slot = 0; slot < catalogue_shard_count && !out.released.empty(); ++slot)
            if (!out.touched[slot] && out.shards[slot])
                for (const auto& id : data_object_ids(*out.shards[slot]))
                    out.released.erase(id);
    return out;
}

CatalogueManager::PreparedCommit CatalogueManager::prepare(const std::optional<ObjectId>& expected_root,
                                                           const CatalogueSnapshot& next,
                                                           const std::set<ObjectId>& old_artwork) {
    PreparedCommit out;
    const auto new_artwork = data_object_ids(next);
    std::set_difference(new_artwork.begin(), new_artwork.end(), old_artwork.begin(),
                        old_artwork.end(), std::inserter(out.added, out.added.end()));
    std::set_difference(old_artwork.begin(), old_artwork.end(), new_artwork.begin(),
                        new_artwork.end(), std::inserter(out.released, out.released.end()));

    // Which shards this write touches: those holding a key at which `next`
    // differs from the catalogue it started from. The rest keep the view's
    // shard and are neither built nor encoded. Without the catalogue at the
    // expected root to compare against, every shard is.
    out.touched.fill(true);
    std::shared_ptr<const CatalogueView> started_from;
    {
        Lock lock(mutex_);
        if (ready_ && cached_ && expected_root && cached_root_ == expected_root)
            started_from = cached_;
    }
    if (!started_from) {
        out.shards = CatalogueView::of(next).shards();
        return out;
    }
    out.touched.fill(false);
    std::array<const std::map<std::string, CatalogueItem>*, catalogue_shard_count> items{};
    std::array<const std::map<std::string, CatalogueSnapshot::MediaProfile, std::less<>>*,
               catalogue_shard_count>
        profiles{};
    std::array<const std::map<std::string, ObjectId, std::less<>>*, catalogue_shard_count>
        indexes{};
    for (size_t i = 0; i < catalogue_shard_count; ++i)
        if (const auto& shard = started_from->shards()[i]) {
            items[i] = &shard->items;
            profiles[i] = &shard->media_profiles;
            indexes[i] = &shard->media_indexes;
        }
    std::array<std::vector<decltype(next.items)::const_iterator>, catalogue_shard_count>
        added_items;
    std::array<std::vector<decltype(next.media_profiles)::const_iterator>, catalogue_shard_count>
        added_profiles;
    std::array<std::vector<decltype(next.media_indexes)::const_iterator>, catalogue_shard_count>
        added_indexes;
    diff_slots(items, next.items, out.touched, added_items);
    diff_slots(profiles, next.media_profiles, out.touched, added_profiles);
    diff_slots(indexes, next.media_indexes, out.touched, added_indexes);
    for (size_t i = 0; i < catalogue_shard_count; ++i) {
        if (!out.touched[i]) {
            out.shards[i] = started_from->shards()[i];
            continue;
        }
        CatalogueSnapshot shard;
        shard.items = rebuild_slot(items[i], next.items, added_items[i]);
        shard.media_profiles = rebuild_slot(profiles[i], next.media_profiles, added_profiles[i]);
        shard.media_indexes = rebuild_slot(indexes[i], next.media_indexes, added_indexes[i]);
        if (!shard.items.empty() || !shard.media_profiles.empty() || !shard.media_indexes.empty())
            out.shards[i] = std::make_shared<const CatalogueSnapshot>(std::move(shard));
    }
    return out;
}

void CatalogueManager::commit(const std::optional<ObjectId>& expected_root, CatalogueDraft&& draft,
                              std::optional<Hash256> expected_namespace, FrameType frame) {
    const auto started = Clock::now();
    auto prepared = prepare(std::move(draft));
    const auto prepare_ms = ms_since(started);
    const auto touched = std::count(prepared.touched.begin(), prepared.touched.end(), true);
    publish(expected_root, std::move(prepared), expected_namespace, std::nullopt, frame);
    Log::debug("catalogue commit touched=" + std::to_string(touched) +
               " prepare_ms=" + std::to_string(prepare_ms) +
               " publish_ms=" + std::to_string(ms_since(started) - prepare_ms));
}

void CatalogueManager::commit(
    const std::optional<ObjectId>& expected_root, const CatalogueSnapshot& next,
    const std::set<ObjectId>& old_artwork, std::optional<Hash256> expected_namespace,
    std::optional<std::pair<std::string, MetadataConflict>> resolved_conflict) {
    publish(expected_root, prepare(expected_root, next, old_artwork), expected_namespace,
            std::move(resolved_conflict));
}

void CatalogueManager::publish(
    const std::optional<ObjectId>& expected_root, PreparedCommit&& prepared,
    std::optional<Hash256> expected_namespace,
    std::optional<std::pair<std::string, MetadataConflict>> resolved_conflict, FrameType frame) {
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

    // Artwork is ordinary immutable DATA. Only new references are validated;
    // unchanged ones were proven by the committed catalogue.
    for (const auto& id : prepared.added) {
        // Held here is enough; only one that is not is fetched to prove it
        // exists somewhere.
        if (local_.data().has(id)) continue;
        if (!store_.get(id, 0, frame))
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

    CatalogueManifest manifest;
    std::vector<std::pair<ObjectId, Bytes>> changed_control;
    changed_control.reserve(catalogue_shard_count + 1);
    for (size_t i = 0; i < catalogue_shard_count; ++i) {
        if (!prepared.touched[i]) {
            manifest.shards[i] = old_manifest.shards[i];
            continue;
        }
        if (!prepared.shards[i])
            continue;
        auto encoded = encode_catalogue(*prepared.shards[i]);
        const auto id = object_id(encoded);
        manifest.shards[i] = id;
        if (old_manifest.shards[i] != id)
            changed_control.push_back({id, std::move(encoded)});
    }
    auto staged =
        std::make_shared<const CatalogueView>(std::move(prepared.shards), manifest.shards);

    auto encoded_manifest = encode_catalogue_manifest(manifest);
    const auto root = object_id(encoded_manifest);
    // An unchanged root is nothing to commit, unless a conflict is being
    // decided in its favour: that decision is the commit.
    if (expected_root && *expected_root == root && !resolved_conflict) {
        install_head(staged, root);
        return;
    }

    {
        Lock lock(mutex_);
        staged_ = staged;
        staged_root_ = root;
    }
    struct Unstage {
        CatalogueManager& self;
        const std::shared_ptr<const CatalogueView>& staged;
        ~Unstage() {
            Lock lock(self.mutex_);
            if (self.staged_ == staged) {
                self.staged_.reset();
                self.staged_root_.reset();
            }
        }
    } unstage{*this, staged};

    // A commit may reference a control object once this node holds it. The
    // other nodes are sent it with the commit's claims, and convergence
    // offers it to every active node, whatever its DATA capacity.
    for (const auto& [id, encoded] : changed_control) {
        if (!local_.control().put(id, encoded))
            throw CatalogueUnavailable("catalogue shard could not be stored");
    }
    if (!local_.control().put(root, encoded_manifest))
        throw CatalogueUnavailable("catalogue manifest could not be stored");

    const auto& released = prepared.released;
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
                retire_objects(metadata, delta, {released.begin(), released.end()});
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
                retire_objects(metadata, delta, {released.begin(), released.end()});
            });
        }
    } catch (const CatalogueConflict&) {
        throw;
    } catch (const std::exception& e) {
        throw CatalogueUnavailable(std::string("catalogue metadata durability unavailable: ") +
                                   e.what());
    }

    {
        Lock lock(mutex_);
        control_converged_root_.reset();
        control_converged_nodes_.clear();
        control_convergence_retry_ = {};
    }
    install_head(staged, root);
}

CatalogueItem CatalogueManager::upsert(CatalogueItem item,
                                        std::optional<uint64_t> expected_revision,
                                        FrameType frame) {
    if (item.id.empty())
        throw std::runtime_error("catalogue item id is required");
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    if (const auto* existing = current.item(item.id)) {
        if (expected_revision && existing->revision != *expected_revision)
            revision_changed(item.id, *expected_revision, existing->revision);
        item.revision = existing->revision + 1;
    } else {
        if (expected_revision)
            throw CatalogueConflict("catalogue item does not exist");
        item.revision = 1;
    }
    item.updated_ns = wall_time_ns();
    current.put(item);
    commit(expected_root, std::move(current), std::nullopt, frame);
    return item;
}

std::vector<CatalogueItem> CatalogueManager::upsert_many(std::vector<CatalogueItem> items) {
    if (items.empty()) return {};
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    const auto updated_ns = wall_time_ns();
    for (auto& item : items) {
        if (item.id.empty())
            throw std::runtime_error("catalogue item id is required");
        const auto* existing = current.item(item.id);
        item.revision = existing ? existing->revision + 1 : 1;
        item.updated_ns = updated_ns;
        current.put(item);
    }
    commit(expected_root, std::move(current));
    return items;
}

bool CatalogueManager::erase(std::string_view id, std::optional<uint64_t> expected_revision) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    const auto* existing = current.item(id);
    if (!existing)
        return false;
    if (expected_revision && existing->revision != *expected_revision)
        revision_changed(existing->id, *expected_revision, existing->revision);
    current.erase(id);
    commit(expected_root, std::move(current));
    return true;
}

bool CatalogueManager::definitely_absent(std::string_view id) const {
    std::optional<ObjectId> cached_root;
    uint64_t cached_generation{};
    {
        Lock lock(mutex_);
        if (!ready_ || !cached_)
            return false;
        if (cached_->item(id))
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
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);

    const auto* root = current.item(id);
    if (!root)
        return {};
    if (expected_revision && root->revision != *expected_revision)
        revision_changed(root->id, *expected_revision, root->revision);

    // Removes the entity and its descendants, returning their media ids so the
    // caller can re-enrich only those files.
    std::set<std::string> removed_ids{root->id};
    bool grew = true;
    while (grew) {
        grew = false;
        current.for_each_item([&](const std::string& candidate_id, const CatalogueItem& candidate) {
            if (removed_ids.contains(candidate_id) || !candidate.parent_id)
                return;
            if (removed_ids.contains(*candidate.parent_id)) {
                removed_ids.insert(candidate_id);
                grew = true;
            }
        });
    }

    std::set<std::string> media_ids;
    for (const auto& remove_id : removed_ids)
        if (const auto* item = current.item(remove_id))
            media_ids.insert(item->media_ids.begin(), item->media_ids.end());

    for (const auto& remove_id : removed_ids)
        current.erase(remove_id);
    commit(expected_root, std::move(current));

    CatalogueClearResult result;
    result.removed_items = removed_ids.size();
    result.media_ids.assign(media_ids.begin(), media_ids.end());
    return result;
}

CatalogueUnbindResult CatalogueManager::unbind_media(std::optional<std::string_view> item_id,
                                                     std::string_view media_id,
                                                     std::optional<uint64_t> expected_revision) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);

    CatalogueUnbindResult result;
    std::vector<std::string> unbound;
    if (item_id) {
        const auto* found = current.item(*item_id);
        if (!found)
            return result;
        result.found = true;
        if (expected_revision && found->revision != *expected_revision)
            revision_changed(found->id, *expected_revision, found->revision);
        if (std::find(found->media_ids.begin(), found->media_ids.end(), media_id) ==
            found->media_ids.end())
            return result;
        unbound.push_back(found->id);
    } else {
        result.found = true;
        current.for_each_item([&](const std::string& id, const CatalogueItem& item) {
            if (std::find(item.media_ids.begin(), item.media_ids.end(), media_id) !=
                item.media_ids.end())
                unbound.push_back(id);
        });
        if (unbound.empty())
            return result;
    }
    result.bound = true;

    const auto now = wall_time_ns();
    std::vector<std::string> emptied;
    for (const auto& id : unbound) {
        auto& item = *current.edit_item(id);
        std::erase(item.media_ids, std::string(media_id));
        ++item.revision;
        item.updated_ns = now;
        const bool leaf = item.kind == CatalogueKind::movie ||
                          item.kind == CatalogueKind::episode ||
                          item.kind == CatalogueKind::track;
        if (leaf && item.media_ids.empty())
            emptied.push_back(id);
    }
    // Each removal may leave its parent with no children: walk up from it.
    while (!emptied.empty()) {
        const auto id = emptied.back();
        emptied.pop_back();
        const auto* found = current.item(id);
        if (!found)
            continue;
        const auto parent = found->parent_id;
        current.erase(id);
        result.removed_ids.push_back(id);
        if (!parent)
            continue;
        bool has_child = false;
        current.for_each_item([&](const std::string&, const CatalogueItem& candidate) {
            has_child = has_child || (candidate.parent_id && *candidate.parent_id == *parent);
        });
        const auto* above = current.item(*parent);
        if (!has_child && above && above->media_ids.empty())
            emptied.push_back(*parent);
    }
    if (item_id)
        if (const auto* kept = current.item(*item_id))
            result.item = *kept;
    commit(expected_root, std::move(current));
    std::sort(result.removed_ids.begin(), result.removed_ids.end());
    return result;
}

size_t CatalogueManager::clear_metadata(std::string_view id,
                                        std::optional<uint64_t> expected_revision) {
    return clear_metadata_with_media(id, expected_revision).removed_items;
}

CatalogueArtwork CatalogueManager::stage_artwork(std::string role, std::string mime_type,
                                                   std::span<const uint8_t> bytes,
                                                   FrameType frame) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    CatalogueArtwork art{std::move(role), object_id(bytes), std::move(mime_type)};
    if (!store_.put_here(art.id, bytes, frame))
        throw std::runtime_error("cannot store artwork in distributed DATA storage");
    return art;
}

CatalogueArtwork CatalogueManager::stage_artwork_deferred(
    std::string role, std::string mime_type, std::span<const uint8_t> bytes,
    DistributedStore::DurabilityBatch& batch, FrameType frame) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    CatalogueArtwork art{std::move(role), object_id(bytes), std::move(mime_type)};
    // Written on this node, durable at the batch's barrier; no peer is waited
    // for.
    if (!store_.put_deferred_here(art.id, bytes, batch, frame))
        throw std::runtime_error("cannot stage artwork in distributed DATA storage");
    return art;
}

bool CatalogueManager::artwork_durability_barrier(DistributedStore::DurabilityBatch& batch,
                                                  FrameType frame) {
    return store_.durability_barrier(batch, frame);
}

void CatalogueManager::reconcile_scanner(const std::vector<CatalogueItem>& discovered,
                                         const std::set<std::string>& active_media_ids,
                                         bool prune_missing,
                                         std::optional<Hash256> expected_namespace,
                                         const std::map<std::string, MediaProbeResult, std::less<>>& profiles,
                                         const std::set<std::string>& vanished_media,
                                         FrameType frame) {
    TimedLock mutation_lock(mutation_mutex_, "catalogue.mutation");
    install_head();
    auto [expected_root, view] = installed();
    CatalogueDraft current(view);
    bool changed = false;

    for (const auto& [media_id, probe] : profiles) {
        CatalogueSnapshot::MediaProfile profile{catalogue_media_profile_schema, true, probe};
        if (!valid_catalogue_media_profile(media_id, profile)) continue;
        if (const auto* found = current.media_profile(media_id); found && *found == profile)
            continue;
        current.put_media_profile(media_id, std::move(profile));
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
        if (const auto* existing = current.item(item.id)) {
            const auto manual = existing->external_ids.find("macha_metadata_locked");
            const bool metadata_locked =
                manual != existing->external_ids.end() && manual->second == "1";
            if (metadata_locked) {
                // A user-edited item keeps its descriptive metadata; its media
                // bindings are still reconciled.
                auto preserved = *existing;
                preserved.media_ids.insert(preserved.media_ids.end(), item.media_ids.begin(),
                                           item.media_ids.end());
                std::sort(preserved.media_ids.begin(), preserved.media_ids.end());
                preserved.media_ids.erase(
                    std::unique(preserved.media_ids.begin(), preserved.media_ids.end()),
                    preserved.media_ids.end());
                if (preserved.media_ids == existing->media_ids)
                    continue;
                ++preserved.revision;
                preserved.updated_ns = wall_time_ns();
                current.put(std::move(preserved));
                changed = true;
                continue;
            }
            // Scanner artwork is a candidate set, not one slot per role: known
            // objects are kept unless resupplied, so an embedded cover and a
            // provider cover coexist.
            for (const auto& art : existing->artwork) {
                const bool already_present = std::any_of(
                    item.artwork.begin(), item.artwork.end(), [&](const auto& candidate) {
                        return candidate.role == art.role && candidate.id == art.id;
                    });
                if (!already_present) item.artwork.push_back(art);
            }
            // A match may be another file of a known item: keep existing
            // bindings; vanished ones are removed below.
            item.media_ids.insert(item.media_ids.end(), existing->media_ids.begin(),
                                  existing->media_ids.end());
            std::sort(item.media_ids.begin(), item.media_ids.end());
            item.media_ids.erase(std::unique(item.media_ids.begin(), item.media_ids.end()),
                                 item.media_ids.end());
            item.revision = existing->revision;
            item.updated_ns = existing->updated_ns;
            if (same_content(item, *existing))
                continue;
            item.revision = existing->revision + 1;
        } else {
            item.revision = 1;
        }
        item.updated_ns = wall_time_ns();
        current.put(std::move(item));
        changed = true;
    }

    if (prune_missing) {
        const auto scanner_made = [](const CatalogueItem& item) {
            const auto marker = item.external_ids.find("macha_scanner");
            return marker != item.external_ids.end() && marker->second == "1";
        };
        const auto leaf = [](const CatalogueItem& item) {
            return item.kind == CatalogueKind::movie || item.kind == CatalogueKind::episode ||
                   item.kind == CatalogueKind::track;
        };
        // Only scanner-owned leaf bindings are reconciled; a manual item may
        // bind a file outside the catalogue roots, so it loses only files gone
        // from the namespace altogether.
        std::vector<std::string> rebind;
        current.for_each_item([&](const std::string& id, const CatalogueItem& item) {
            const bool scanner = scanner_made(item);
            if (scanner && !leaf(item))
                return;
            if (!scanner && vanished_media.empty())
                return;
            const bool loses = std::any_of(
                item.media_ids.begin(), item.media_ids.end(), [&](const std::string& media) {
                    return scanner ? !active_media_ids.contains(media)
                                   : vanished_media.contains(media);
                });
            if (loses)
                rebind.push_back(id);
        });
        for (const auto& id : rebind) {
            auto& item = *current.edit_item(id);
            const bool scanner = scanner_made(item);
            std::erase_if(item.media_ids, [&](const std::string& media) {
                return scanner ? !active_media_ids.contains(media) : vanished_media.contains(media);
            });
            ++item.revision;
            item.updated_ns = wall_time_ns();
            changed = true;
        }

        std::vector<std::string> emptied;
        current.for_each_item([&](const std::string& id, const CatalogueItem& item) {
            if (scanner_made(item) && leaf(item) && item.media_ids.empty())
                emptied.push_back(id);
        });
        for (const auto& id : emptied) {
            current.erase(id);
            changed = true;
        }

        // Bottom-up removal of empty scanner-created hierarchy nodes.
        const auto parent_kind = [](const CatalogueItem& item) {
            return item.kind == CatalogueKind::show || item.kind == CatalogueKind::season ||
                   item.kind == CatalogueKind::artist || item.kind == CatalogueKind::album;
        };
        bool removed = true;
        while (removed) {
            removed = false;
            std::set<std::string> parents;
            current.for_each_item([&](const std::string&, const CatalogueItem& item) {
                if (item.parent_id)
                    parents.insert(*item.parent_id);
            });
            std::vector<std::string> childless;
            current.for_each_item([&](const std::string& id, const CatalogueItem& item) {
                if (scanner_made(item) && parent_kind(item) && !parents.contains(id))
                    childless.push_back(id);
            });
            for (const auto& id : childless) {
                current.erase(id);
                changed = removed = true;
            }
        }
    }

    if (changed)
        commit(expected_root, std::move(current), expected_namespace, frame);
}

CatalogueArtwork CatalogueManager::put_artwork(std::string_view item_id, std::string role,
                                                std::string mime_type,
                                                std::span<const uint8_t> bytes,
                                                std::optional<uint64_t> expected_revision,
                                                FrameType frame) {
    if (bytes.empty())
        throw std::runtime_error("artwork body is empty");
    auto item = get(item_id);
    if (!item)
        throw std::runtime_error("catalogue item not found");
    if (expected_revision && item->revision != *expected_revision)
        revision_changed(item->id, *expected_revision, item->revision);

    CatalogueArtwork art = stage_artwork(std::move(role), std::move(mime_type), bytes, frame);

    std::erase_if(item->artwork, [&](const CatalogueArtwork& existing) {
        return existing.role == art.role;
    });
    item->artwork.push_back(art);
    try {
        (void)upsert(*item, item->revision, frame);
    } catch (...) {
        // Content-addressed: a concurrent commit may make this hash live, so
        // failed staging is left as an orphan for reachability GC.
        throw;
    }
    return art;
}

std::shared_ptr<const CatalogueIndexes> CatalogueManager::indexes() {
    const auto current = current_snapshot();
    {
        Lock lock(mutex_);
        if (indexes_ && indexed_ == current)
            return indexes_;
    }
    auto built = std::make_shared<CatalogueIndexes>();
    for (const auto& [id, item] : current->items()) {
        for (const auto& art : item.artwork)
            built->artwork_types.emplace(art.id, art.mime_type);
        for (const auto& media_id : item.media_ids)
            built->media_bindings[media_id].push_back(id);
    }
    Lock lock(mutex_);
    indexed_ = current;
    indexes_ = built;
    return built;
}

std::optional<CatalogueArtworkContent> CatalogueManager::artwork(const ObjectId& id) {
    const auto index = indexes();
    std::optional<std::string> mime_type;
    if (const auto found = index->artwork_types.find(id); found != index->artwork_types.end())
        mime_type = found->second;
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

CatalogueMaintenance CatalogueManager::maintenance_objects(const CatalogueMaintenanceHead& head) {
    CatalogueMaintenance out;
    const auto& metadata_root = head.root;
    const auto& metadata_roots = head.roots;
    const bool metadata_current = head.current;
    // A root conflict's alternatives stay durable until resolved: protect
    // their roots, manifests, shards and artwork from GC.
    out.control_live.insert(metadata_roots.begin(), metadata_roots.end());
    bool root_readable = true;
    std::optional<ObjectId> root;
    std::shared_ptr<const CatalogueView> cached;
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
            root_readable = false;
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
        const bool root_converged = ready_ && cached_root_ == metadata_root;
        out.complete = metadata_current && root_readable && root_converged &&
                       protected_roots_complete;
    }
    if (cached) {
        for (const auto& id : data_object_ids(*cached))
            out.live.insert(id);
    }
    return out;
}

size_t CatalogueManager::control_gc_step(const IdLookup& live,
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

        if (live.contains(*id)) {
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
