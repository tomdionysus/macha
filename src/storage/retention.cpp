// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/retention.hpp"

#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'R', 'T', 'J', '1'};
constexpr std::array<uint8_t, 8> checkpoint_aad{'M', 'A', 'C', 'H', 'R', 'T', 'S', '1'};
constexpr std::array<uint8_t, 8> legacy_checkpoint_magic{'M', 'R', 'T', 'S', '0', '0', '0', '1'};
constexpr std::array<uint8_t, 8> shard_checkpoint_magic{'M', 'R', 'T', 'S', '0', '0', '0', '2'};
constexpr uint8_t op_add = 1;
constexpr uint8_t op_release = 2;
constexpr uint8_t op_prune = 3;
constexpr uint32_t max_legacy_frame = 64U * 1024U * 1024U;
constexpr uint32_t max_journal_frame = 4U * 1024U * 1024U;
constexpr uint64_t journal_compact_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t retain_ids_per_frame = 65536;
constexpr uint64_t max_checkpoint_shard_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t checkpoint_shards = 256;
// A checkpoint is due once the tries hold this many changed objects unsaved,
// or the journal this many bytes: what a start replays stays small.
constexpr uint64_t checkpoint_changed = 8192;
constexpr uint64_t checkpoint_journal_bytes = 4ULL * 1024 * 1024;
constexpr size_t scan_page = 256;

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

// One object's observed-remove state, as its trie record holds it.
struct ObjectState {
    std::map<NodeId, uint64_t> adds;
    std::map<NodeId, uint64_t> removed;
    bool empty() const noexcept { return adds.empty() && removed.empty(); }
};
using StateMap = std::map<ObjectId, ObjectState>;

// u16 n | (origin, u64 sequence) x n, for the adds and then the removes.
Bytes encode_state(const ObjectState& state) {
    Writer writer;
    for (const auto* side : {&state.adds, &state.removed}) {
        if (side->size() > UINT16_MAX)
            throw std::runtime_error("too many retention clocks for one object");
        writer.u16(static_cast<uint16_t>(side->size()));
        for (const auto& [origin, sequence] : *side) {
            writer.fixed(origin.bytes);
            writer.u64(sequence);
        }
    }
    return writer.take();
}

ObjectState decode_state(std::span<const uint8_t> bytes) {
    ObjectState state;
    Reader reader(bytes);
    for (auto* side : {&state.adds, &state.removed}) {
        const auto count = reader.u16();
        for (uint16_t i = 0; i < count; ++i) {
            NodeId origin{reader.fixed<16>()};
            const auto sequence = reader.u64();
            if (origin == NodeId{} || !sequence || !side->emplace(origin, sequence).second)
                throw DecodeError("bad retention record clock");
        }
    }
    reader.finish();
    return state;
}

bool claimed(std::span<const uint8_t> record) {
    // The adds' count leads the record.
    return record.size() >= 2 && (record[0] != 0 || record[1] != 0);
}

ObjectState read_clocks(Reader& reader, uint32_t limit) {
    ObjectState object;
    const auto adds = reader.u32();
    if (adds > limit)
        throw DecodeError("too many retention checkpoint adds");
    for (uint32_t j = 0; j < adds; ++j) {
        NodeId origin{reader.fixed<16>()};
        const auto sequence = reader.u64();
        if (origin == NodeId{} || !sequence || !object.adds.emplace(origin, sequence).second)
            throw DecodeError("bad retention checkpoint add");
    }
    const auto removed = reader.u32();
    if (removed > limit)
        throw DecodeError("too many retention checkpoint removes");
    for (uint32_t j = 0; j < removed; ++j) {
        NodeId origin{reader.fixed<16>()};
        const auto sequence = reader.u64();
        if (origin == NodeId{} || !sequence || !object.removed.emplace(origin, sequence).second)
            throw DecodeError("bad retention checkpoint remove");
    }
    for (auto it = object.adds.begin(); it != object.adds.end();) {
        const auto rm = object.removed.find(it->first);
        if (rm != object.removed.end() && rm->second >= it->second)
            it = object.adds.erase(it);
        else
            ++it;
    }
    return object;
}

void decode_checkpoint_shard(const std::array<uint8_t, 32>& key, uint8_t shard,
                             std::span<const uint8_t> encoded, StateMap& data,
                             StateMap& control) {
    Reader outer(encoded);
    if (outer.fixed<8>() != shard_checkpoint_magic)
        throw DecodeError("bad retention checkpoint shard magic");
    const auto nonce = outer.fixed<12>();
    const auto tag = outer.fixed<16>();
    const auto ciphertext = outer.bytes(max_checkpoint_shard_bytes);
    outer.finish();
    const auto plain = aes_gcm_open(key, nonce, tag, ciphertext, checkpoint_aad);
    Reader reader(plain);
    if (reader.u8() != 2 || reader.u8() != shard)
        throw DecodeError("bad retention checkpoint shard identity");
    for (auto* state : {&data, &control}) {
        const auto count = reader.u32();
        if (count > 1'500'000)
            throw DecodeError("too many retention checkpoint shard objects");
        for (uint32_t i = 0; i < count; ++i) {
            ObjectId id{reader.fixed<32>()};
            if (id.bytes[0] != shard)
                throw DecodeError("retention object stored in wrong checkpoint shard");
            if (!state->emplace(id, read_clocks(reader, 65536)).second)
                throw DecodeError("duplicate retention checkpoint object");
        }
    }
    reader.finish();
}

void decode_monolithic_checkpoint(const std::array<uint8_t, 32>& key,
                                  std::span<const uint8_t> encoded, StateMap& data,
                                  StateMap& control) {
    Reader outer(encoded);
    if (outer.fixed<8>() != legacy_checkpoint_magic)
        throw DecodeError("bad retention checkpoint magic");
    const auto nonce = outer.fixed<12>();
    const auto tag = outer.fixed<16>();
    const auto ciphertext = outer.bytes(max_legacy_frame);
    outer.finish();
    const auto plain = aes_gcm_open(key, nonce, tag, ciphertext, checkpoint_aad);
    Reader reader(plain);
    if (reader.u8() != 1)
        throw DecodeError("unsupported retention checkpoint version");
    for (auto* state : {&data, &control}) {
        const auto count = reader.u32();
        if (count > 4'000'000)
            throw DecodeError("too many retention checkpoint objects");
        for (uint32_t i = 0; i < count; ++i) {
            ObjectId id{reader.fixed<32>()};
            if (!state->emplace(id, read_clocks(reader, 1'000'000)).second)
                throw DecodeError("duplicate retention checkpoint object");
        }
    }
    reader.finish();
}

Bytes read_file(const std::filesystem::path& path, uint64_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (size > limit)
        throw std::runtime_error("retention checkpoint is too large: " + path.string());
    std::ifstream input(path, std::ios::binary);
    Bytes bytes(static_cast<size_t>(size));
    if (!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(size))))
        throw std::runtime_error("cannot read retention checkpoint " + path.string());
    return bytes;
}

// The claims the checkpoint files before the tries hold: the sharded
// generation claims.current names, or the monolithic claims.meta.
void load_checkpoint_files(const std::filesystem::path& root, const std::array<uint8_t, 32>& key,
                           StateMap& data, StateMap& control) {
    const auto manifest = root / "claims.current";
    if (!std::filesystem::exists(manifest)) {
        const auto monolithic = root / "claims.meta";
        if (std::filesystem::exists(monolithic))
            decode_monolithic_checkpoint(key, read_file(monolithic, max_legacy_frame + 64ULL),
                                         data, control);
        return;
    }
    const auto generation = read_small_text(manifest, 256);
    if (!valid_generation_name(generation))
        throw std::runtime_error("invalid retention checkpoint generation");
    const auto generation_path = root / "checkpoints" / generation;
    for (size_t i = 0; i < checkpoint_shards; ++i) {
        const auto shard = static_cast<uint8_t>(i);
        decode_checkpoint_shard(
            key, shard,
            read_file(generation_path / shard_name(shard), max_checkpoint_shard_bytes + 64ULL),
            data, control);
    }
}

void remove_checkpoint_files(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove(root / "claims.current", error);
    std::filesystem::remove(root / "claims.meta", error);
    std::filesystem::remove_all(root / "checkpoints", error);
    if (error)
        Log::debug("cannot remove retention checkpoint files: " + error.message());
}

std::vector<ObjectId> sorted_unique(std::vector<ObjectId> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace

RetentionStore::RetentionStore(std::filesystem::path state_path, std::array<uint8_t, 32> key,
                               size_t cache_bytes)
    : root_(state_path / "retention"), key_(key),
      journal_(state_path / "retention" / "claims.log", key, journal_aad, max_journal_frame) {
    std::filesystem::create_directories(root_);
    load(cache_bytes);
}

RetentionStore::ClassLedger& RetentionStore::class_for(RetentionClass type) {
    return type == RetentionClass::data ? data_ : control_;
}

const RetentionStore::ClassLedger& RetentionStore::class_for(RetentionClass type) const {
    return type == RetentionClass::data ? data_ : control_;
}

void RetentionStore::apply_add_locked(RetentionClass type, const RetentionDot& dot,
                                      std::vector<ObjectId> ids) {
    if (dot.origin == NodeId{} || !dot.sequence)
        throw std::runtime_error("invalid retention mutation dot");
    auto& trie = *class_for(type).trie;
    std::vector<ObjectTrie::Change> changes;
    for (const auto& id : sorted_unique(std::move(ids))) {
        const auto record = trie.get(id);
        auto object = record ? decode_state(*record) : ObjectState{};
        const auto removed = object.removed.find(dot.origin);
        if (removed != object.removed.end() && removed->second >= dot.sequence)
            continue;
        auto& current = object.adds[dot.origin];
        if (current >= dot.sequence)
            continue;
        current = dot.sequence;
        changes.push_back({id, encode_state(object)});
    }
    changed_ += changes.size();
    trie.install(changes);
}

size_t RetentionStore::apply_release_locked(RetentionClass type, const RetentionClock& observed,
                                            std::vector<ObjectId> ids) {
    auto& trie = *class_for(type).trie;
    std::vector<ObjectTrie::Change> changes;
    for (const auto& id : sorted_unique(std::move(ids))) {
        const auto record = trie.get(id);
        if (!record)
            continue;
        auto object = decode_state(*record);
        bool object_changed = false;
        for (const auto& [origin, sequence] : observed) {
            if (!sequence)
                continue;
            auto& removed = object.removed[origin];
            if (removed < sequence) {
                removed = sequence;
                object_changed = true;
            }
            auto add = object.adds.find(origin);
            if (add != object.adds.end() && add->second <= sequence) {
                object.adds.erase(add);
                object_changed = true;
            }
        }
        if (object_changed)
            changes.push_back({id, encode_state(object)});
    }
    changed_ += changes.size();
    trie.install(changes);
    return changes.size();
}

size_t RetentionStore::apply_prune_locked(RetentionClass type, std::vector<ObjectId> ids) {
    auto& trie = *class_for(type).trie;
    std::vector<ObjectTrie::Change> changes;
    for (const auto& id : sorted_unique(std::move(ids))) {
        const auto record = trie.get(id);
        if (record && !claimed(*record))
            changes.push_back({id, std::nullopt});
    }
    changed_ += changes.size();
    trie.install(changes);
    return changes.size();
}

void RetentionStore::checkpoint_locked() {
    data_.trie->checkpoint();
    control_.trie->checkpoint();
    // The tries are durable; replaying the old journal over them would be
    // idempotent had this not happened.
    journal_.reset();
    changed_ = 0;
    // The first checkpoint supersedes the files the tries were migrated from.
    remove_checkpoint_files(root_);
}

void RetentionStore::checkpoint_if_due_locked() {
    if (changed_ >= checkpoint_changed || journal_.bytes() >= checkpoint_journal_bytes)
        checkpoint_locked();
}

void RetentionStore::scan_locked(
    const ClassLedger& ledger, std::optional<ObjectId>& cursor, size_t limit,
    const std::function<bool(const ObjectId&, std::span<const uint8_t>)>& visit) const {
    std::optional<ObjectId> start;
    std::optional<ObjectId> after = cursor;
    bool wrapped = false;
    size_t visited = 0;
    while (visited < limit) {
        const auto page = ledger.trie->next(after, std::min(scan_page, limit - visited));
        if (page.empty()) {
            if (wrapped || !after)
                return;
            wrapped = true;
            after.reset();
            continue;
        }
        for (const auto& [id, record] : page) {
            if (wrapped && start && !(id < *start))
                return;
            if (!start)
                start = id;
            ++visited;
            cursor = id;
            after = id;
            if (!visit(id, record))
                return;
        }
    }
}

void RetentionStore::migrate_locked() {
    // The claims the files before the tries held, and the journal written
    // since them, which the normal replay then applies over the tries.
    StateMap data;
    StateMap control;
    load_checkpoint_files(root_, key_, data, control);
    for (auto [state, ledger] : {std::pair{&data, &data_}, std::pair{&control, &control_}}) {
        std::vector<ObjectTrie::Record> records;
        records.reserve(state->size());
        for (const auto& [id, object] : *state)
            if (!object.empty())
                records.emplace_back(id, encode_state(object));
        ledger->trie->replace_all(std::move(records));
    }
    durable_replace_file(root_ / "ledger", "1\n");
    Log::info("retention claims moved into the ledger data=" + std::to_string(data.size()) +
              " control=" + std::to_string(control.size()));
}

void RetentionStore::load_journal_locked() {
    (void)journal_.replay(
        [&](std::span<const uint8_t> plaintext) MACHA_REQUIRES(mutex_) {
            Reader reader(plaintext);
            const auto op = reader.u8();
            const auto raw_class = reader.u8();
            if (!valid_class(raw_class))
                throw DecodeError("bad retention object class");
            const auto type = static_cast<RetentionClass>(raw_class);
            const auto read_ids = [&](const char* what) {
                const auto count = reader.u32();
                if (count > 1'000'000)
                    throw DecodeError(std::string("too many retention ") + what + " objects");
                std::vector<ObjectId> ids;
                ids.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                    ids.push_back(ObjectId{reader.fixed<32>()});
                reader.finish();
                return ids;
            };
            if (op == op_add) {
                RetentionDot dot;
                dot.origin.bytes = reader.fixed<16>();
                dot.sequence = reader.u64();
                apply_add_locked(type, dot, read_ids("add"));
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
                (void)apply_release_locked(type, observed, read_ids("release"));
            } else if (op == op_prune) {
                (void)apply_prune_locked(type, read_ids("prune"));
            } else {
                throw DecodeError("unknown retention journal operation");
            }
        },
        max_legacy_frame, "retention journal");
}

void RetentionStore::load(size_t cache_bytes) {
    Lock lock(mutex_);
    ObjectTrie::Options data_options;
    data_options.cache_bytes = cache_bytes - cache_bytes / 4;
    ObjectTrie::Options control_options;
    control_options.cache_bytes = cache_bytes / 4;
    data_.trie = std::make_unique<ObjectTrie>(root_ / "ledger-data", key_, data_options);
    control_.trie = std::make_unique<ObjectTrie>(root_ / "ledger-control", key_, control_options);
    if (!std::filesystem::exists(root_ / "ledger"))
        migrate_locked();
    load_journal_locked();
    checkpoint_if_due_locked();
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
    const auto ids = sorted_unique(input);

    Lock lock(mutex_);
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
        journal_.append(writer.data());
        apply_add_locked(type, dot,
                         std::vector<ObjectId>(ids.begin() + static_cast<std::ptrdiff_t>(begin),
                                               ids.begin() + static_cast<std::ptrdiff_t>(end)));
        checkpoint_if_due_locked();
    }
}

bool RetentionStore::retained(RetentionClass type, const ObjectId& id) const {
    Lock lock(mutex_);
    const auto record = class_for(type).trie->get(id);
    return record && claimed(*record);
}

RetentionStore::Claims RetentionStore::claims(RetentionClass type, const ObjectId& id) const {
    Lock lock(mutex_);
    const auto record = class_for(type).trie->get(id);
    if (!record)
        return {};
    auto object = decode_state(*record);
    return {std::move(object.adds), std::move(object.removed)};
}

std::vector<ObjectId> RetentionStore::retained_ids(RetentionClass type) const {
    Lock lock(mutex_);
    std::vector<ObjectId> ids;
    std::optional<ObjectId> after;
    while (true) {
        const auto page = class_for(type).trie->next(after, scan_page);
        for (const auto& [id, record] : page)
            if (claimed(record))
                ids.push_back(id);
        if (page.size() < scan_page)
            return ids;
        after = page.back().first;
    }
}

std::optional<ObjectId> RetentionStore::next_retained(
    RetentionClass type, std::optional<ObjectId>& cursor, bool& complete) const {
    Lock lock(mutex_);
    while (true) {
        const auto page = class_for(type).trie->next(cursor, scan_page);
        for (const auto& [id, record] : page) {
            cursor = id;
            if (claimed(record)) {
                complete = false;
                return id;
            }
        }
        if (page.size() < scan_page) {
            complete = true;
            cursor.reset();
            return {};
        }
    }
}

size_t RetentionStore::release_unreferenced(RetentionClass type, const IdLookup& live,
                                            const RetentionClock& observed,
                                            size_t operation_budget) {
    if (!operation_budget || observed.empty())
        return 0;
    RetentionClock encoded_clock;
    for (const auto& [node, sequence] : observed)
        if (node != NodeId{} && sequence)
            encoded_clock[node] = sequence;
    if (encoded_clock.empty())
        return 0;

    Lock lock(mutex_);
    auto& ledger = class_for(type);
    if (ledger.trie->size() == 0) {
        ledger.release_after.reset();
        return 0;
    }
    // A bounded stretch of claims from where the last call stopped: commits
    // take this lock for their own claims, and nearly every claim is live,
    // so a call that looked at them all would hold it for the whole store.
    std::vector<ObjectId> candidates;
    candidates.reserve(operation_budget);
    scan_locked(ledger, ledger.release_after, std::max<size_t>(operation_budget * 128, 8192),
                [&](const ObjectId& id, std::span<const uint8_t> record) {
                    if (claimed(record) && !live.contains(id))
                        candidates.push_back(id);
                    return candidates.size() < operation_budget;
                });
    if (candidates.empty())
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
    journal_.append(writer.data());
    const auto released = apply_release_locked(type, encoded_clock, std::move(candidates));
    checkpoint_if_due_locked();
    return released;
}

size_t RetentionStore::claim_objects(RetentionClass type) const {
    return retained_ids(type).size();
}

bool RetentionStore::compact_if_needed(size_t record_threshold) {
    Lock lock(mutex_);
    if (journal_.frames() < record_threshold && journal_.bytes() < journal_compact_bytes)
        return false;
    checkpoint_locked();
    return true;
}

size_t RetentionStore::prune_unclaimed(
    RetentionClass type, const std::function<bool(const ObjectId&)>& exists,
    size_t operation_budget) {
    if (!operation_budget || !exists)
        return 0;
    // Three steps, so `exists` (which may read a device) runs without mutex_:
    // pick the unclaimed records under the lock, ask the store without it,
    // then journal and erase under the lock only what is still unclaimed.
    std::vector<ObjectId> unclaimed;
    {
        Lock lock(mutex_);
        auto& ledger = class_for(type);
        if (ledger.trie->size() == 0) {
            ledger.prune_after.reset();
            return 0;
        }
        scan_locked(ledger, ledger.prune_after, operation_budget,
                    [&](const ObjectId& id, std::span<const uint8_t> record) {
                        if (!claimed(record))
                            unclaimed.push_back(id);
                        return true;
                    });
    }

    std::erase_if(unclaimed, [&](const ObjectId& id) { return exists(id); });
    if (unclaimed.empty())
        return 0;

    Lock lock(mutex_);
    Writer writer;
    writer.u8(op_prune);
    writer.u8(static_cast<uint8_t>(type));
    writer.u32(static_cast<uint32_t>(unclaimed.size()));
    for (const auto& id : unclaimed)
        writer.fixed(id.bytes);
    journal_.append(writer.data());
    const auto removed = apply_prune_locked(type, std::move(unclaimed));
    if (class_for(type).trie->size() == 0)
        class_for(type).prune_after.reset();
    checkpoint_if_due_locked();
    return removed;
}

} // namespace macha
