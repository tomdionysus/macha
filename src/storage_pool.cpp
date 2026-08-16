// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_pool.hpp"

#include "codec.hpp"
#include "diagnostics.hpp"
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
    // This mutex protects only the small in-memory state below. No filesystem
    // operation is performed while it is held.
    mutable std::mutex mutex;
    StorageBackendConfig cfg;
    NodeId token{};
    bool token_known{};
    bool online{};
    bool configured{true};
    uint64_t generation{1};
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
    DiagnosticLock lock(mutex_, "storage.pool");
    return backends_;
}

std::filesystem::path StoragePool::identity_path(const std::filesystem::path& path) const {
    auto text = path.lexically_normal().string();
    auto hash = sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    return state_path_ / "backend-identities" / (to_string(hash) + ".id");
}

void StoragePool::deactivate(const std::shared_ptr<Backend>& backend,
                             const std::shared_ptr<LocalStore>& expected,
                             const std::string& reason, uint64_t expected_generation) const {
    std::shared_ptr<LocalStore> retired;
    std::filesystem::path path;
    bool log = false;
    {
        std::lock_guard lock(backend->mutex);
        if ((expected_generation && backend->generation != expected_generation) ||
            (expected && backend->store != expected))
            return;
        path = backend->cfg.path;
        log = backend->online || backend->last_error != reason;
        backend->online = false;
        retired = std::move(backend->store);
        backend->last_error = reason;
    }
    // LocalStore destruction may join its accounting thread. Never do that
    // while holding backend state.
    retired.reset();
    if (log)
        Log::warn("storage backend offline " + path.string() + ": " + reason);
}

bool StoragePool::activate(const std::shared_ptr<Backend>& backend) {
    StorageBackendConfig cfg;
    NodeId known_token{};
    bool token_known = false;
    bool configured = false;
    bool was_online = false;
    uint64_t generation = 0;
    std::shared_ptr<LocalStore> existing;
    {
        std::lock_guard lock(backend->mutex);
        cfg = backend->cfg;
        known_token = backend->token;
        token_known = backend->token_known;
        configured = backend->configured;
        was_online = backend->online;
        generation = backend->generation;
        existing = backend->store;
    }
    if (!configured)
        return false;

    const auto activate_started = Clock::now();
    if (!was_online)
        Log::debug("storage backend probe begin path=" + cfg.path.string());
    try {
        std::error_code ec;
        if (!std::filesystem::is_directory(cfg.path, ec) || ec) {
            const std::string reason = ec ? ec.message() : "configured path is not a directory";
            deactivate(backend, existing, reason, generation);
            return false;
        }

        auto state_id = identity_path(cfg.path);
        auto marker = cfg.path / marker_name;
        bool have_state = std::filesystem::exists(state_id, ec) && !ec;
        ec.clear();
        bool have_marker = std::filesystem::exists(marker, ec) && !ec;

        NodeId token = known_token;
        NodeId expected_node{}, expected_token{};
        if (have_state) {
            if (!parse_marker(read_text(state_id), expected_node, expected_token) ||
                expected_node != node_id_)
                throw std::runtime_error("invalid backend identity in node state");
            token = expected_token;
            token_known = true;
        }

        if (have_marker) {
            NodeId marker_node{}, marker_token{};
            if (!parse_marker(read_text(marker), marker_node, marker_token))
                throw std::runtime_error("invalid backend marker");
            if (marker_node != node_id_)
                throw std::runtime_error("backend belongs to another node identity");
            if (token_known && marker_token != token)
                throw std::runtime_error("backend identity mismatch (wrong disk mounted)");
            token = marker_token;
            token_known = true;
            if (!have_state)
                atomic_text(state_id, marker_text(node_id_, marker_token));
        } else if (have_state) {
            deactivate(backend, existing, "known backend marker is absent", generation);
            return false;
        } else {
            token = random_node_id();
            token_known = true;
            atomic_text(marker, marker_text(node_id_, token));
            atomic_text(state_id, marker_text(node_id_, token));
        }

        NodeId marker_node{}, marker_token{};
        if (!parse_marker(read_text(marker), marker_node, marker_token) ||
            marker_node != node_id_ || marker_token != token)
            throw std::runtime_error("backend marker changed");

        auto store = existing;
        if (!store)
            store = std::make_shared<LocalStore>(cfg.path, cfg.limit, key_);

        bool installed = false;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->generation == generation && backend->configured &&
                backend->cfg.path == cfg.path && backend->cfg.limit == cfg.limit) {
                backend->token = token;
                backend->token_known = token_known;
                backend->store = store;
                backend->online = true;
                backend->last_error.clear();
                installed = true;
            }
        }
        if (!installed)
            return false;
        if (!was_online) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - activate_started);
            Log::info("storage backend online " + cfg.path.string() +
                      " elapsed_ms=" + std::to_string(elapsed.count()) +
                      " accounting=" + std::string(store->scan_complete() ? "ready" : "reconciling"));
        }
        return true;
    } catch (const std::exception& error) {
        deactivate(backend, existing, error.what(), generation);
        return false;
    }
}

void StoragePool::reconfigure(const std::vector<StorageBackendConfig>& configs) {
    std::vector<std::shared_ptr<LocalStore>> retired;
    {
        DiagnosticLock lock(mutex_, "storage.pool");
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
                    ++(*existing)->generation;
                    (*existing)->online = false;
                    retired.push_back(std::move((*existing)->store));
                }
            }
        }
        for (auto& backend : backends_) {
            std::lock_guard backend_lock(backend->mutex);
            if (!wanted.contains(backend->cfg.path.lexically_normal())) {
                backend->configured = false;
                backend->online = false;
                ++backend->generation;
                retired.push_back(std::move(backend->store));
                backend->last_error = "removed from configuration";
            }
        }
    }
    // Destroy retired stores after all backend/pool locks have been released.
    retired.clear();
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
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
        {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store)
                continue;
            store = backend->store;
            path = backend->cfg.path;
        }
        try {
            if (store->has(id) || store->put(id, data))
                return true;
        } catch (const std::exception& error) {
            Log::debug("storage write failed " + path.string() + ": " + error.what());
            deactivate(backend, store, error.what());
        }
    }
    return false;
}

void StoragePool::observe_get(size_t bytes, uint64_t elapsed) const {
    diag_gets_.fetch_add(1, std::memory_order_relaxed);
    diag_get_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    diag_get_ms_.fetch_add(elapsed, std::memory_order_relaxed);
    auto maximum = diag_get_max_ms_.load(std::memory_order_relaxed);
    while (maximum < elapsed &&
           !diag_get_max_ms_.compare_exchange_weak(maximum, elapsed, std::memory_order_relaxed)) {
    }

    if (!Log::enabled(LogLevel::debug))
        return;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now().time_since_epoch())
                            .count();
    auto previous = diag_get_report_ns_.load(std::memory_order_relaxed);
    constexpr int64_t report_ns = 5'000'000'000LL;
    if (!previous) {
        int64_t unset = 0;
        (void)diag_get_report_ns_.compare_exchange_strong(unset, now_ns,
                                                          std::memory_order_relaxed);
        return;
    }
    if (now_ns - previous < report_ns)
        return;
    if (!diag_get_report_ns_.compare_exchange_strong(previous, now_ns,
                                                     std::memory_order_relaxed))
        return;

    const auto window_ms = static_cast<uint64_t>((now_ns - previous) / 1'000'000LL);
    const auto gets = diag_gets_.exchange(0, std::memory_order_relaxed);
    const auto total_bytes = diag_get_bytes_.exchange(0, std::memory_order_relaxed);
    const auto total_ms = diag_get_ms_.exchange(0, std::memory_order_relaxed);
    const auto max_ms = diag_get_max_ms_.exchange(0, std::memory_order_relaxed);
    if (!gets)
        return;
    Log::debug("DIAG storage-get window_ms=" + std::to_string(window_ms) +
               " gets=" + std::to_string(gets) +
               " bytes=" + std::to_string(total_bytes) +
               " avg_ms=" + std::to_string(total_ms / gets) +
               " max_ms=" + std::to_string(max_ms));
}

std::optional<Bytes> StoragePool::get(const ObjectId& id) const {
    for (const auto& backend : ranked(id)) {
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
        {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store)
                continue;
            store = backend->store;
            path = backend->cfg.path;
        }
        try {
            if (!store->has(id))
                continue;
            auto started = Clock::now();
            auto data = store->get(id);
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            if (Log::enabled(LogLevel::all))
                Log::trace("DIAG backend-get path=" + path.string() + " id=" + to_string(id) +
                       " result=" + std::to_string(data ? 1 : 0) +
                       " bytes=" + std::to_string(data ? data->size() : 0) +
                       " ms=" + std::to_string(elapsed.count()));
            observe_get(data ? data->size() : 0, static_cast<uint64_t>(elapsed.count()));
            return data;
        } catch (const std::exception& error) {
            // Corrupt content is an object failure, not a backend failure.
            Log::warn("discarding unreadable local object " + to_string(id) + " on " +
                      path.string() + ": " + error.what());
            try {
                (void)store->remove(id);
            } catch (...) {
            }
        }
    }
    return {};
}

bool StoragePool::has(const ObjectId& id) const {
    for (const auto& backend : ranked(id)) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (!store)
            continue;
        try {
            if (store->has(id))
                return true;
        } catch (...) {
        }
    }
    return false;
}

bool StoragePool::remove(const ObjectId& id) {
    bool removed = false;
    for (const auto& backend : snapshot()) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (!store)
            continue;
        try {
            removed = store->remove(id) || removed;
        } catch (...) {
        }
    }
    return removed;
}

std::vector<ObjectId> StoragePool::list() const {
    full_list_scans_.fetch_add(1, std::memory_order_relaxed);
    std::set<ObjectId> unique;
    for (const auto& backend : snapshot()) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (!store)
            continue;
        try {
            auto ids = store->list();
            unique.insert(ids.begin(), ids.end());
        } catch (...) {
        }
    }
    return {unique.begin(), unique.end()};
}

std::optional<StoragePool::CursorItem> StoragePool::next_physical(Cursor& cursor,
                                                                 bool& pass_complete) const {
    pass_complete = false;
    struct View {
        std::shared_ptr<Backend> backend;
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
    };
    std::vector<View> views;
    for (const auto& backend : snapshot()) {
        std::lock_guard lock(backend->mutex);
        if (backend->online && backend->store)
            views.push_back({backend, backend->store, backend->cfg.path});
    }
    if (views.empty()) {
        cursor = {};
        pass_complete = true;
        return {};
    }

    // A backend set/store replacement invalidates only the maintenance cursor;
    // foreground operations are completely independent of this state.
    if (cursor.backend_count != views.size() || cursor.backend_index >= views.size() ||
        (cursor.store && cursor.store != views[cursor.backend_index].store)) {
        cursor = {};
        cursor.backend_count = views.size();
    }
    if (!cursor.store)
        cursor.store = views[cursor.backend_index].store;

    while (cursor.completed_backends < views.size()) {
        auto& view = views[cursor.backend_index];
        if (cursor.store != view.store) {
            cursor.store = view.store;
            cursor.local = {};
        }
        bool exhausted = false;
        if (auto id = view.store->next_object(cursor.local, exhausted))
            return CursorItem{view.backend, view.store, view.path, *id};
        if (!exhausted)
            return {};

        ++cursor.completed_backends;
        cursor.backend_index = (cursor.backend_index + 1) % views.size();
        cursor.store = views[cursor.backend_index].store;
        cursor.local = {};
    }

    cursor = {};
    pass_complete = true;
    return {};
}

std::optional<ObjectId> StoragePool::next_object(Cursor& cursor, bool& pass_complete) const {
    auto item = next_physical(cursor, pass_complete);
    return item ? std::optional<ObjectId>{item->id} : std::nullopt;
}

bool StoragePool::older_than(const ObjectId& id, std::chrono::seconds age) const {
    for (const auto& backend : snapshot()) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (!store)
            continue;
        try {
            if (store->has(id) && store->older_than(id, age))
                return true;
        } catch (...) {
        }
    }
    return false;
}

StoragePool::MaintenanceResult
StoragePool::rebalance_step(uint64_t budget_bytes, size_t operation_budget,
                            const std::function<bool()>& should_yield) {
    MaintenanceResult result;
    while (!operation_budget || result.objects < operation_budget) {
        if (should_yield && should_yield()) {
            result.yielded = true;
            return result;
        }
        bool pass_complete = false;
        auto item = next_physical(rebalance_cursor_, pass_complete);
        if (!item) {
            result.complete = pass_complete;
            return result;
        }
        ++result.objects;
        const auto id = item->id;

        auto order = ranked(id);
        if (order.empty())
            continue;
        auto preferred = order.front();

        struct Holder {
            std::shared_ptr<Backend> backend;
            std::shared_ptr<LocalStore> store;
        };
        std::vector<Holder> holders;
        uint64_t estimated = 0;
        for (const auto& backend : snapshot()) {
            std::shared_ptr<LocalStore> store;
            {
                std::lock_guard lock(backend->mutex);
                if (backend->online)
                    store = backend->store;
            }
            if (!store)
                continue;
            try {
                if (store->has(id)) {
                    holders.push_back({backend, store});
                    estimated = std::max(estimated, store->stored_size(id));
                }
            } catch (...) {
            }
        }
        if (holders.empty())
            continue;

        auto preferred_it = std::find_if(holders.begin(), holders.end(), [&](const Holder& holder) {
            return holder.backend == preferred;
        });
        bool preferred_has = preferred_it != holders.end();
        if (!preferred_has) {
            if (budget_bytes && result.bytes && result.bytes + estimated > budget_bytes)
                return result;

            std::optional<Bytes> data;
            for (const auto& holder : holders) {
                try {
                    data = holder.store->get(id);
                    if (data)
                        break;
                } catch (...) {
                }
            }

            std::shared_ptr<LocalStore> preferred_store;
            {
                std::lock_guard lock(preferred->mutex);
                if (preferred->online)
                    preferred_store = preferred->store;
            }
            if (data && preferred_store) {
                try {
                    if (preferred_store->put(id, *data)) {
                        result.bytes += data->size();
                        holders.push_back({preferred, preferred_store});
                        preferred_has = true;
                    }
                } catch (...) {
                }
            }
        }

        if (preferred_has) {
            for (const auto& holder : holders) {
                if (holder.backend == preferred)
                    continue;
                try {
                    (void)holder.store->remove(id);
                } catch (...) {
                }
            }
        }
        if (budget_bytes && result.bytes >= budget_bytes)
            return result;
    }
    return result;
}

StoragePool::MaintenanceResult
StoragePool::scrub_step(uint64_t budget_bytes, size_t operation_budget,
                        const std::function<bool()>& should_yield) {
    MaintenanceResult result;
    while (!operation_budget || result.objects < operation_budget) {
        if (should_yield && should_yield()) {
            result.yielded = true;
            return result;
        }
        bool pass_complete = false;
        auto item = next_physical(scrub_cursor_, pass_complete);
        if (!item) {
            result.complete = pass_complete;
            return result;
        }
        ++result.objects;
        try {
            auto data = item->store->get(item->id);
            if (data)
                result.bytes += data->size();
        } catch (const std::exception& error) {
            Log::warn("removing corrupt local object " + to_string(item->id) + " on " +
                      item->path.string() + ": " + error.what());
            try {
                (void)item->store->remove(item->id);
            } catch (...) {
            }
        }
        if (budget_bytes && result.bytes >= budget_bytes)
            return result;
    }
    return result;
}

uint64_t StoragePool::rebalance_once(uint64_t budget_bytes) {
    uint64_t total = 0;
    while (true) {
        auto step = rebalance_step(budget_bytes ? budget_bytes - total : 0, 256);
        total += step.bytes;
        if (step.complete || (budget_bytes && total >= budget_bytes))
            return total;
    }
}

uint64_t StoragePool::used() const {
    uint64_t total = 0;
    for (const auto& backend : snapshot()) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (store)
            total += store->used();
    }
    return total;
}

uint64_t StoragePool::limit() const {
    uint64_t total = 0;
    for (const auto& backend : snapshot()) {
        uint64_t capacity = 0;
        bool include = false;
        {
            std::lock_guard lock(backend->mutex);
            include = backend->configured && backend->token_known;
            capacity = backend->cfg.limit;
        }
        if (include) {
            if (capacity > std::numeric_limits<uint64_t>::max() - total)
                return std::numeric_limits<uint64_t>::max();
            total += capacity;
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
