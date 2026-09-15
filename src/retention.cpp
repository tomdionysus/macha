// SPDX-License-Identifier: GPL-3.0-or-later
#include "retention.hpp"

#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'R', 'T', 'J', '1'};
constexpr std::array<uint8_t, 8> checkpoint_aad{'M', 'A', 'C', 'H', 'R', 'T', 'S', '1'};
constexpr std::array<uint8_t, 8> legacy_checkpoint_magic{'M', 'R', 'T', 'S', '0', '0', '0', '1'};
constexpr std::array<uint8_t, 8> shard_checkpoint_magic{'M', 'R', 'T', 'S', '0', '0', '0', '2'};
constexpr uint8_t op_add = 1;
constexpr uint8_t op_release = 2;
constexpr uint32_t max_legacy_frame = 64U * 1024U * 1024U;
constexpr uint32_t max_journal_frame = 4U * 1024U * 1024U;
constexpr uint64_t journal_compact_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t retain_ids_per_frame = 65536;
constexpr uint64_t max_checkpoint_shard_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t checkpoint_shards = 256;

void write_all(int fd, std::span<const uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::runtime_error(std::string("retention journal write failed: ") +
                                     std::strerror(errno));
        done += static_cast<size_t>(n);
    }
}

bool pread_all(int fd, std::span<uint8_t> bytes, uint64_t offset) {
    size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::pread(fd, bytes.data() + done, bytes.size() - done,
                               static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

void fsync_checked(int fd) {
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    if (rc != 0)
        throw std::runtime_error(std::string("retention journal sync failed: ") +
                                 std::strerror(errno));
}

void sync_directory(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        throw std::runtime_error("cannot open retention directory for sync: " + path.string());
    try {
        fsync_checked(fd);
        if (::close(fd) != 0)
            throw std::runtime_error("cannot close retention directory: " +
                                     std::string(std::strerror(errno)));
    } catch (...) {
        ::close(fd);
        throw;
    }
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24U) |
           (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) |
           static_cast<uint32_t>(p[3]);
}

std::array<uint8_t, 4> be32(uint32_t value) {
    return {static_cast<uint8_t>(value >> 24U), static_cast<uint8_t>(value >> 16U),
            static_cast<uint8_t>(value >> 8U), static_cast<uint8_t>(value)};
}

bool valid_class(uint8_t value) {
    return value == static_cast<uint8_t>(RetentionClass::data) ||
           value == static_cast<uint8_t>(RetentionClass::control);
}

std::string shard_name(uint8_t shard) {
    std::ostringstream out;
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(shard)
        << ".meta";
    return out.str();
}

bool valid_generation_name(std::string_view value) {
    return value.starts_with("gen-") && value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos && value.size() <= 128;
}

std::string read_small_text(const std::filesystem::path& path, size_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (size > limit)
        throw std::runtime_error("retention checkpoint manifest is too large");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read retention checkpoint manifest " + path.string());
    std::string text(static_cast<size_t>(size), '\0');
    if (size && !input.read(text.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read retention checkpoint manifest " + path.string());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t'))
        text.pop_back();
    return text;
}
} // namespace

RetentionStore::RetentionStore(std::filesystem::path state_path, std::array<uint8_t, 32> key)
    : legacy_checkpoint_path_(state_path / "retention" / "claims.meta"),
      checkpoint_root_(state_path / "retention" / "checkpoints"),
      checkpoint_manifest_path_(state_path / "retention" / "claims.current"),
      journal_path_(std::move(state_path) / "retention" / "claims.log"), key_(key) {
    std::filesystem::create_directories(journal_path_.parent_path());
    std::filesystem::create_directories(checkpoint_root_);
    load();
}

RetentionStore::StateMap& RetentionStore::state_for(RetentionClass type) {
    return type == RetentionClass::data ? data_ : control_;
}

const RetentionStore::StateMap& RetentionStore::state_for(RetentionClass type) const {
    return type == RetentionClass::data ? data_ : control_;
}

std::optional<ObjectId>& RetentionStore::cursor_for(RetentionClass type) {
    return type == RetentionClass::data ? data_release_after_ : control_release_after_;
}

std::optional<ObjectId>& RetentionStore::prune_cursor_for(RetentionClass type) {
    return type == RetentionClass::data ? data_prune_after_ : control_prune_after_;
}

void RetentionStore::apply_add_locked(RetentionClass type, const RetentionDot& dot,
                                      const std::vector<ObjectId>& ids) {
    if (dot.origin == NodeId{} || !dot.sequence)
        throw std::runtime_error("invalid retention mutation dot");
    auto& state = state_for(type);
    for (const auto& id : ids) {
        auto& object = state[id];
        const auto removed = object.removed.find(dot.origin);
        if (removed != object.removed.end() && removed->second >= dot.sequence)
            continue;
        auto& current = object.adds[dot.origin];
        current = std::max(current, dot.sequence);
    }
}

size_t RetentionStore::apply_release_locked(RetentionClass type, const RetentionClock& observed,
                                            const std::vector<ObjectId>& ids) {
    auto& state = state_for(type);
    size_t changed = 0;
    for (const auto& id : ids) {
        auto found = state.find(id);
        if (found == state.end())
            continue;
        bool object_changed = false;
        for (const auto& [origin, sequence] : observed) {
            if (!sequence)
                continue;
            auto& removed = found->second.removed[origin];
            if (removed < sequence) {
                removed = sequence;
                object_changed = true;
            }
            auto add = found->second.adds.find(origin);
            if (add != found->second.adds.end() && add->second <= sequence) {
                found->second.adds.erase(add);
                object_changed = true;
            }
        }
        if (object_changed)
            ++changed;
    }
    return changed;
}

void RetentionStore::append_frame_locked(std::span<const uint8_t> plaintext) {
    const auto sealed = aes_gcm_seal(key_, plaintext, journal_aad);
    if (sealed.ciphertext.size() > max_journal_frame)
        throw std::runtime_error("retention journal frame too large");
    const auto length = be32(static_cast<uint32_t>(sealed.ciphertext.size()));
    std::vector<uint8_t> frame;
    frame.reserve(4 + sealed.nonce.size() + sealed.tag.size() + sealed.ciphertext.size());
    frame.insert(frame.end(), length.begin(), length.end());
    frame.insert(frame.end(), sealed.nonce.begin(), sealed.nonce.end());
    frame.insert(frame.end(), sealed.tag.begin(), sealed.tag.end());
    frame.insert(frame.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());

    const int fd = ::open(journal_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot open retention journal " + journal_path_.string() +
                                 ": " + std::strerror(errno));
    try {
        write_all(fd, frame);
        fsync_checked(fd);
        if (::close(fd) != 0)
            throw std::runtime_error("cannot close retention journal: " +
                                     std::string(std::strerror(errno)));
    } catch (...) {
        ::close(fd);
        throw;
    }
    ++journal_records_;
    journal_bytes_ += frame.size();
}

Bytes RetentionStore::encode_checkpoint_shard_locked(uint8_t shard) const {
    Writer plain;
    plain.u8(2);
    plain.u8(shard);

    auto range_for = [&](const StateMap& state) {
        ObjectId low{};
        low.bytes[0] = shard;
        auto begin = state.lower_bound(low);
        if (shard == 255)
            return std::pair{begin, state.end()};
        ObjectId high{};
        high.bytes[0] = static_cast<uint8_t>(shard + 1);
        return std::pair{begin, state.lower_bound(high)};
    };

    auto encode_state = [&](const StateMap& state) {
        const auto [begin, end] = range_for(state);
        const auto count = static_cast<uint64_t>(std::distance(begin, end));
        if (count > UINT32_MAX)
            throw std::runtime_error("too many retention objects in checkpoint shard");
        plain.u32(static_cast<uint32_t>(count));
        for (auto it = begin; it != end; ++it) {
            const auto& [id, object] = *it;
            plain.fixed(id.bytes);
            if (object.adds.size() > UINT32_MAX || object.removed.size() > UINT32_MAX)
                throw std::runtime_error("too many retention clocks for one object");
            plain.u32(static_cast<uint32_t>(object.adds.size()));
            for (const auto& [origin, sequence] : object.adds) {
                plain.fixed(origin.bytes);
                plain.u64(sequence);
            }
            plain.u32(static_cast<uint32_t>(object.removed.size()));
            for (const auto& [origin, sequence] : object.removed) {
                plain.fixed(origin.bytes);
                plain.u64(sequence);
            }
        }
    };
    encode_state(data_);
    encode_state(control_);

    const auto sealed = aes_gcm_seal(key_, plain.data(), checkpoint_aad);
    if (sealed.ciphertext.size() > max_checkpoint_shard_bytes)
        throw std::runtime_error("retention checkpoint shard too large");
    Writer outer;
    outer.fixed(shard_checkpoint_magic);
    outer.fixed(sealed.nonce);
    outer.fixed(sealed.tag);
    outer.bytes(sealed.ciphertext);
    return outer.take();
}

void RetentionStore::decode_checkpoint_shard_locked(uint8_t shard,
                                                     std::span<const uint8_t> encoded) {
    Reader outer(encoded);
    if (outer.fixed<8>() != shard_checkpoint_magic)
        throw DecodeError("bad retention checkpoint shard magic");
    const auto nonce = outer.fixed<12>();
    const auto tag = outer.fixed<16>();
    const auto ciphertext = outer.bytes(max_checkpoint_shard_bytes);
    outer.finish();
    const auto plain = aes_gcm_open(key_, nonce, tag, ciphertext, checkpoint_aad);
    Reader reader(plain);
    if (reader.u8() != 2 || reader.u8() != shard)
        throw DecodeError("bad retention checkpoint shard identity");

    auto decode_state = [&](StateMap& state) {
        const auto count = reader.u32();
        // A 64 MiB shard cannot contain anywhere near UINT32_MAX valid records,
        // but keep an explicit structural bound before any map growth.
        if (count > 1'500'000)
            throw DecodeError("too many retention checkpoint shard objects");
        for (uint32_t i = 0; i < count; ++i) {
            ObjectId id{reader.fixed<32>()};
            if (id.bytes[0] != shard)
                throw DecodeError("retention object stored in wrong checkpoint shard");
            ObjectState object;
            const auto adds = reader.u32();
            if (adds > 65536)
                throw DecodeError("too many retention checkpoint adds");
            for (uint32_t j = 0; j < adds; ++j) {
                NodeId origin{reader.fixed<16>()};
                const auto sequence = reader.u64();
                if (origin == NodeId{} || !sequence ||
                    !object.adds.emplace(origin, sequence).second)
                    throw DecodeError("bad retention checkpoint add");
            }
            const auto removed = reader.u32();
            if (removed > 65536)
                throw DecodeError("too many retention checkpoint removes");
            for (uint32_t j = 0; j < removed; ++j) {
                NodeId origin{reader.fixed<16>()};
                const auto sequence = reader.u64();
                if (origin == NodeId{} || !sequence ||
                    !object.removed.emplace(origin, sequence).second)
                    throw DecodeError("bad retention checkpoint remove");
            }
            for (auto it = object.adds.begin(); it != object.adds.end();) {
                const auto rm = object.removed.find(it->first);
                if (rm != object.removed.end() && rm->second >= it->second)
                    it = object.adds.erase(it);
                else
                    ++it;
            }
            if (!state.emplace(id, std::move(object)).second)
                throw DecodeError("duplicate retention checkpoint object");
        }
    };
    decode_state(data_);
    decode_state(control_);
    reader.finish();
}

void RetentionStore::decode_legacy_checkpoint_locked(std::span<const uint8_t> encoded) {
    Reader outer(encoded);
    if (outer.fixed<8>() != legacy_checkpoint_magic)
        throw DecodeError("bad retention checkpoint magic");
    const auto nonce = outer.fixed<12>();
    const auto tag = outer.fixed<16>();
    const auto ciphertext = outer.bytes(max_legacy_frame);
    outer.finish();
    const auto plain = aes_gcm_open(key_, nonce, tag, ciphertext, checkpoint_aad);
    Reader reader(plain);
    if (reader.u8() != 1)
        throw DecodeError("unsupported retention checkpoint version");

    auto decode_state = [&](StateMap& state) {
        const auto count = reader.u32();
        if (count > 4'000'000)
            throw DecodeError("too many retention checkpoint objects");
        for (uint32_t i = 0; i < count; ++i) {
            ObjectId id{reader.fixed<32>()};
            ObjectState object;
            const auto adds = reader.u32();
            if (adds > 1'000'000)
                throw DecodeError("too many retention checkpoint adds");
            for (uint32_t j = 0; j < adds; ++j) {
                NodeId origin{reader.fixed<16>()};
                const auto sequence = reader.u64();
                if (origin == NodeId{} || !sequence ||
                    !object.adds.emplace(origin, sequence).second)
                    throw DecodeError("bad retention checkpoint add");
            }
            const auto removed = reader.u32();
            if (removed > 1'000'000)
                throw DecodeError("too many retention checkpoint removes");
            for (uint32_t j = 0; j < removed; ++j) {
                NodeId origin{reader.fixed<16>()};
                const auto sequence = reader.u64();
                if (origin == NodeId{} || !sequence ||
                    !object.removed.emplace(origin, sequence).second)
                    throw DecodeError("bad retention checkpoint remove");
            }
            for (auto it = object.adds.begin(); it != object.adds.end();) {
                const auto rm = object.removed.find(it->first);
                if (rm != object.removed.end() && rm->second >= it->second)
                    it = object.adds.erase(it);
                else
                    ++it;
            }
            if (!state.emplace(id, std::move(object)).second)
                throw DecodeError("duplicate retention checkpoint object");
        }
    };
    decode_state(data_);
    decode_state(control_);
    reader.finish();
}

void RetentionStore::cleanup_checkpoint_generations_locked(std::string_view keep) const {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(checkpoint_root_, error)) {
        if (error)
            break;
        if (!entry.is_directory())
            continue;
        const auto name = entry.path().filename().string();
        if (name == keep)
            continue;
        std::error_code remove_error;
        std::filesystem::remove_all(entry.path(), remove_error);
        if (remove_error)
            Log::debug("cannot remove obsolete retention checkpoint generation " +
                       entry.path().string() + ": " + remove_error.message());
    }
}

void RetentionStore::load_checkpoint_generation_locked() {
    if (!std::filesystem::exists(checkpoint_manifest_path_)) {
        if (!std::filesystem::exists(legacy_checkpoint_path_)) {
            cleanup_checkpoint_generations_locked({});
            return;
        }
        const auto size = std::filesystem::file_size(legacy_checkpoint_path_);
        if (size > max_legacy_frame + 64ULL)
            throw std::runtime_error("legacy retention checkpoint is too large");
        std::ifstream checkpoint(legacy_checkpoint_path_, std::ios::binary);
        if (!checkpoint)
            throw std::runtime_error("cannot read retention checkpoint " +
                                     legacy_checkpoint_path_.string());
        Bytes encoded(static_cast<size_t>(size));
        if (size && !checkpoint.read(reinterpret_cast<char*>(encoded.data()),
                                     static_cast<std::streamsize>(size)))
            throw std::runtime_error("cannot read retention checkpoint " +
                                     legacy_checkpoint_path_.string());
        decode_legacy_checkpoint_locked(encoded);
        cleanup_checkpoint_generations_locked({});
        return;
    }

    const auto generation = read_small_text(checkpoint_manifest_path_, 256);
    if (!valid_generation_name(generation))
        throw std::runtime_error("invalid retention checkpoint generation");
    const auto generation_path = checkpoint_root_ / generation;
    for (size_t i = 0; i < checkpoint_shards; ++i) {
        const auto shard = static_cast<uint8_t>(i);
        const auto path = generation_path / shard_name(shard);
        const auto size = std::filesystem::file_size(path);
        if (size > max_checkpoint_shard_bytes + 64ULL)
            throw std::runtime_error("retention checkpoint shard is too large: " + path.string());
        std::ifstream checkpoint(path, std::ios::binary);
        if (!checkpoint)
            throw std::runtime_error("cannot read retention checkpoint shard " + path.string());
        Bytes encoded(static_cast<size_t>(size));
        if (size && !checkpoint.read(reinterpret_cast<char*>(encoded.data()),
                                     static_cast<std::streamsize>(size)))
            throw std::runtime_error("cannot read retention checkpoint shard " + path.string());
        decode_checkpoint_shard_locked(shard, encoded);
    }
    cleanup_checkpoint_generations_locked(generation);
}

void RetentionStore::load_journal_locked() {
    if (!std::filesystem::exists(journal_path_))
        return;
    const int fd = ::open(journal_path_.c_str(), O_RDWR);
    if (fd < 0)
        throw std::runtime_error("cannot read retention journal " + journal_path_.string());
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const auto saved = errno;
        ::close(fd);
        throw std::runtime_error("cannot stat retention journal: " +
                                 std::string(std::strerror(saved)));
    }
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t offset = 0;
    uint64_t durable = 0;
    while (offset + 4 <= file_size) {
        std::array<uint8_t, 4> length_bytes{};
        if (!pread_all(fd, length_bytes, offset))
            break;
        const uint32_t length = read_be32(length_bytes.data());
        if (length > max_legacy_frame)
            break;
        const uint64_t frame_size = 4 + 12 + 16 + static_cast<uint64_t>(length);
        if (offset > std::numeric_limits<uint64_t>::max() - frame_size ||
            offset + frame_size > file_size)
            break;
        std::array<uint8_t, 12> nonce{};
        std::array<uint8_t, 16> tag{};
        Bytes ciphertext(length);
        if (!pread_all(fd, nonce, offset + 4) || !pread_all(fd, tag, offset + 16) ||
            !pread_all(fd, ciphertext, offset + 32))
            break;
        try {
            const auto plaintext = aes_gcm_open(key_, nonce, tag, ciphertext, journal_aad);
            Reader reader(plaintext);
            const auto op = reader.u8();
            const auto raw_class = reader.u8();
            if (!valid_class(raw_class))
                throw DecodeError("bad retention object class");
            const auto type = static_cast<RetentionClass>(raw_class);
            if (op == op_add) {
                RetentionDot dot;
                dot.origin.bytes = reader.fixed<16>();
                dot.sequence = reader.u64();
                const auto count = reader.u32();
                if (count > 1'000'000)
                    throw DecodeError("too many retention add objects");
                std::vector<ObjectId> ids;
                ids.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                    ids.push_back(ObjectId{reader.fixed<32>()});
                reader.finish();
                apply_add_locked(type, dot, ids);
            } else if (op == op_release) {
                const auto clocks = reader.u32();
                if (clocks > 1'000'000)
                    throw DecodeError("too many retention release clocks");
                RetentionClock observed;
                for (uint32_t i = 0; i < clocks; ++i) {
                    NodeId node{reader.fixed<16>()};
                    const auto sequence = reader.u64();
                    if (node == NodeId{} || !sequence ||
                        !observed.emplace(node, sequence).second)
                        throw DecodeError("bad retention release clock");
                }
                const auto count = reader.u32();
                if (count > 1'000'000)
                    throw DecodeError("too many retention release objects");
                std::vector<ObjectId> ids;
                ids.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                    ids.push_back(ObjectId{reader.fixed<32>()});
                reader.finish();
                (void)apply_release_locked(type, observed, ids);
            } else {
                throw DecodeError("unknown retention journal operation");
            }
        } catch (const std::exception& error) {
            Log::warn("retention journal recovered path=" + journal_path_.string() +
                      " offset=" + std::to_string(offset) + " reason=" + error.what());
            break;
        }
        offset += frame_size;
        durable = offset;
        ++journal_records_;
    }

    if (durable != file_size) {
        if (::ftruncate(fd, static_cast<off_t>(durable)) != 0) {
            const auto error = errno;
            ::close(fd);
            throw std::runtime_error("cannot truncate retention journal: " +
                                     std::string(std::strerror(error)));
        }
        fsync_checked(fd);
    }
    if (::close(fd) != 0)
        throw std::runtime_error("cannot close retention journal");
    journal_bytes_ = durable;
}

void RetentionStore::load() {
    std::lock_guard lock(mutex_);
    load_checkpoint_generation_locked();
    load_journal_locked();
}

void RetentionStore::retain(RetentionClass type, const ObjectId& id, const RetentionDot& dot) {
    retain_batch(type, std::vector<ObjectId>{id}, dot);
}

void RetentionStore::retain_batch(RetentionClass type, const std::vector<ObjectId>& input,
                                  const RetentionDot& dot) {
    if (input.empty())
        return;
    if (dot.origin == NodeId{} || !dot.sequence)
        throw std::runtime_error("invalid retention mutation dot");
    auto ids = input;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::lock_guard lock(mutex_);
    for (size_t begin = 0; begin < ids.size(); begin += retain_ids_per_frame) {
        const auto end = std::min(ids.size(), begin + retain_ids_per_frame);
        Writer writer;
        writer.u8(op_add);
        writer.u8(static_cast<uint8_t>(type));
        writer.fixed(dot.origin.bytes);
        writer.u64(dot.sequence);
        writer.u32(static_cast<uint32_t>(end - begin));
        for (size_t i = begin; i < end; ++i)
            writer.fixed(ids[i].bytes);
        append_frame_locked(writer.data());
        std::vector<ObjectId> slice(ids.begin() + static_cast<std::ptrdiff_t>(begin),
                                    ids.begin() + static_cast<std::ptrdiff_t>(end));
        apply_add_locked(type, dot, slice);
    }
}

bool RetentionStore::retained(RetentionClass type, const ObjectId& id) const {
    std::lock_guard lock(mutex_);
    const auto& state = state_for(type);
    const auto found = state.find(id);
    return found != state.end() && !found->second.adds.empty();
}

RetentionStore::Claims RetentionStore::claims(RetentionClass type, const ObjectId& id) const {
    std::lock_guard lock(mutex_);
    const auto& state = state_for(type);
    const auto found = state.find(id);
    if (found == state.end())
        return {};
    return {found->second.adds, found->second.removed};
}

std::vector<ObjectId> RetentionStore::retained_ids(RetentionClass type) const {
    std::lock_guard lock(mutex_);
    std::vector<ObjectId> ids;
    for (const auto& [id, state] : state_for(type))
        if (!state.adds.empty())
            ids.push_back(id);
    return ids;
}

std::optional<ObjectId> RetentionStore::next_retained(
    RetentionClass type, std::optional<ObjectId>& cursor, bool& complete) const {
    std::lock_guard lock(mutex_);
    const auto& state = state_for(type);
    auto it = cursor ? state.upper_bound(*cursor) : state.begin();
    while (it != state.end() && it->second.adds.empty())
        ++it;
    if (it == state.end()) {
        complete = true;
        cursor.reset();
        return {};
    }
    cursor = it->first;
    complete = false;
    return it->first;
}

size_t RetentionStore::release_unreferenced(RetentionClass type,
                                            const std::vector<ObjectId>& live,
                                            const RetentionClock& observed,
                                            size_t operation_budget) {
    if (!operation_budget || observed.empty())
        return 0;

    std::lock_guard lock(mutex_);
    auto& state = state_for(type);
    auto& cursor = cursor_for(type);
    if (state.empty()) {
        cursor.reset();
        return 0;
    }

    auto it = cursor ? state.upper_bound(*cursor) : state.begin();
    if (it == state.end())
        it = state.begin();
    const auto start = it;
    bool wrapped = false;
    std::vector<ObjectId> candidates;
    candidates.reserve(operation_budget);

    while (it != state.end() && candidates.size() < operation_budget) {
        if (!std::binary_search(live.begin(), live.end(), it->first) && !it->second.adds.empty())
            candidates.push_back(it->first);
        cursor = it->first;
        ++it;
        if (it == state.end() && !wrapped) {
            it = state.begin();
            wrapped = true;
        }
        if (wrapped && it == start)
            break;
    }
    if (candidates.empty())
        return 0;

    RetentionClock encoded_clock;
    for (const auto& [node, sequence] : observed)
        if (node != NodeId{} && sequence)
            encoded_clock[node] = sequence;
    if (encoded_clock.empty())
        return 0;

    Writer writer;
    writer.u8(op_release);
    writer.u8(static_cast<uint8_t>(type));
    writer.u32(static_cast<uint32_t>(encoded_clock.size()));
    for (const auto& [node, sequence] : encoded_clock) {
        writer.fixed(node.bytes);
        writer.u64(sequence);
    }
    writer.u32(static_cast<uint32_t>(candidates.size()));
    for (const auto& id : candidates)
        writer.fixed(id.bytes);
    append_frame_locked(writer.data());
    return apply_release_locked(type, encoded_clock, candidates);
}

size_t RetentionStore::claim_objects(RetentionClass type) const {
    std::lock_guard lock(mutex_);
    size_t count = 0;
    for (const auto& [_, state] : state_for(type))
        if (!state.adds.empty())
            ++count;
    return count;
}

bool RetentionStore::compact_if_needed(size_t record_threshold) {
    std::lock_guard lock(mutex_);
    if (journal_records_ < record_threshold && journal_bytes_ < journal_compact_bytes)
        return false;

    const auto generation = "gen-" + std::to_string(unix_ms()) + "-" +
                            std::to_string(getpid()) + "-" +
                            std::to_string(journal_records_);
    const auto generation_path = checkpoint_root_ / generation;
    std::filesystem::create_directories(generation_path);
    for (size_t i = 0; i < checkpoint_shards; ++i) {
        const auto shard = static_cast<uint8_t>(i);
        const auto encoded = encode_checkpoint_shard_locked(shard);
        durable_replace_file(
            generation_path / shard_name(shard),
            std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
    }
    // Persist the generation directory entry before publishing the tiny manifest
    // which makes the complete set authoritative.
    sync_directory(checkpoint_root_);
    durable_replace_file(checkpoint_manifest_path_, generation + "\n");

    // The checkpoint is already durable. If this replacement fails or the
    // process dies before it, replaying the old journal is idempotent.
    durable_replace_file(journal_path_, {});
    journal_records_ = 0;
    journal_bytes_ = 0;

    std::error_code remove_error;
    std::filesystem::remove(legacy_checkpoint_path_, remove_error);
    cleanup_checkpoint_generations_locked(generation);
    return true;
}

size_t RetentionStore::prune_unclaimed(
    RetentionClass type, const std::function<bool(const ObjectId&)>& exists,
    size_t operation_budget) {
    if (!operation_budget || !exists)
        return 0;
    std::lock_guard lock(mutex_);
    auto& state = state_for(type);
    auto& cursor = prune_cursor_for(type);
    if (state.empty()) {
        cursor.reset();
        return 0;
    }

    auto it = cursor ? state.upper_bound(*cursor) : state.begin();
    if (it == state.end())
        it = state.begin();
    const auto start_id = it->first;
    bool wrapped = false;
    size_t examined = 0;
    size_t removed = 0;
    while (!state.empty() && examined < operation_budget) {
        if (it == state.end()) {
            if (wrapped)
                break;
            it = state.begin();
            wrapped = true;
            if (it == state.end())
                break;
        }
        if (wrapped && it->first == start_id)
            break;

        ++examined;
        const auto id = it->first;
        const bool erase = it->second.adds.empty() && !exists(id);
        if (erase) {
            it = state.erase(it);
            ++removed;
        } else {
            ++it;
        }
        cursor = id;
    }
    if (state.empty())
        cursor.reset();
    return removed;
}

} // namespace macha
