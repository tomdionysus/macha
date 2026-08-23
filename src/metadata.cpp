// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata.hpp"
#include "codec.hpp"
#include "log.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <unistd.h>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> SM5{'D', 'H', 'T', 'M', 'E', 'T', 'A', '5'},
    SM6{'D', 'H', 'T', 'M', 'E', 'T', 'A', '6'},
    SM7{'D', 'H', 'T', 'M', 'E', 'T', 'A', '7'},
    SM8{'D', 'H', 'T', 'M', 'E', 'T', 'A', '8'},
    DM{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'},
    MJ{'D', 'H', 'T', 'M', 'J', 'N', 'L', '1'};
constexpr uint8_t JOURNAL_PREPARE_FULL = 1, JOURNAL_PREPARE_DELTA = 2,
                  JOURNAL_SEED_FULL = 3, JOURNAL_COMMIT = 4;
void entry(Writer& w, const FsEntry& e) {
    w.u8((uint8_t)e.type);
    w.u32(e.mode);
    w.u32(e.uid);
    w.u32(e.gid);
    w.u64(e.size);
    w.i64(e.ctime_ns);
    w.i64(e.mtime_ns);
    w.u64(e.version);
    w.u32(e.extents.size());
    for (auto& x : e.extents) {
        w.u64(x.offset);
        w.u64(x.length);
        w.u8(x.hole);
        w.fixed(x.id.bytes);
    }
}
FsEntry entry(Reader& r) {
    FsEntry e;
    auto t = r.u8();
    if (t < 1 || t > 2)
        throw DecodeError("bad entry type");
    e.type = (EntryType)t;
    e.mode = r.u32();
    e.uid = r.u32();
    e.gid = r.u32();
    e.size = r.u64();
    e.ctime_ns = r.i64();
    e.mtime_ns = r.i64();
    e.version = r.u64();
    auto n = r.u32();
    if (n > 10000000)
        throw DecodeError("too many extents");
    for (uint32_t i = 0; i < n; ++i) {
        ExtentRef x;
        x.offset = r.u64();
        x.length = r.u64();
        x.hole = r.u8();
        x.id.bytes = r.fixed<32>();
        e.extents.push_back(x);
    }
    return e;
}
Bytes encode_snapshot_v7(const MetadataSnapshot& s) {
    // Exact pre-0.10.0 snapshot representation. This is used only while
    // replaying DLT1 records from an existing metadata journal: the journal
    // stores the successor hash, so reconstructing the historical SM7 bytes
    // is part of on-disk compatibility. New snapshots are always SM8.
    Writer w;
    w.raw(SM7);
    w.u32(s.metadata_voters.size());
    for (const auto& v : s.metadata_voters)
        w.fixed(v.bytes);
    w.u32(s.data_replication);
    w.u64(s.extent_size);
    w.u32(s.mutation_sequences.size());
    for (const auto& [node, sequence] : s.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(s.entries.size());
    for (const auto& [path, value] : s.entries) {
        w.string(path);
        entry(w, value);
    }
    w.u8(s.catalogue_root.has_value());
    if (s.catalogue_root)
        w.fixed(s.catalogue_root->bytes);
    w.u32(s.garbage.size());
    for (const auto& garbage : s.garbage)
        w.fixed(garbage.id.bytes);
    return w.take();
}

bool metadata_delta_v1(std::span<const uint8_t> data) {
    static constexpr std::array<uint8_t, 8> magic{'D', 'H', 'T', 'M', 'D', 'L', 'T', '1'};
    return data.size() >= magic.size() && std::equal(magic.begin(), magic.end(), data.begin());
}

void writefile(const std::filesystem::path& p, std::span<const uint8_t> d) {
    auto t = p.string() + ".tmp." + std::to_string(getpid());
    int f = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (f < 0)
        throw std::runtime_error(strerror(errno));
    size_t q = 0;
    while (q < d.size()) {
        auto n = write(f, d.data() + q, d.size() - q);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(f);
            throw std::runtime_error(strerror(errno));
        }
        q += n;
    }
    fsync(f);
    close(f);
    if (rename(t.c_str(), p.c_str()))
        throw std::runtime_error(strerror(errno));
    int dirfd = open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        (void)fsync(dirfd);
        close(dirfd);
    }
}
} // namespace
Bytes encode_snapshot(const MetadataSnapshot& s) {
    Writer w;
    w.raw(SM8);
    w.u32(s.metadata_voters.size());
    for (auto& v : s.metadata_voters)
        w.fixed(v.bytes);
    w.u32(s.data_replication);
    w.u64(s.extent_size);
    w.u32(s.mutation_sequences.size());
    for (const auto& [node, sequence] : s.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(s.entries.size());
    for (auto& [p, e] : s.entries) {
        w.string(p);
        entry(w, e);
    }
    w.u8(s.catalogue_root.has_value());
    if (s.catalogue_root)
        w.fixed(s.catalogue_root->bytes);
    w.u32(s.garbage.size());
    for (const auto& garbage : s.garbage) {
        w.fixed(garbage.id.bytes);
        w.i64(garbage.retired_at_ns);
        w.fixed(garbage.retirement_id.bytes);
    }
    return w.take();
}
MetadataSnapshot decode_snapshot(std::span<const uint8_t> d) {
    Reader r(d);
    auto m = r.raw(8);
    const bool v5 = std::equal(m.begin(), m.end(), SM5.begin());
    const bool v6 = std::equal(m.begin(), m.end(), SM6.begin());
    const bool v7 = std::equal(m.begin(), m.end(), SM7.begin());
    const bool v8 = std::equal(m.begin(), m.end(), SM8.begin());
    if (!v5 && !v6 && !v7 && !v8)
        throw DecodeError("bad snapshot");
    auto nv = r.u32();
    if (nv > 1024)
        throw DecodeError("too many metadata voters");
    MetadataSnapshot s;
    for (uint32_t i = 0; i < nv; ++i) {
        NodeId v{r.fixed<16>()};
        s.metadata_voters.push_back(v);
    }
    s.data_replication = r.u32();
    s.extent_size = r.u64();
    if (v7 || v8) {
        auto mutations = r.u32();
        if (mutations > 65536)
            throw DecodeError("too many metadata mutation origins");
        for (uint32_t i = 0; i < mutations; ++i) {
            NodeId node{r.fixed<16>()};
            auto sequence = r.u64();
            if (!sequence || !s.mutation_sequences.emplace(node, sequence).second)
                throw DecodeError("bad metadata mutation sequence");
        }
    }
    auto n = r.u32();
    if (n > 5000000)
        throw DecodeError("too many filesystem entries");
    for (uint32_t i = 0; i < n; ++i) {
        auto p = normalize_path(r.string());
        if (!s.entries.emplace(p, entry(r)).second)
            throw DecodeError("duplicate path");
    }
    if ((v6 || v7 || v8) && r.u8()) {
        ObjectId root;
        root.bytes = r.fixed<32>();
        s.catalogue_root = root;
    }
    auto garbage_count = r.u32();
    if (garbage_count > 10000000)
        throw DecodeError("too many garbage records");
    s.garbage.reserve(garbage_count);
    for (uint32_t i = 0; i < garbage_count; ++i) {
        GarbageRef garbage;
        garbage.id.bytes = r.fixed<32>();
        if (v8) {
            garbage.retired_at_ns = r.i64();
            if (garbage.retired_at_ns < 0)
                throw DecodeError("bad garbage retirement time");
            garbage.retirement_id.bytes = r.fixed<16>();
        }
        s.garbage.push_back(garbage);
    }
    r.finish();
    auto x = s.entries.find("/");
    if (x == s.entries.end() || x->second.type != EntryType::directory)
        throw DecodeError("missing root");
    return s;
}

Bytes encode_metadata_delta(const MetadataDelta& delta) {
    static constexpr std::array<uint8_t, 8> magic{'D', 'H', 'T', 'M', 'D', 'L', 'T', '2'};
    Writer w;
    w.raw(magic);
    w.u32(delta.mutation_sequences.size());
    for (const auto& [node, sequence] : delta.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(delta.erase_entries.size());
    for (const auto& path : delta.erase_entries)
        w.string(path);
    w.u32(delta.upsert_entries.size());
    for (const auto& [path, value] : delta.upsert_entries) {
        w.string(path);
        entry(w, value);
    }
    w.u8(static_cast<uint8_t>(delta.catalogue));
    if (delta.catalogue == CatalogueDelta::set) {
        if (!delta.catalogue_root)
            throw std::runtime_error("metadata delta missing catalogue root");
        w.fixed(delta.catalogue_root->bytes);
    }
    w.u32(delta.erase_garbage.size());
    for (const auto& id : delta.erase_garbage)
        w.fixed(id.bytes);
    w.u32(delta.upsert_garbage.size());
    for (const auto& garbage : delta.upsert_garbage) {
        w.fixed(garbage.id.bytes);
        w.i64(garbage.retired_at_ns);
        w.fixed(garbage.retirement_id.bytes);
    }
    return w.take();
}

MetadataDelta decode_metadata_delta(std::span<const uint8_t> data) {
    static constexpr std::array<uint8_t, 8> magic_v1{'D', 'H', 'T', 'M', 'D', 'L', 'T', '1'};
    static constexpr std::array<uint8_t, 8> magic_v2{'D', 'H', 'T', 'M', 'D', 'L', 'T', '2'};
    Reader r(data);
    auto got = r.raw(magic_v1.size());
    const bool v1 = std::equal(got.begin(), got.end(), magic_v1.begin());
    const bool v2 = std::equal(got.begin(), got.end(), magic_v2.begin());
    if (!v1 && !v2)
        throw DecodeError("bad metadata delta");
    MetadataDelta delta;
    auto sequences = r.u32();
    if (sequences > 65536)
        throw DecodeError("too many metadata delta sequences");
    for (uint32_t i = 0; i < sequences; ++i) {
        NodeId node{r.fixed<16>()};
        auto sequence = r.u64();
        if (!sequence || !delta.mutation_sequences.emplace(node, sequence).second)
            throw DecodeError("bad metadata delta sequence");
    }
    auto erased = r.u32();
    if (erased > 5000000)
        throw DecodeError("too many metadata delta erases");
    delta.erase_entries.reserve(erased);
    for (uint32_t i = 0; i < erased; ++i)
        delta.erase_entries.push_back(normalize_path(r.string()));
    if (!std::is_sorted(delta.erase_entries.begin(), delta.erase_entries.end()) ||
        std::adjacent_find(delta.erase_entries.begin(), delta.erase_entries.end()) !=
            delta.erase_entries.end())
        throw DecodeError("metadata delta erases not canonical");
    auto upserts = r.u32();
    if (upserts > 5000000)
        throw DecodeError("too many metadata delta upserts");
    for (uint32_t i = 0; i < upserts; ++i) {
        auto path = normalize_path(r.string());
        if (!delta.upsert_entries.emplace(path, entry(r)).second)
            throw DecodeError("duplicate metadata delta path");
    }
    auto catalogue = r.u8();
    if (catalogue > static_cast<uint8_t>(CatalogueDelta::set))
        throw DecodeError("bad metadata catalogue delta");
    delta.catalogue = static_cast<CatalogueDelta>(catalogue);
    if (delta.catalogue == CatalogueDelta::set) {
        ObjectId id;
        id.bytes = r.fixed<32>();
        delta.catalogue_root = id;
    }

    if (v1) {
        // DLT1 is retained only for replaying metadata journals written by
        // pre-0.10.0 nodes. New network mutations are always DLT2.
        auto garbage = r.u32();
        if (garbage > 10000000)
            throw DecodeError("too much metadata delta garbage");
        delta.upsert_garbage.reserve(garbage);
        for (uint32_t i = 0; i < garbage; ++i) {
            GarbageRef value;
            value.id.bytes = r.fixed<32>();
            delta.upsert_garbage.push_back(value);
        }
    } else {
        auto erased_garbage = r.u32();
        if (erased_garbage > 10000000)
            throw DecodeError("too many metadata delta garbage erases");
        delta.erase_garbage.reserve(erased_garbage);
        std::set<ObjectId> erased_ids;
        for (uint32_t i = 0; i < erased_garbage; ++i) {
            ObjectId id;
            id.bytes = r.fixed<32>();
            if (!erased_ids.insert(id).second)
                throw DecodeError("duplicate metadata delta garbage erase");
            delta.erase_garbage.push_back(id);
        }
        auto garbage = r.u32();
        if (garbage > 10000000)
            throw DecodeError("too many metadata delta garbage upserts");
        delta.upsert_garbage.reserve(garbage);
        std::set<ObjectId> upsert_ids;
        for (uint32_t i = 0; i < garbage; ++i) {
            GarbageRef value;
            value.id.bytes = r.fixed<32>();
            value.retired_at_ns = r.i64();
            value.retirement_id.bytes = r.fixed<16>();
            if (value.retired_at_ns < 0 || !upsert_ids.insert(value.id).second)
                throw DecodeError("bad metadata delta garbage upsert");
            delta.upsert_garbage.push_back(value);
        }
    }
    r.finish();
    return delta;
}

std::optional<MetadataDelta> metadata_delta(const MetadataSnapshot& before,
                                            const MetadataSnapshot& after) {
    if (before.metadata_voters != after.metadata_voters ||
        before.data_replication != after.data_replication ||
        before.extent_size != after.extent_size)
        return {};

    MetadataDelta delta;
    for (const auto& [node, sequence] : before.mutation_sequences) {
        auto it = after.mutation_sequences.find(node);
        if (it == after.mutation_sequences.end() || it->second < sequence)
            return {};
    }
    for (const auto& [node, sequence] : after.mutation_sequences) {
        auto it = before.mutation_sequences.find(node);
        if (it == before.mutation_sequences.end() || it->second != sequence)
            delta.mutation_sequences.emplace(node, sequence);
    }

    for (const auto& [path, value] : before.entries) {
        auto it = after.entries.find(path);
        if (it == after.entries.end())
            delta.erase_entries.push_back(path);
    }
    for (const auto& [path, value] : after.entries) {
        auto it = before.entries.find(path);
        if (it == before.entries.end() || it->second != value)
            delta.upsert_entries.emplace(path, value);
    }

    if (before.catalogue_root != after.catalogue_root) {
        if (after.catalogue_root) {
            delta.catalogue = CatalogueDelta::set;
            delta.catalogue_root = after.catalogue_root;
        } else {
            delta.catalogue = CatalogueDelta::clear;
        }
    }

    std::map<ObjectId, GarbageRef> before_garbage;
    std::map<ObjectId, GarbageRef> after_garbage;
    for (const auto& garbage : before.garbage) {
        if (!before_garbage.emplace(garbage.id, garbage).second)
            return {};
    }
    for (const auto& garbage : after.garbage) {
        if (!after_garbage.emplace(garbage.id, garbage).second)
            return {};
    }
    for (const auto& garbage : before.garbage) {
        if (!after_garbage.contains(garbage.id))
            delta.erase_garbage.push_back(garbage.id);
    }
    for (const auto& garbage : after.garbage) {
        auto it = before_garbage.find(garbage.id);
        if (it == before_garbage.end() || it->second != garbage)
            delta.upsert_garbage.push_back(garbage);
    }

    return delta;
}

MetadataSnapshot apply_metadata_delta(const MetadataSnapshot& before, const MetadataDelta& delta) {
    MetadataSnapshot out = before;
    for (const auto& [node, sequence] : delta.mutation_sequences) {
        auto it = out.mutation_sequences.find(node);
        if (it != out.mutation_sequences.end() && sequence < it->second)
            throw DecodeError("metadata delta sequence regressed");
        out.mutation_sequences[node] = sequence;
    }
    for (const auto& path : delta.erase_entries) {
        auto normalized = normalize_path(path);
        if (normalized == "/")
            throw DecodeError("metadata delta removed root");
        out.entries.erase(normalized);
    }
    for (const auto& [path, value] : delta.upsert_entries)
        out.entries[normalize_path(path)] = value;
    switch (delta.catalogue) {
    case CatalogueDelta::unchanged:
        break;
    case CatalogueDelta::clear:
        out.catalogue_root.reset();
        break;
    case CatalogueDelta::set:
        if (!delta.catalogue_root)
            throw DecodeError("metadata delta missing catalogue root");
        out.catalogue_root = delta.catalogue_root;
        break;
    }
    for (const auto& id : delta.erase_garbage) {
        std::erase_if(out.garbage, [&](const GarbageRef& garbage) { return garbage.id == id; });
    }
    for (const auto& garbage : delta.upsert_garbage) {
        auto it = std::find_if(out.garbage.begin(), out.garbage.end(),
                               [&](const GarbageRef& value) { return value.id == garbage.id; });
        if (it == out.garbage.end())
            out.garbage.push_back(garbage);
        else
            *it = garbage;
    }
    auto root = out.entries.find("/");
    if (root == out.entries.end() || root->second.type != EntryType::directory)
        throw DecodeError("metadata delta lost root");
    return out;
}

Hash256 metadata_hash(uint64_t g, const Hash256& p, std::span<const uint8_t> d) {
    Writer w;
    w.u64(g);
    w.fixed(p.bytes);
    w.bytes(d);
    return sha256(w.data());
}
Bytes encode_metadata_record(const MetadataRecord& m) {
    Writer w;
    w.u64(m.generation);
    w.fixed(m.previous.bytes);
    w.fixed(m.hash.bytes);
    w.bytes(m.payload);
    return w.take();
}
MetadataRecord decode_metadata_record(std::span<const uint8_t> d) {
    Reader r(d);
    MetadataRecord m;
    m.generation = r.u64();
    m.previous.bytes = r.fixed<32>();
    m.hash.bytes = r.fixed<32>();
    m.payload = r.bytes();
    r.finish();
    if (!valid_metadata_record(m))
        throw DecodeError("bad metadata hash");
    return m;
}
MetadataRecord genesis_metadata() {
    MetadataSnapshot s;
    FsEntry r;
    r.type = EntryType::directory;
    r.mode = 0755;
    s.entries["/"] = r;
    MetadataRecord m;
    m.generation = 1;
    m.payload = encode_snapshot(s);
    m.hash = metadata_hash(1, m.previous, m.payload);
    return m;
}
bool valid_metadata_record(const MetadataRecord& m) {
    return m.generation && metadata_hash(m.generation, m.previous, m.payload) == m.hash;
}
MetadataReplica::MetadataReplica(std::filesystem::path r, std::array<uint8_t, 32> k)
    : p_(r / "metadata" / "current.meta"),
      committed_p_(r / "metadata" / "committed.meta"),
      checkpoint_p_(r / "metadata" / "checkpoint.meta"),
      journal_p_(r / "metadata" / "journal.log"), key_(k) {
    std::filesystem::create_directories(checkpoint_p_.parent_path());

    if (auto checkpoint = load(checkpoint_p_)) {
        cur_ = *checkpoint;
        committed_ = *checkpoint;
        load_journal();
        return;
    }

    // 0.8.x migration. The old format kept a fully materialised current and
    // committed snapshot. Preserve the committed record as the journal base and
    // represent a newer accepted-but-not-committed current record as a trusted
    // full seed entry. After the new files are durable, rename the old files so
    // accidentally starting a pre-0.9 binary fails instead of silently rolling
    // the namespace backwards.
    auto current = load(p_);
    auto committed = load(committed_p_);
    if (current.has_value() != committed.has_value())
        throw std::runtime_error(
            "incomplete metadata state: current.meta and committed.meta are both required");

    if (!current) {
        cur_ = genesis_metadata();
        committed_ = cur_;
        reset_checkpoint(committed_);
        return;
    }

    if (committed->generation > current->generation)
        throw std::runtime_error("metadata committed generation is newer than current");
    if (committed->generation == current->generation && committed->hash != current->hash)
        throw std::runtime_error("metadata current/committed generation conflict");

    cur_ = *committed;
    committed_ = *committed;
    if (current->hash != committed->hash) {
        // Migration ordering matters for an accepted-but-not-committed vote:
        // make its journal seed durable before publishing checkpoint.meta. If
        // we crash earlier, the old v10 files remain authoritative and migration
        // simply retries; if we crash later, replay cannot forget the vote.
        writefile(journal_p_, {});
        journal_records_ = 0;
        journal_bytes_ = 0;
        append_journal(JOURNAL_SEED_FULL, *current, current->payload);
        persist(checkpoint_p_, committed_);
        cur_ = *current;
    } else {
        reset_checkpoint(committed_);
    }

    std::error_code ec;
    std::filesystem::rename(p_, p_.string() + ".v10", ec);
    ec.clear();
    std::filesystem::rename(committed_p_, committed_p_.string() + ".v10", ec);
}

MetadataRecord MetadataReplica::current() const {
    std::lock_guard g(m_);
    return cur_;
}

MetadataRecord MetadataReplica::committed() const {
    std::lock_guard g(m_);
    return committed_;
}

MetadataIdentity MetadataReplica::current_identity() const {
    std::lock_guard g(m_);
    return {cur_.generation, cur_.hash};
}

MetadataIdentity MetadataReplica::committed_identity() const {
    std::lock_guard g(m_);
    return {committed_.generation, committed_.hash};
}

uint64_t MetadataReplica::generation() const {
    std::lock_guard g(m_);
    return cur_.generation;
}

uint64_t MetadataReplica::committed_generation() const {
    std::lock_guard g(m_);
    return committed_.generation;
}

void MetadataReplica::append_journal(uint8_t kind, const MetadataRecord& record,
                                     std::span<const uint8_t> body) {
    Writer plain;
    plain.u8(kind);
    plain.u64(record.generation);
    plain.fixed(record.previous.bytes);
    plain.fixed(record.hash.bytes);
    plain.bytes(body);
    auto sealed = aes_gcm_seal(key_, plain.data(), MJ);

    Writer envelope;
    envelope.fixed(sealed.nonce);
    envelope.fixed(sealed.tag);
    envelope.bytes(sealed.ciphertext);
    auto payload = envelope.take();
    if (payload.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("metadata journal record too large");

    Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    auto bytes = frame.take();

    int fd = open(journal_p_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        throw std::runtime_error(strerror(errno));
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            auto error = errno;
            close(fd);
            throw std::runtime_error(strerror(error));
        }
        offset += static_cast<size_t>(count);
    }
    if (fsync(fd)) {
        auto error = errno;
        close(fd);
        throw std::runtime_error(strerror(error));
    }
    close(fd);
    ++journal_records_;
    journal_bytes_ += bytes.size();
}

void MetadataReplica::load_journal() {
    journal_records_ = 0;
    journal_bytes_ = 0;
    if (!std::filesystem::exists(journal_p_))
        return;

    std::ifstream stream(journal_p_, std::ios::binary);
    Bytes bytes(std::istreambuf_iterator<char>(stream), {});
    size_t offset = 0;
    size_t valid = 0;
    auto input = std::span<const uint8_t>(bytes);

    while (offset + 4 <= bytes.size()) {
        Reader header(input.subspan(offset, 4));
        auto length = header.u32();
        header.finish();
        if (length > 256U * 1024U * 1024U)
            throw std::runtime_error("metadata journal record too large");
        if (offset + 4ULL + length > bytes.size())
            break; // interrupted append: discard the incomplete trailing frame.

        auto frame = input.subspan(offset + 4, length);
        Reader envelope(frame);
        auto nonce = envelope.fixed<12>();
        auto tag = envelope.fixed<16>();
        auto ciphertext = envelope.bytes();
        envelope.finish();
        auto plaintext = aes_gcm_open(key_, nonce, tag, ciphertext, MJ);
        Reader record_reader(plaintext);
        auto kind = record_reader.u8();
        MetadataRecord record;
        record.generation = record_reader.u64();
        record.previous.bytes = record_reader.fixed<32>();
        record.hash.bytes = record_reader.fixed<32>();
        auto body = record_reader.bytes();
        record_reader.finish();

        // Compaction writes the new checkpoint before truncating the old
        // journal. A crash in that small window legitimately leaves journal
        // frames already represented by the checkpoint; ignore only those
        // exact/older generations and replay anything newer.
        if (record.generation < committed_.generation ||
            (record.generation == committed_.generation && record.hash == committed_.hash)) {
            offset += 4 + length;
            valid = offset;
            ++journal_records_;
            journal_bytes_ += 4 + length;
            continue;
        }
        if (record.generation == committed_.generation && record.hash != committed_.hash)
            throw std::runtime_error("metadata journal conflicts with checkpoint");

        switch (kind) {
        case JOURNAL_PREPARE_FULL: {
            if (record.generation != cur_.generation + 1 || record.previous != cur_.hash)
                throw std::runtime_error("metadata journal full CAS chain broken");
            record.payload = std::move(body);
            if (!valid_metadata_record(record))
                throw std::runtime_error("metadata journal full CAS hash invalid");
            (void)decode_snapshot(record.payload);
            cur_ = std::move(record);
            break;
        }
        case JOURNAL_PREPARE_DELTA: {
            if (record.generation != cur_.generation + 1 || record.previous != cur_.hash)
                throw std::runtime_error("metadata journal delta CAS chain broken");
            auto snapshot = decode_snapshot(cur_.payload);
            auto delta = decode_metadata_delta(body);
            auto replayed = apply_metadata_delta(snapshot, delta);
            record.payload = metadata_delta_v1(body) ? encode_snapshot_v7(replayed)
                                                     : encode_snapshot(replayed);
            if (!valid_metadata_record(record))
                throw std::runtime_error("metadata journal delta CAS hash invalid");
            cur_ = std::move(record);
            break;
        }
        case JOURNAL_SEED_FULL: {
            record.payload = std::move(body);
            if (!valid_metadata_record(record))
                throw std::runtime_error("metadata journal seed hash invalid");
            (void)decode_snapshot(record.payload);
            if (record.generation < cur_.generation)
                throw std::runtime_error("metadata journal seed moved backwards");
            if (record.generation == cur_.generation && record.hash != cur_.hash)
                throw std::runtime_error("metadata journal seed generation conflict");
            cur_ = std::move(record);
            break;
        }
        case JOURNAL_COMMIT:
            if (record.generation == committed_.generation && record.hash == committed_.hash)
                break;
            if (record.generation != cur_.generation || record.hash != cur_.hash)
                throw std::runtime_error("metadata journal commit does not match current");
            committed_ = cur_;
            break;
        default:
            throw std::runtime_error("unknown metadata journal record");
        }

        offset += 4 + length;
        valid = offset;
        ++journal_records_;
        journal_bytes_ += 4 + length;
    }

    if (valid != bytes.size()) {
        std::filesystem::resize_file(journal_p_, valid);
        int fd = open(journal_p_.c_str(), O_WRONLY);
        if (fd >= 0) {
            (void)fsync(fd);
            close(fd);
        }
    }
}

void MetadataReplica::reset_checkpoint(const MetadataRecord& record) {
    persist(checkpoint_p_, record);
    writefile(journal_p_, {});
    journal_records_ = 0;
    journal_bytes_ = 0;
}

void MetadataReplica::compact_if_needed() {
    constexpr size_t record_threshold = 128;
    constexpr uint64_t byte_threshold = 8ULL * 1024 * 1024;
    if (cur_.generation != committed_.generation || cur_.hash != committed_.hash)
        return;
    if (journal_records_ < record_threshold && journal_bytes_ < byte_threshold)
        return;
    const auto records = journal_records_;
    const auto bytes = journal_bytes_;
    const auto generation = committed_.generation;
    const auto snapshot_bytes = committed_.payload.size();
    reset_checkpoint(committed_);
    Log::debug("metadata journal compacted generation=" + std::to_string(generation) +
               " records=" + std::to_string(records) +
               " journal_bytes=" + std::to_string(bytes) +
               " snapshot_bytes=" + std::to_string(snapshot_bytes));
}

bool MetadataReplica::cas(uint64_t generation, const Hash256& hash,
                          std::span<const uint8_t> payload, MetadataRecord* out) {
    std::lock_guard lock(m_);
    if (cur_.generation != generation || cur_.hash != hash) {
        if (out)
            *out = cur_;
        return false;
    }
    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    next.payload.assign(payload.begin(), payload.end());
    (void)decode_snapshot(next.payload);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);
    append_journal(JOURNAL_PREPARE_FULL, next, next.payload);
    cur_ = next;
    if (out)
        *out = next;
    return true;
}

bool MetadataReplica::cas_delta(uint64_t generation, const Hash256& hash,
                                std::span<const uint8_t> encoded_delta, MetadataRecord* out) {
    std::lock_guard lock(m_);
    if (cur_.generation != generation || cur_.hash != hash) {
        if (out)
            *out = cur_;
        return false;
    }
    auto before = decode_snapshot(cur_.payload);
    auto delta = decode_metadata_delta(encoded_delta);
    auto after = apply_metadata_delta(before, delta);

    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    next.payload = metadata_delta_v1(encoded_delta) ? encode_snapshot_v7(after)
                                                     : encode_snapshot(after);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);
    append_journal(JOURNAL_PREPARE_DELTA, next, encoded_delta);
    cur_ = next;
    if (out)
        *out = next;
    return true;
}

bool MetadataReplica::install_committed_delta(uint64_t generation, const Hash256& hash,
                                              std::span<const uint8_t> encoded_delta,
                                              const MetadataRecord& committed) {
    if (!valid_metadata_record(committed))
        return false;
    std::lock_guard lock(m_);

    if (cur_.generation == committed.generation && cur_.hash == committed.hash) {
        if (committed_.generation == committed.generation && committed_.hash == committed.hash)
            return true;
        append_journal(JOURNAL_COMMIT, cur_);
        committed_ = cur_;
        return true;
    }
    if (cur_.generation != generation || cur_.hash != hash)
        return false;

    auto before = decode_snapshot(cur_.payload);
    auto delta = decode_metadata_delta(encoded_delta);
    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    auto after = apply_metadata_delta(before, delta);
    next.payload = metadata_delta_v1(encoded_delta) ? encode_snapshot_v7(after)
                                                     : encode_snapshot(after);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);
    if (next.generation != committed.generation || next.previous != committed.previous ||
        next.hash != committed.hash || next.payload != committed.payload)
        return false;

    append_journal(JOURNAL_PREPARE_DELTA, next, encoded_delta);
    cur_ = next;
    append_journal(JOURNAL_COMMIT, cur_);
    committed_ = cur_;
    return true;
}

bool MetadataReplica::seed(const MetadataRecord& record) {
    if (!valid_metadata_record(record))
        return false;
    (void)decode_snapshot(record.payload);
    std::lock_guard lock(m_);
    if (record.generation < cur_.generation)
        return false;
    if (record.generation == cur_.generation) {
        if (record.hash == cur_.hash)
            return true;
        if (record.previous != cur_.previous)
            return false;
        if (record.hash < cur_.hash)
            return false;
    }
    append_journal(JOURNAL_SEED_FULL, record, record.payload);
    cur_ = record;
    return true;
}

bool MetadataReplica::remember_committed(const MetadataRecord& record) {
    if (!valid_metadata_record(record))
        return false;
    (void)decode_snapshot(record.payload);
    std::lock_guard lock(m_);
    if (record.generation < committed_.generation)
        return false;
    if (record.generation == committed_.generation && record.hash != committed_.hash)
        return false;
    if (record.hash == committed_.hash)
        return true;

    if (cur_.generation < record.generation) {
        append_journal(JOURNAL_SEED_FULL, record, record.payload);
        cur_ = record;
    }
    if (cur_.generation != record.generation || cur_.hash != record.hash)
        return false;

    append_journal(JOURNAL_COMMIT, cur_);
    committed_ = cur_;
    return true;
}

bool MetadataReplica::remember_current_committed(uint64_t generation, const Hash256& hash) {
    std::lock_guard lock(m_);
    if (cur_.generation != generation || cur_.hash != hash)
        return false;
    if (cur_.generation < committed_.generation)
        return false;
    if (cur_.generation == committed_.generation && cur_.hash != committed_.hash)
        return false;
    if (cur_.hash == committed_.hash)
        return true;
    append_journal(JOURNAL_COMMIT, cur_);
    committed_ = cur_;
    return true;
}

void MetadataReplica::compact() {
    std::lock_guard lock(m_);
    compact_if_needed();
}

void MetadataReplica::persist(const std::filesystem::path& path, const MetadataRecord& record) {
    auto encoded = encode_metadata_record(record);
    auto sealed = aes_gcm_seal(key_, encoded, DM);
    Writer writer;
    writer.raw(DM);
    writer.fixed(sealed.nonce);
    writer.fixed(sealed.tag);
    writer.bytes(sealed.ciphertext);
    writefile(path, writer.data());
}

std::optional<MetadataRecord> MetadataReplica::load(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path))
        return {};
    std::ifstream stream(path, std::ios::binary);
    Bytes bytes(std::istreambuf_iterator<char>(stream), {});
    Reader reader(bytes);
    auto magic = reader.raw(8);
    if (!std::equal(magic.begin(), magic.end(), DM.begin()))
        throw std::runtime_error("bad metadata file");
    auto nonce = reader.fixed<12>();
    auto tag = reader.fixed<16>();
    auto ciphertext = reader.bytes();
    reader.finish();
    return decode_metadata_record(aes_gcm_open(key_, nonce, tag, ciphertext, DM));
}

Hash256 metadata_namespace_signature(const MetadataSnapshot& snapshot) {
    Writer writer;
    writer.u64(snapshot.entries.size());
    for (const auto& [path, entry] : snapshot.entries) {
        writer.string(path);
        writer.u8(static_cast<uint8_t>(entry.type));
        if (entry.type != EntryType::file)
            continue;
        writer.u64(entry.size);
        writer.u32(entry.extents.size());
        for (const auto& extent : entry.extents) {
            writer.u64(extent.offset);
            writer.u64(extent.length);
            writer.u8(extent.hole);
            if (!extent.hole)
                writer.fixed(extent.id.bytes);
        }
    }
    return sha256(writer.data());
}

std::string normalize_path(const std::string& p) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/')
            ++i;
        auto s = i;
        while (i < p.size() && p[i] != '/')
            ++i;
        if (s == i)
            break;
        auto x = p.substr(s, i - s);
        if (x == ".")
            continue;
        if (x == "..") {
            if (!v.empty())
                v.pop_back();
            continue;
        }
        v.push_back(x);
    }
    if (v.empty())
        return "/";
    std::string o;
    for (auto& x : v)
        o += "/" + x;
    return o;
}
std::string parent_path(const std::string& p) {
    auto n = normalize_path(p);
    if (n == "/")
        return "/";
    auto i = n.rfind('/');
    return i ? n.substr(0, i) : "/";
}
std::string base_name(const std::string& p) {
    auto n = normalize_path(p);
    return n == "/" ? "/" : n.substr(n.rfind('/') + 1);
}
int64_t wall_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace macha
