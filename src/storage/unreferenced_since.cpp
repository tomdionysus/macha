// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/unreferenced_since.hpp"

#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <fstream>
#include <iterator>
#include <string_view>

namespace macha {

namespace {
constexpr uint32_t persisted_schema = 1;
}

Bytes encode_unreferenced_since(const std::map<ObjectId, uint64_t>& sightings) {
    Writer out;
    out.u32(persisted_schema);
    out.u64(sightings.size());
    for (const auto& [id, first_unix_ms] : sightings) {
        out.fixed(id.bytes);
        out.u64(first_unix_ms);
    }
    return out.take();
}

std::map<ObjectId, uint64_t> decode_unreferenced_since(std::span<const uint8_t> bytes) {
    Reader in(bytes);
    if (in.u32() != persisted_schema)
        throw DecodeError("unknown unreferenced-since schema");
    std::map<ObjectId, uint64_t> sightings;
    const auto count = in.u64();
    for (uint64_t i = 0; i < count; ++i) {
        ObjectId id{in.fixed<32>()};
        const auto first_unix_ms = in.u64();
        if (!sightings.emplace(id, first_unix_ms).second)
            throw DecodeError("duplicate unreferenced-since object");
    }
    in.finish();
    return sightings;
}

UnreferencedSince::UnreferencedSince(std::filesystem::path path) : path_(std::move(path)) {
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        return;
    const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        Lock lock(mutex_);
        for (const auto& [id, first_unix_ms] : decode_unreferenced_since(bytes))
            seen_.emplace(id, Sighting{first_unix_ms, true});
    } catch (const DecodeError& error) {
        Log::warn("unreferenced-since: ignoring " + path_.string() + ": " + error.what());
    }
}

bool UnreferencedSince::matured(const ObjectId& id, uint64_t now_unix_ms,
                                std::chrono::milliseconds grace) {
    Lock lock(mutex_);
    auto [found, inserted] = seen_.emplace(id, Sighting{now_unix_ms, true});
    if (inserted)
        dirty_ = true;
    found->second.renewed = true;
    // A clock set back restarts the wait rather than ending it early.
    if (found->second.first_unix_ms > now_unix_ms) {
        found->second.first_unix_ms = now_unix_ms;
        dirty_ = true;
    }
    return now_unix_ms - found->second.first_unix_ms >= static_cast<uint64_t>(grace.count());
}

void UnreferencedSince::forget(const ObjectId& id) {
    Lock lock(mutex_);
    if (seen_.erase(id))
        dirty_ = true;
}

void UnreferencedSince::pass_complete() {
    Lock lock(mutex_);
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (!it->second.renewed) {
            it = seen_.erase(it);
            dirty_ = true;
        } else {
            it->second.renewed = false;
            ++it;
        }
    }
}

void UnreferencedSince::save() {
    std::map<ObjectId, uint64_t> sightings;
    {
        Lock lock(mutex_);
        if (!dirty_)
            return;
        for (const auto& [id, sighting] : seen_)
            sightings.emplace(id, sighting.first_unix_ms);
        dirty_ = false;
    }
    const auto bytes = encode_unreferenced_since(sightings);
    try {
        std::filesystem::create_directories(path_.parent_path());
        durable_replace_file(path_, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                     bytes.size()));
    } catch (const std::exception& error) {
        Log::warn("unreferenced-since: cannot write " + path_.string() + ": " + error.what());
        Lock lock(mutex_);
        dirty_ = true;
    }
}

size_t UnreferencedSince::size() const {
    Lock lock(mutex_);
    return seen_.size();
}

} // namespace macha
