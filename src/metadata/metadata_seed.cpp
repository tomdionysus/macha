// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata/metadata_seed.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "log.hpp"

#include "dagda/domain/domain_id.h"
#include "dannan/identity/durable_file.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace macha {
namespace {

constexpr std::array<uint8_t, 8> seed_magic{'M', 'A', 'C', 'H', 'S', 'E', 'D', '1'};

// The magic and the domain's id, so a seed opens only as this file of this domain.
Bytes seed_aad() {
    Bytes aad(seed_magic.begin(), seed_magic.end());
    aad.insert(aad.end(), dagda::domain::domain_v1.bytes.begin(),
               dagda::domain::domain_v1.bytes.end());
    return aad;
}

} // namespace

MetadataSeed::MetadataSeed(std::filesystem::path state_path, std::array<uint8_t, 32> key)
    : path_(std::move(state_path) / "metadata.seed"), key_(key) {}

void MetadataSeed::remember(const MetadataRecord& record) {
    Lock lock(mutex_);
    if (kept_ && *kept_ == record.hash)
        return;
    try {
        const auto aad = seed_aad();
        const auto sealed = aes_gcm_seal(key_, encode_metadata_record(record), aad);
        Writer writer;
        writer.raw(seed_magic);
        writer.fixed(sealed.nonce);
        writer.fixed(sealed.tag);
        writer.bytes(sealed.ciphertext);
        const auto& bytes = writer.data();
        dannan::identity::durable_replace_file(
            path_, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        kept_ = record.hash;
    } catch (const std::exception& error) {
        Log::debug("metadata seed write: " + std::string(error.what()));
    }
}

std::optional<MetadataRecord> MetadataSeed::read() const {
    Lock lock(mutex_);
    try {
        if (!std::filesystem::exists(path_))
            return {};
        std::ifstream in(path_, std::ios::binary);
        const Bytes bytes(std::istreambuf_iterator<char>(in), {});
        Reader reader(bytes);
        const auto magic = reader.raw(seed_magic.size());
        if (!std::equal(magic.begin(), magic.end(), seed_magic.begin()))
            throw std::runtime_error("bad metadata seed header");
        const auto nonce = reader.fixed<12>();
        const auto tag = reader.fixed<16>();
        const auto ciphertext = reader.bytes();
        reader.finish();
        auto record =
            decode_metadata_record(aes_gcm_open(key_, nonce, tag, ciphertext, seed_aad()));
        kept_ = record.hash;
        return record;
    } catch (const std::exception& error) {
        Log::debug("metadata seed read: " + std::string(error.what()));
        return {};
    }
}

} // namespace macha
