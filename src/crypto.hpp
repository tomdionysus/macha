// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
#include <filesystem>
#include <memory>
namespace macha {
struct ClusterKeys {
    std::array<uint8_t, 32> master{}, auth{}, storage{};
    std::array<uint8_t, 16> cluster_id{};
};
struct X25519KeyPair {
    std::array<uint8_t, 32> private_key{};
    std::array<uint8_t, 32> public_key{};
};
struct SealedData {
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    Bytes ciphertext;
};
ClusterKeys load_cluster_keys(const std::filesystem::path&);
Bytes random_bytes(size_t);
NodeId random_node_id();

class Sha256Hasher {
    struct State;
    std::unique_ptr<State> state_;

  public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(Sha256Hasher&&) noexcept;
    Sha256Hasher& operator=(Sha256Hasher&&) noexcept;
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    void update(std::span<const uint8_t>);
    Hash256 finish();
};

Hash256 sha256(std::span<const uint8_t>);
ObjectId object_id(std::span<const uint8_t>);
std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t>, std::span<const uint8_t>);
std::array<uint8_t, 32> hkdf_sha256(std::span<const uint8_t>, std::span<const uint8_t>,
                                    std::span<const uint8_t>);
SealedData aes_gcm_seal(std::span<const uint8_t, 32>, std::span<const uint8_t>,
                        std::span<const uint8_t> aad = {});
Bytes aes_gcm_open(std::span<const uint8_t, 32>, std::span<const uint8_t, 12>,
                   std::span<const uint8_t, 16>, std::span<const uint8_t>,
                   std::span<const uint8_t> aad = {});
X25519KeyPair x25519_generate();
std::array<uint8_t, 32> x25519_shared(std::span<const uint8_t, 32>,
                                      std::span<const uint8_t, 32>);
bool constant_time_equal(std::span<const uint8_t>, std::span<const uint8_t>);
} // namespace macha
