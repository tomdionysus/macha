// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata.hpp"
#include "codec.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> SM5{'D', 'H', 'T', 'M', 'E', 'T', 'A', '5'},
    SM6{'D', 'H', 'T', 'M', 'E', 'T', 'A', '6'},
    SM7{'D', 'H', 'T', 'M', 'E', 'T', 'A', '7'},
    DM{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'};
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
    w.raw(SM7);
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
    for (const auto& garbage : s.garbage)
        w.fixed(garbage.id.bytes);
    return w.take();
}
MetadataSnapshot decode_snapshot(std::span<const uint8_t> d) {
    Reader r(d);
    auto m = r.raw(8);
    const bool v5 = std::equal(m.begin(), m.end(), SM5.begin());
    const bool v6 = std::equal(m.begin(), m.end(), SM6.begin());
    const bool v7 = std::equal(m.begin(), m.end(), SM7.begin());
    if (!v5 && !v6 && !v7)
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
    if (v7) {
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
    if ((v6 || v7) && r.u8()) {
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
        s.garbage.push_back(garbage);
    }
    r.finish();
    auto x = s.entries.find("/");
    if (x == s.entries.end() || x->second.type != EntryType::directory)
        throw DecodeError("missing root");
    return s;
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
      committed_p_(std::move(r) / "metadata" / "committed.meta"), key_(k) {
    std::filesystem::create_directories(p_.parent_path());
    auto current = load(p_);
    auto committed = load(committed_p_);

    if (!current && !committed) {
        cur_ = genesis_metadata();
        committed_ = cur_;
        persist(p_, cur_);
        persist(committed_p_, committed_);
        return;
    }
    if (!current || !committed)
        throw std::runtime_error(
            "incomplete metadata state: current.meta and committed.meta are both required");

    cur_ = *current;
    committed_ = *committed;
}
MetadataRecord MetadataReplica::current() const {
    std::lock_guard g(m_);
    return cur_;
}
MetadataRecord MetadataReplica::committed() const {
    std::lock_guard g(m_);
    return committed_;
}
bool MetadataReplica::cas(uint64_t g, const Hash256& h, std::span<const uint8_t> d,
                          MetadataRecord* out) {
    std::lock_guard l(m_);
    if (cur_.generation != g || cur_.hash != h) {
        if (out)
            *out = cur_;
        return false;
    }
    MetadataRecord n;
    n.generation = g + 1;
    n.previous = h;
    n.payload.assign(d.begin(), d.end());
    decode_snapshot(n.payload);
    n.hash = metadata_hash(n.generation, n.previous, n.payload);
    persist(p_, n);
    cur_ = n;
    if (out)
        *out = n;
    return true;
}
bool MetadataReplica::seed(const MetadataRecord& r) {
    if (!valid_metadata_record(r))
        return false;
    decode_snapshot(r.payload);
    std::lock_guard l(m_);
    if (r.generation < cur_.generation)
        return false;
    if (r.generation == cur_.generation) {
        if (r.hash == cur_.hash)
            return true;
        if (r.previous != cur_.previous)
            return false;
        if (r.hash < cur_.hash)
            return false;
    }
    persist(p_, r);
    cur_ = r;
    return true;
}
bool MetadataReplica::remember_committed(const MetadataRecord& r) {
    if (!valid_metadata_record(r))
        return false;
    decode_snapshot(r.payload);
    std::lock_guard l(m_);
    if (r.generation < committed_.generation)
        return false;
    if (r.generation == committed_.generation && r.hash != committed_.hash)
        return false;
    if (r.hash == committed_.hash)
        return true;
    persist(committed_p_, r);
    committed_ = r;
    return true;
}
void MetadataReplica::persist(const std::filesystem::path& path, const MetadataRecord& r) {
    auto p = encode_metadata_record(r);
    auto s = aes_gcm_seal(key_, p, DM);
    Writer w;
    w.raw(DM);
    w.fixed(s.nonce);
    w.fixed(s.tag);
    w.bytes(s.ciphertext);
    writefile(path, w.data());
}
std::optional<MetadataRecord> MetadataReplica::load(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path))
        return {};
    std::ifstream f(path, std::ios::binary);
    Bytes b(std::istreambuf_iterator<char>(f), {});
    Reader r(b);
    auto m = r.raw(8);
    if (!std::equal(m.begin(), m.end(), DM.begin()))
        throw std::runtime_error("bad metadata file");
    auto n = r.fixed<12>();
    auto t = r.fixed<16>();
    auto c = r.bytes();
    r.finish();
    return decode_metadata_record(aes_gcm_open(key_, n, t, c, DM));
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
