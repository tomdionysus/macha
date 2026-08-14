// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <stdexcept>
namespace macha {
namespace {
using C = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using M = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using P = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
std::array<uint8_t, 32> derive(std::span<const uint8_t, 32> k, const char* l) {
    return hkdf_sha256(k, {}, std::span<const uint8_t>((const uint8_t*)l, strlen(l)));
}
} // namespace
Hash256 sha256(std::span<const uint8_t> d) {
    Hash256 o;
    M c(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    unsigned n = 0;
    if (!c || EVP_DigestInit_ex(c.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(c.get(), d.data(), d.size()) != 1 ||
        EVP_DigestFinal_ex(c.get(), o.bytes.data(), &n) != 1 || n != 32)
        throw std::runtime_error("SHA256 failed");
    return o;
}
ObjectId object_id(std::span<const uint8_t> d) {
    ObjectId o;
    o.bytes = sha256(d).bytes;
    return o;
}
Bytes random_bytes(size_t n) {
    Bytes b(n);
    if (n && RAND_bytes(b.data(), n) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return b;
}
NodeId random_node_id() {
    NodeId i;
    if (RAND_bytes(i.bytes.data(), i.bytes.size()) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return i;
}
std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> k, std::span<const uint8_t> d) {
    std::array<uint8_t, 32> o{};
    unsigned n = 0;
    if (!HMAC(EVP_sha256(), k.data(), k.size(), d.data(), d.size(), o.data(), &n) || n != 32)
        throw std::runtime_error("HMAC failed");
    return o;
}
std::array<uint8_t, 32> hkdf_sha256(std::span<const uint8_t> k, std::span<const uint8_t> s,
                                    std::span<const uint8_t> i) {
    std::array<uint8_t, 32> o{};
    P c(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
    if (!c || EVP_PKEY_derive_init(c.get()) <= 0 ||
        EVP_PKEY_CTX_set_hkdf_md(c.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_key(c.get(), k.data(), k.size()) <= 0)
        throw std::runtime_error("HKDF failed");
    if (!s.empty() && EVP_PKEY_CTX_set1_hkdf_salt(c.get(), s.data(), s.size()) <= 0)
        throw std::runtime_error("HKDF salt failed");
    if (!i.empty() && EVP_PKEY_CTX_add1_hkdf_info(c.get(), i.data(), i.size()) <= 0)
        throw std::runtime_error("HKDF info failed");
    size_t n = o.size();
    if (EVP_PKEY_derive(c.get(), o.data(), &n) <= 0 || n != 32)
        throw std::runtime_error("HKDF derive failed");
    return o;
}
ClusterKeys load_cluster_keys(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open key file");
    Bytes b(std::istreambuf_iterator<char>(f), {});
    while (!b.empty() && (b.back() == '\n' || b.back() == '\r'))
        b.pop_back();
    if (b.empty())
        throw std::runtime_error("empty key file");
    ClusterKeys k;
    k.master = sha256(b).bytes;
    k.auth = derive(k.master, "macha/auth/v1");
    k.storage = derive(k.master, "macha/storage/v1");
    auto c = derive(k.master, "macha/cluster-id/v1");
    std::copy_n(c.begin(), 16, k.cluster_id.begin());
    OPENSSL_cleanse(b.data(), b.size());
    return k;
}
SealedData aes_gcm_seal(std::span<const uint8_t, 32> k, std::span<const uint8_t> p,
                        std::span<const uint8_t> a) {
    SealedData o;
    auto n = random_bytes(12);
    std::copy(n.begin(), n.end(), o.nonce.begin());
    o.ciphertext.resize(p.size());
    C c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int produced = 0;
    if (!c || EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_EncryptInit_ex(c.get(), nullptr, nullptr, k.data(), o.nonce.data()) != 1)
        throw std::runtime_error("AES-GCM init failed");
    if (!a.empty()) {
        int aad_consumed = 0;
        if (EVP_EncryptUpdate(c.get(), nullptr, &aad_consumed, a.data(), a.size()) != 1)
            throw std::runtime_error("AES-GCM AAD failed");
    }
    int total = 0;
    if (!p.empty()) {
        if (EVP_EncryptUpdate(c.get(), o.ciphertext.data(), &produced, p.data(), p.size()) != 1)
            throw std::runtime_error("AES-GCM encrypt failed");
        total = produced;
    }
    std::array<uint8_t, 16> final_buffer{};
    uint8_t* final_output = o.ciphertext.empty() ? final_buffer.data()
                                                  : o.ciphertext.data() + total;
    if (EVP_EncryptFinal_ex(c.get(), final_output, &produced) != 1)
        throw std::runtime_error("AES-GCM final failed");
    total += produced;
    o.ciphertext.resize(static_cast<size_t>(total));
    if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, 16, o.tag.data()) != 1)
        throw std::runtime_error("AES-GCM tag failed");
    return o;
}
Bytes aes_gcm_open(std::span<const uint8_t, 32> k, std::span<const uint8_t, 12> n,
                   std::span<const uint8_t, 16> tag, std::span<const uint8_t> ct,
                   std::span<const uint8_t> a) {
    Bytes o(ct.size());
    C c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int produced = 0;
    if (!c || EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_DecryptInit_ex(c.get(), nullptr, nullptr, k.data(), n.data()) != 1)
        throw std::runtime_error("AES-GCM init failed");
    if (!a.empty()) {
        int aad_consumed = 0;
        if (EVP_DecryptUpdate(c.get(), nullptr, &aad_consumed, a.data(), a.size()) != 1)
            throw std::runtime_error("AES-GCM AAD failed");
    }
    int total = 0;
    if (!ct.empty()) {
        if (EVP_DecryptUpdate(c.get(), o.data(), &produced, ct.data(), ct.size()) != 1)
            throw std::runtime_error("AES-GCM decrypt failed");
        total = produced;
    }
    if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag.data())) !=
        1)
        throw std::runtime_error("AES-GCM tag failed");
    std::array<uint8_t, 16> final_buffer{};
    uint8_t* final_output = o.empty() ? final_buffer.data() : o.data() + total;
    if (EVP_DecryptFinal_ex(c.get(), final_output, &produced) != 1)
        throw std::runtime_error("AES-GCM authentication failed");
    total += produced;
    o.resize(static_cast<size_t>(total));
    return o;
}
X25519KeyPair x25519_generate() {
    P context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0)
        throw std::runtime_error("X25519 keygen init failed");

    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw) <= 0)
        throw std::runtime_error("X25519 keygen failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);

    X25519KeyPair out;
    size_t private_size = out.private_key.size();
    size_t public_size = out.public_key.size();
    if (EVP_PKEY_get_raw_private_key(key.get(), out.private_key.data(), &private_size) <= 0 ||
        EVP_PKEY_get_raw_public_key(key.get(), out.public_key.data(), &public_size) <= 0 ||
        private_size != out.private_key.size() || public_size != out.public_key.size()) {
        throw std::runtime_error("X25519 raw key export failed");
    }
    return out;
}

std::array<uint8_t, 32> x25519_shared(std::span<const uint8_t, 32> private_key,
                                      std::span<const uint8_t, 32> peer_public_key) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> local(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(),
                                     private_key.size()),
        EVP_PKEY_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> peer(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public_key.data(),
                                    peer_public_key.size()),
        EVP_PKEY_free);
    if (!local || !peer)
        throw std::runtime_error("X25519 raw key import failed");

    P context(EVP_PKEY_CTX_new(local.get(), nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0 ||
        EVP_PKEY_derive_set_peer(context.get(), peer.get()) <= 0) {
        throw std::runtime_error("X25519 derive init failed");
    }

    std::array<uint8_t, 32> out{};
    size_t size = out.size();
    if (EVP_PKEY_derive(context.get(), out.data(), &size) <= 0 || size != out.size())
        throw std::runtime_error("X25519 derive failed");
    return out;
}

bool constant_time_equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
} // namespace macha
