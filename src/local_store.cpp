// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_store.hpp"
#include "codec.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> M{'D', 'H', 'T', 'O', 'B', 'J', '0', '1'};
void wa(int f, std::span<const uint8_t> b) {
    size_t p = 0;
    while (p < b.size()) {
        auto n = ::write(f, b.data() + p, b.size() - p);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(strerror(errno));
        }
        p += n;
    }
}
Bytes rf(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("cannot open object: " + std::string(strerror(errno)));

    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        auto error = std::string(strerror(errno));
        close(fd);
        throw std::runtime_error("cannot stat object: " + error);
    }

    Bytes out(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < out.size()) {
        auto n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            auto error = std::string(strerror(errno));
            close(fd);
            throw std::runtime_error("cannot read object: " + error);
        }
        if (n == 0) {
            close(fd);
            throw std::runtime_error("short object read");
        }
        done += static_cast<size_t>(n);
    }
    if (close(fd) != 0)
        throw std::runtime_error("cannot close object: " + std::string(strerror(errno)));
    return out;
}
void syncdir(const std::filesystem::path& p) {
    int f = ::open(p.c_str(), O_RDONLY | O_DIRECTORY);
    if (f >= 0) {
        (void)fsync(f);
        close(f);
    }
}
} // namespace

StorageLock::StorageLock(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    auto path = root / ".macha.lock";
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0)
        throw std::runtime_error("cannot open storage lock: " + std::string(strerror(errno)));
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        auto error = std::string(strerror(errno));
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("storage path is already in use: " + error);
    }
}

StorageLock::~StorageLock() {
    if (fd_ >= 0) {
        (void)flock(fd_, LOCK_UN);
        close(fd_);
    }
}

LocalStore::LocalStore(std::filesystem::path r, uint64_t l, std::array<uint8_t, 32> k)
    : root_(std::move(r)), objects_(root_ / "objects"), limit_(l), key_(k) {
    std::filesystem::create_directories(objects_);
    scan();
}
std::filesystem::path LocalStore::path(const ObjectId& i) const {
    auto s = to_string(i);
    return objects_ / s.substr(0, 2) / s.substr(2, 2) / (s + ".obj");
}
bool LocalStore::put(const ObjectId& i, std::span<const uint8_t> d) {
    if (object_id(d) != i)
        throw std::runtime_error("object hash mismatch");
    auto p = path(i);
    if (std::filesystem::exists(p))
        return true;
    std::lock_guard g(m_);
    if (std::filesystem::exists(p))
        return true;
    auto s = aes_gcm_seal(key_, d, i.bytes);
    Writer h;
    h.raw(M);
    h.u64(d.size());
    h.fixed(s.nonce);
    h.fixed(s.tag);
    uint64_t need = h.data().size() + s.ciphertext.size();
    if (used_ + need > limit_)
        return false;
    std::filesystem::create_directories(p.parent_path());
    auto t = p.string() + ".tmp." + std::to_string(getpid()) + "." + std::to_string(unix_ms());
    int f = ::open(t.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (f < 0)
        throw std::runtime_error(strerror(errno));
    try {
        wa(f, h.data());
        wa(f, s.ciphertext);
        if (fsync(f))
            throw std::runtime_error(strerror(errno));
        if (close(f))
            throw std::runtime_error(strerror(errno));
        f = -1;
        if (rename(t.c_str(), p.c_str()))
            throw std::runtime_error(strerror(errno));
        syncdir(p.parent_path());
        used_ += need;
        return true;
    } catch (...) {
        if (f >= 0)
            close(f);
        std::error_code e;
        std::filesystem::remove(t, e);
        throw;
    }
}
std::optional<Bytes> LocalStore::get(const ObjectId& i) const {
    auto p = path(i);
    if (!std::filesystem::exists(p))
        return {};
    auto e = rf(p);
    Reader r(e);
    auto m = r.raw(8);
    if (!std::equal(m.begin(), m.end(), M.begin()))
        throw std::runtime_error("bad object header");
    auto sz = r.u64();
    auto n = r.fixed<12>();
    auto t = r.fixed<16>();
    auto c = r.raw(r.remaining());
    auto d = aes_gcm_open(key_, n, t, c, i.bytes);
    if (d.size() != sz || object_id(d) != i)
        throw std::runtime_error("object integrity failure");
    return d;
}
bool LocalStore::has(const ObjectId& i) const {
    return std::filesystem::exists(path(i));
}
bool LocalStore::remove(const ObjectId& i) {
    std::lock_guard g(m_);
    auto p = path(i);
    std::error_code e;
    auto n = std::filesystem::file_size(p, e);
    if (e || !std::filesystem::remove(p, e))
        return false;
    used_ -= n;
    return true;
}
std::vector<ObjectId> LocalStore::list() const {
    std::vector<ObjectId> o;
    std::error_code e;
    for (auto& x : std::filesystem::recursive_directory_iterator(objects_, e)) {
        if (e)
            break;
        if (!x.is_regular_file())
            continue;
        auto n = x.path().filename().string();
        if (n.size() != 68 || n.substr(64) != ".obj")
            continue;
        auto b = unhex(n.substr(0, 64));
        if (!b || b->size() != 32)
            continue;
        ObjectId i;
        std::copy(b->begin(), b->end(), i.bytes.begin());
        o.push_back(i);
    }
    return o;
}
std::filesystem::path LocalStore::object_path(const ObjectId& i) const {
    return path(i);
}
uint64_t LocalStore::stored_size(const ObjectId& i) const {
    std::error_code e;
    auto n = std::filesystem::file_size(path(i), e);
    return e ? 0 : n;
}
std::filesystem::file_time_type LocalStore::last_write(const ObjectId& i) const {
    std::error_code e;
    auto t = std::filesystem::last_write_time(path(i), e);
    return e ? std::filesystem::file_time_type::min() : t;
}
void LocalStore::touch(const ObjectId& i) {
    std::error_code e;
    std::filesystem::last_write_time(path(i), std::filesystem::file_time_type::clock::now(), e);
}
bool LocalStore::older_than(const ObjectId& i, std::chrono::seconds age) const {
    std::error_code e;
    auto t = std::filesystem::last_write_time(path(i), e);
    if (e)
        return false;
    return std::filesystem::file_time_type::clock::now() - t > age;
}
void LocalStore::scan() {
    uint64_t n = 0;
    std::error_code e;
    for (auto& x : std::filesystem::recursive_directory_iterator(objects_, e)) {
        if (e)
            break;
        if (!x.is_regular_file())
            continue;
        auto name = x.path().filename().string();
        if (name.find(".tmp.") != std::string::npos) {
            std::error_code r;
            std::filesystem::remove(x.path(), r);
            continue;
        }
        n += x.file_size(e);
    }
    used_ = n;
}
NodeId load_or_create_node_id(const std::filesystem::path& r) {
    std::filesystem::create_directories(r);
    auto p = r / "node.id";
    if (std::filesystem::exists(p)) {
        std::ifstream f(p);
        std::string s;
        f >> s;
        auto b = unhex(s);
        if (!b || b->size() != 16)
            throw std::runtime_error("invalid node.id");
        NodeId i;
        std::copy(b->begin(), b->end(), i.bytes.begin());
        return i;
    }
    auto i = random_node_id();
    auto t = p.string() + ".tmp." + std::to_string(getpid());
    auto encoded = to_string(i) + "\n";
    int fd = ::open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create node.id: " + std::string(strerror(errno)));
    try {
        wa(fd, {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()});
        if (fsync(fd))
            throw std::runtime_error("cannot sync node.id: " + std::string(strerror(errno)));
        if (close(fd))
            throw std::runtime_error("cannot close node.id: " + std::string(strerror(errno)));
        fd = -1;
        if (::rename(t.c_str(), p.c_str()))
            throw std::runtime_error("cannot install node.id: " + std::string(strerror(errno)));
        syncdir(p.parent_path());
    } catch (...) {
        if (fd >= 0)
            close(fd);
        std::error_code error;
        std::filesystem::remove(t, error);
        throw;
    }
    return i;
}
} // namespace macha
