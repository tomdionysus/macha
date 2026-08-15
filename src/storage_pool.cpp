// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_pool.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <unistd.h>

namespace macha {
namespace {
constexpr const char* marker_name = ".macha.backend";

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    std::string text((std::istreambuf_iterator<char>(in)), {});
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

void atomic_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    auto tmp = path.string() + ".tmp." + std::to_string(getpid()) + "." + std::to_string(unix_ms());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot create " + tmp);
        out << text << '\n';
        out.flush();
        if (!out)
            throw std::runtime_error("cannot write " + tmp);
    }
    std::filesystem::rename(tmp, path);
}

std::string marker_text(const NodeId& node, const NodeId& token) {
    return to_string(node) + " " + to_string(token);
}

bool parse_marker(const std::string& text, NodeId& node, NodeId& token) {
    auto space = text.find(' ');
    if (space == std::string::npos)
        return false;
    auto n = unhex(text.substr(0, space));
    auto t = unhex(text.substr(space + 1));
    if (!n || !t || n->size() != 16 || t->size() != 16)
        return false;
    std::copy(n->begin(), n->end(), node.bytes.begin());
    std::copy(t->begin(), t->end(), token.bytes.begin());
    return true;
}
} // namespace

struct StoragePool::Backend {
    mutable std::mutex mutex;
    StorageBackendConfig cfg;
    NodeId token{};
    bool token_known{};
    bool online{};
    bool configured{true};
    std::shared_ptr<LocalStore> store;
    std::string last_error;
};

StoragePool::StoragePool(std::filesystem::path state_path, NodeId node_id,
                         std::vector<StorageBackendConfig> configs,
                         std::array<uint8_t, 32> key)
    : state_path_(std::move(state_path)), node_id_(node_id), key_(key) {
    reconfigure(configs);
    refresh();
}

std::vector<std::shared_ptr<StoragePool::Backend>> StoragePool::snapshot() const {
    std::lock_guard lock(mutex_);
    return backends_;
}

std::filesystem::path StoragePool::identity_path(const std::filesystem::path& path) const {
    auto text = path.lexically_normal().string();
    auto hash = sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    return state_path_ / "backend-identities" / (to_string(hash) + ".id");
}

void StoragePool::deactivate(const std::shared_ptr<Backend>& backend,
                             const std::string& reason) const {
    std::lock_guard lock(backend->mutex);
    if (backend->online || backend->last_error != reason)
        Log::warn("storage backend offline " + backend->cfg.path.string() + ": " + reason);
    backend->online = false;
    backend->store.reset();
    backend->last_error = reason;
}

bool StoragePool::activate(const std::shared_ptr<Backend>& backend) {
    std::lock_guard lock(backend->mutex);
    try {
        std::error_code ec;
        if (!std::filesystem::is_directory(backend->cfg.path, ec) || ec) {
            const std::string reason = ec ? ec.message() : "configured path is not a directory";
            if (backend->online || backend->last_error != reason)
                Log::warn("storage backend offline " + backend->cfg.path.string() + ": " + reason);
            backend->online = false;
            backend->store.reset();
            backend->last_error = reason;
            return false;
        }

        auto state_id = identity_path(backend->cfg.path);
        auto marker = backend->cfg.path / marker_name;
        bool have_state = std::filesystem::exists(state_id, ec) && !ec;
        ec.clear();
        bool have_marker = std::filesystem::exists(marker, ec) && !ec;

        NodeId expected_node{}, expected_token{};
        if (have_state) {
            if (!parse_marker(read_text(state_id), expected_node, expected_token) ||
                expected_node != node_id_)
                throw std::runtime_error("invalid backend identity in node state");
            backend->token = expected_token;
            backend->token_known = true;
        }

        if (have_marker) {
            NodeId marker_node{}, marker_token{};
            if (!parse_marker(read_text(marker), marker_node, marker_token))
                throw std::runtime_error("invalid backend marker");
            if (marker_node != node_id_)
                throw std::runtime_error("backend belongs to another node identity");
            if (backend->token_known && marker_token != backend->token)
                throw std::runtime_error("backend identity mismatch (wrong disk mounted)");
            backend->token = marker_token;
            backend->token_known = true;
            if (!have_state)
                atomic_text(state_id, marker_text(node_id_, marker_token));
        } else if (have_state) {
            // A known disk disappeared. Never create a new marker on the bare
            // mountpoint: doing so could silently write onto the root filesystem.
            backend->online = false;
            backend->store.reset();
            backend->last_error = "known backend marker is absent";
            return false;
        } else {
            // First sighting of a newly configured backend. The path must already
            // exist (normally because the disk is mounted) before it is adopted.
            backend->token = random_node_id();
            backend->token_known = true;
            atomic_text(marker, marker_text(node_id_, backend->token));
            atomic_text(state_id, marker_text(node_id_, backend->token));
        }

        // Re-read the marker on every refresh. This detects an unmount/remount or
        // a different disk appearing at the same path.
        NodeId marker_node{}, marker_token{};
        if (!parse_marker(read_text(marker), marker_node, marker_token) ||
            marker_node != node_id_ || marker_token != backend->token)
            throw std::runtime_error("backend marker changed");

        if (!backend->store)
            backend->store = std::make_shared<LocalStore>(backend->cfg.path, backend->cfg.limit, key_);
        bool was_online = backend->online;
        backend->online = true;
        backend->last_error.clear();
        if (!was_online)
            Log::info("storage backend online " + backend->cfg.path.string());
        return true;
    } catch (const std::exception& error) {
        backend->online = false;
        backend->store.reset();
        if (backend->last_error != error.what())
            Log::warn("storage backend unavailable " + backend->cfg.path.string() + ": " + error.what());
        backend->last_error = error.what();
        return false;
    }
}

void StoragePool::reconfigure(const std::vector<StorageBackendConfig>& configs) {
    std::lock_guard lock(mutex_);
    std::set<std::filesystem::path> wanted;
    for (const auto& cfg : configs) {
        auto normalized = cfg.path.lexically_normal();
        if (!wanted.insert(normalized).second)
            throw std::runtime_error("duplicate storage backend: " + normalized.string());
        auto existing = std::find_if(backends_.begin(), backends_.end(), [&](const auto& backend) {
            std::lock_guard backend_lock(backend->mutex);
            return backend->cfg.path.lexically_normal() == normalized;
        });
        if (existing == backends_.end()) {
            auto backend = std::make_shared<Backend>();
            backend->cfg = cfg;
            backend->configured = true;
            backends_.push_back(std::move(backend));
        } else {
            std::lock_guard backend_lock((*existing)->mutex);
            (*existing)->configured = true;
            if ((*existing)->cfg.limit != cfg.limit) {
                (*existing)->cfg.limit = cfg.limit;
                (*existing)->store.reset();
                (*existing)->online = false;
            }
        }
    }
    for (auto& backend : backends_) {
        std::lock_guard backend_lock(backend->mutex);
        if (!wanted.contains(backend->cfg.path.lexically_normal())) {
            backend->configured = false;
            backend->online = false;
            backend->store.reset();
            backend->last_error = "removed from configuration";
        }
    }
}

void StoragePool::refresh() {
    for (const auto& backend : snapshot()) {
        bool configured = false;
        {
            std::lock_guard lock(backend->mutex);
            configured = backend->configured;
        }
        if (configured)
            (void)activate(backend);
    }
}

std::vector<std::shared_ptr<StoragePool::Backend>> StoragePool::ranked(const ObjectId& id) const {
    std::vector<NodeInfo> placement_nodes;
    std::vector<std::pair<NodeId, std::shared_ptr<Backend>>> lookup;
    for (const auto& backend : snapshot()) {
        NodeId token;
        uint64_t capacity = 0;
        bool eligible = false;
        {
            std::lock_guard lock(backend->mutex);
            // A previously adopted configured disk remains part of the stable
            // placement map while temporarily offline. put()/get() skip it and
            // use the ranked fallback; when it returns rebalance restores the
            // intended proportional placement. A never-seen path has no stable
            // token yet and therefore contributes neither capacity nor placement.
            eligible = backend->configured && backend->token_known;
            token = backend->token;
            capacity = backend->cfg.limit;
        }
        if (!eligible)
            continue;
        NodeInfo node;
        node.id = token;
        node.capacity = capacity;
        placement_nodes.push_back(node);
        lookup.push_back({token, backend});
    }

    auto order = capacity_placement_nodes(id.bytes, placement_nodes, 1);
    std::vector<std::shared_ptr<Backend>> out;
    out.reserve(order.size());
    for (const auto& placed : order) {
        auto it = std::find_if(lookup.begin(), lookup.end(), [&](const auto& item) {
            return item.first == placed.id;
        });
        if (it != lookup.end())
            out.push_back(it->second);
    }
    return out;
}

bool StoragePool::put(const ObjectId& id, std::span<const uint8_t> data) {
    for (const auto& backend : ranked(id)) {
        try {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store)
                continue;
            if (backend->store->has(id) || backend->store->put(id, data))
                return true;
        } catch (const std::exception& error) {
            // Drop the lock before deactivation takes it again.
            std::string reason = error.what();
            Log::debug("storage write failed " + backend->cfg.path.string() + ": " + reason);
            std::lock_guard lock(backend->mutex);
            backend->online = false;
            backend->store.reset();
            backend->last_error = reason;
        }
    }
    return false;
}

std::optional<Bytes> StoragePool::get(const ObjectId& id) const {
    for (const auto& backend : ranked(id)) {
        try {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store || !backend->store->has(id))
                continue;
            auto started = Clock::now();
            auto data = backend->store->get(id);
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            Log::debug("DIAG backend-get path=" + backend->cfg.path.string() +
                       " id=" + to_string(id) +
                       " result=" + std::to_string(data ? 1 : 0) +
                       " bytes=" + std::to_string(data ? data->size() : 0) +
                       " ms=" + std::to_string(elapsed.count()));
            return data;
        } catch (const std::exception& error) {
            // Object authentication/corruption is an object failure, not a disk
            // failure. Discard the bad local copy and let DHT fallback/repair
            // recover it without taking every other object on this backend down.
            Log::warn("discarding unreadable local object " + to_string(id) + " on " +
                      backend->cfg.path.string() + ": " + error.what());
            try {
                std::lock_guard lock(backend->mutex);
                if (backend->store)
                    (void)backend->store->remove(id);
            } catch (...) {
            }
        }
    }
    return {};
}

bool StoragePool::has(const ObjectId& id) const {
    for (const auto& backend : ranked(id)) {
        try {
            std::lock_guard lock(backend->mutex);
            if (backend->online && backend->store && backend->store->has(id))
                return true;
        } catch (...) {
        }
    }
    return false;
}

bool StoragePool::remove(const ObjectId& id) {
    bool removed = false;
    for (const auto& backend : snapshot()) {
        try {
            std::lock_guard lock(backend->mutex);
            if (backend->online && backend->store)
                removed = backend->store->remove(id) || removed;
        } catch (...) {
        }
    }
    return removed;
}

std::vector<ObjectId> StoragePool::list() const {
    std::set<ObjectId> unique;
    for (const auto& backend : snapshot()) {
        try {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store)
                continue;
            auto ids = backend->store->list();
            unique.insert(ids.begin(), ids.end());
        } catch (...) {
        }
    }
    return {unique.begin(), unique.end()};
}

bool StoragePool::older_than(const ObjectId& id, std::chrono::seconds age) const {
    for (const auto& backend : snapshot()) {
        try {
            std::lock_guard lock(backend->mutex);
            if (backend->online && backend->store && backend->store->has(id) &&
                backend->store->older_than(id, age))
                return true;
        } catch (...) {
        }
    }
    return false;
}

uint64_t StoragePool::rebalance_once(uint64_t budget_bytes) {
    auto ids = list();
    if (ids.empty()) {
        rebalance_offset_ = 0;
        return 0;
    }
    rebalance_offset_ %= ids.size();
    std::rotate(ids.begin(), ids.begin() + rebalance_offset_, ids.end());

    uint64_t moved = 0;
    size_t processed = 0;
    for (const auto& id : ids) {
        auto order = ranked(id);
        if (order.empty())
            break;
        auto preferred = order.front();

        std::vector<std::shared_ptr<Backend>> holders;
        uint64_t estimated = 0;
        for (const auto& backend : snapshot()) {
            try {
                std::lock_guard lock(backend->mutex);
                if (backend->online && backend->store && backend->store->has(id)) {
                    holders.push_back(backend);
                    estimated = std::max(estimated, backend->store->stored_size(id));
                }
            } catch (...) {
            }
        }
        if (holders.empty()) {
            ++processed;
            continue;
        }

        bool preferred_has = std::find(holders.begin(), holders.end(), preferred) != holders.end();
        if (!preferred_has) {
            if (budget_bytes && moved && moved + estimated > budget_bytes)
                break;
            std::optional<Bytes> data;
            for (const auto& holder : holders) {
                try {
                    std::lock_guard lock(holder->mutex);
                    if (holder->online && holder->store) {
                        data = holder->store->get(id);
                        if (data)
                            break;
                    }
                } catch (...) {
                }
            }
            if (data) {
                bool installed = false;
                try {
                    std::lock_guard lock(preferred->mutex);
                    if (preferred->online && preferred->store)
                        installed = preferred->store->put(id, *data);
                } catch (...) {
                }
                if (installed) {
                    moved += data->size();
                    holders.push_back(preferred);
                    preferred_has = true;
                }
            }
        }

        if (preferred_has) {
            for (const auto& holder : holders) {
                if (holder == preferred)
                    continue;
                try {
                    std::lock_guard lock(holder->mutex);
                    if (holder->online && holder->store)
                        (void)holder->store->remove(id);
                } catch (...) {
                }
            }
        }
        ++processed;
        if (budget_bytes && moved >= budget_bytes)
            break;
    }
    rebalance_offset_ = (rebalance_offset_ + processed) % ids.size();
    return moved;
}

uint64_t StoragePool::used() const {
    uint64_t total = 0;
    for (const auto& backend : snapshot()) {
        std::lock_guard lock(backend->mutex);
        if (backend->online && backend->store)
            total += backend->store->used();
    }
    return total;
}

uint64_t StoragePool::limit() const {
    uint64_t total = 0;
    for (const auto& backend : snapshot()) {
        std::lock_guard lock(backend->mutex);
        // Capacity is a topology weight, not a live free-space/online score.
        // Keep a known configured backend in the advertised capacity while it
        // is temporarily offline; removing it from configuration drops it.
        if (backend->configured && backend->token_known) {
            if (backend->cfg.limit > std::numeric_limits<uint64_t>::max() - total)
                return std::numeric_limits<uint64_t>::max();
            total += backend->cfg.limit;
        }
    }
    return total;
}

size_t StoragePool::online_backends() const {
    size_t count = 0;
    for (const auto& backend : snapshot()) {
        std::lock_guard lock(backend->mutex);
        if (backend->online && backend->store)
            ++count;
    }
    return count;
}
} // namespace macha
