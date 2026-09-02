// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_pool.hpp"

#include "codec.hpp"
#include "diagnostics.hpp"
#include "durable_file.hpp"
#include "log.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace macha {
namespace {
constexpr const char* marker_name = ".macha.backend";
constexpr std::string_view backend_marker_version = "macha-backend-v18";

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    std::string text((std::istreambuf_iterator<char>(in)), {});
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

std::string marker_text(const NodeId& node, const NodeId& token) {
    return std::string(backend_marker_version) + " " + to_string(node) + " " + to_string(token);
}

bool parse_marker(const std::string& text, NodeId& node, NodeId& token) {
    const auto first = text.find(' ');
    if (first == std::string::npos || text.substr(0, first) != backend_marker_version)
        return false;
    const auto second = text.find(' ', first + 1);
    if (second == std::string::npos)
        return false;
    auto n = unhex(text.substr(first + 1, second - first - 1));
    auto t = unhex(text.substr(second + 1));
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
    uint64_t instance_id{};
    std::shared_ptr<DurabilityDomain> durability_domain;
    std::shared_ptr<LocalStore> store;
    std::string last_error;
};

StoragePool::StoragePool(std::filesystem::path state_path, NodeId node_id,
                         std::vector<StorageBackendConfig> configs,
                         std::array<uint8_t, 32> key,
                         std::chrono::milliseconds durability_batch_window,
                         StoragePackingConfig packing)
    : state_path_(std::move(state_path)), node_id_(node_id), key_(key),
      durability_batch_window_(durability_batch_window), packing_(packing) {
    reconfigure(configs);
    refresh();
}

std::vector<std::shared_ptr<StoragePool::Backend>> StoragePool::snapshot() const {
    DiagnosticLock lock(mutex_, "storage.pool");
    return backends_;
}

std::shared_ptr<DurabilityDomain> StoragePool::domain_for(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        throw std::runtime_error("cannot stat storage durability domain " + path.string());
    const auto device = static_cast<uint64_t>(st.st_dev);
    std::lock_guard lock(domain_mutex_);
    if (const auto found = domains_by_device_.find(device); found != domains_by_device_.end() &&
        !found->second->failed()) {
        found->second->add_representative(path);
        return found->second;
    }
    auto domain = std::make_shared<DurabilityDomain>(next_domain_id_++, path,
                                                     durability_batch_window_);
    domains_by_device_[device] = domain;
    return domain;
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
        backend->durability_domain.reset();
        backend->instance_id = 0;
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
                durable_replace_file(state_id, marker_text(node_id_, marker_token) + "\n");
        } else if (have_state) {
            deactivate(backend, existing, "known backend marker is absent", generation);
            return false;
        } else {
            // 0.18 is an explicit fresh-backend format boundary. A directory
            // without a versioned backend marker is adoptable only when empty;
            // otherwise old loose objects/pack layouts could be silently
            // reinterpreted as current authoritative DATA.
            auto first = std::filesystem::directory_iterator(cfg.path, ec);
            if (ec)
                throw std::runtime_error("cannot inspect unversioned backend: " + ec.message());
            if (first != std::filesystem::directory_iterator())
                throw std::runtime_error(
                    "non-empty unversioned storage backend; 0.18 requires an empty backend");
            token = random_node_id();
            token_known = true;
            durable_replace_file(marker, marker_text(node_id_, token) + "\n");
            durable_replace_file(state_id, marker_text(node_id_, token) + "\n");
        }

        NodeId marker_node{}, marker_token{};
        if (!parse_marker(read_text(marker), marker_node, marker_token) ||
            marker_node != node_id_ || marker_token != token)
            throw std::runtime_error("backend marker changed");

        auto store = existing;
        std::shared_ptr<DurabilityDomain> durability_domain;
        uint64_t instance_id = 0;
        if (!store) {
            durability_domain = domain_for(cfg.path);
            {
                std::lock_guard lock(domain_mutex_);
                instance_id = next_backend_instance_++;
            }
            LocalStoreOptions options;
            options.limit = cfg.limit;
            options.reserve_free = cfg.reserve_free;
            options.pack_threshold = packing_.threshold;
            options.pack_target_size = packing_.target_size;
            store = std::make_shared<LocalStore>(cfg.path, options, key_,
                                                 LocalStoreMode::authoritative,
                                                 durability_domain);
        } else {
            std::lock_guard lock(backend->mutex);
            durability_domain = backend->durability_domain;
            instance_id = backend->instance_id;
        }

        bool installed = false;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->generation == generation && backend->configured &&
                backend->cfg.path == cfg.path && backend->cfg.limit == cfg.limit &&
                backend->cfg.reserve_free == cfg.reserve_free) {
                backend->token = token;
                backend->token_known = token_known;
                backend->store = store;
                backend->durability_domain = durability_domain;
                backend->instance_id = instance_id;
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
                if ((*existing)->cfg.limit != cfg.limit ||
                    (*existing)->cfg.reserve_free != cfg.reserve_free) {
                    (*existing)->cfg.limit = cfg.limit;
                    (*existing)->cfg.reserve_free = cfg.reserve_free;
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
    size_t online = 0;
    for (const auto& backend : snapshot()) {
        bool configured = false;
        {
            std::lock_guard lock(backend->mutex);
            configured = backend->configured;
        }
        if (configured && activate(backend))
            ++online;
    }
    online_backends_cached_.store(online, std::memory_order_relaxed);
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
            // put() is also an object reaffirmation. LocalStore::put() refreshes
            // the physical age when the content hash already exists, which keeps
            // reachability GC from racing a new write that reuses an old orphan.
            // Do not short-circuit this through has().
            if (store->put(id, data))
                return true;
        } catch (const std::exception& error) {
            Log::debug("storage write failed " + path.string() + ": " + error.what());
            deactivate(backend, store, error.what());
        }
    }
    return false;
}

std::optional<StoragePool::DurabilityToken> StoragePool::put_deferred(
    const ObjectId& id, std::span<const uint8_t> data) {
    for (const auto& backend : ranked(id)) {
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
        uint64_t backend_instance = 0;
        uint64_t domain = 0;
        {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store || !backend->durability_domain ||
                !backend->instance_id)
                continue;
            store = backend->store;
            path = backend->cfg.path;
            backend_instance = backend->instance_id;
            domain = backend->durability_domain->id();
        }
        try {
            const auto generation = store->put_deferred(id, data);
            if (!generation)
                continue;

            // The placement token names the exact backend incarnation which
            // accepted the object. If it disappeared/reopened while put() was
            // in progress, do not allow the new incarnation to satisfy the old
            // publication requirement merely because it shares a filesystem.
            {
                std::lock_guard lock(backend->mutex);
                if (!backend->online || backend->store != store ||
                    backend->instance_id != backend_instance ||
                    !backend->durability_domain || backend->durability_domain->id() != domain)
                    continue;
            }
            return DurabilityToken{domain, *generation, backend_instance};
        } catch (const std::exception& error) {
            Log::debug("storage deferred write failed " + path.string() + ": " + error.what());
            deactivate(backend, store, error.what());
        }
    }
    return {};
}

void StoragePool::durability_barrier(const DurabilityToken& token, DurabilityUrgency urgency) {
    if (!token.valid())
        throw std::runtime_error("invalid storage durability token");
    for (const auto& backend : snapshot()) {
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
        {
            std::lock_guard lock(backend->mutex);
            if (!backend->online || !backend->store || !backend->durability_domain ||
                backend->instance_id != token.backend_instance ||
                backend->durability_domain->id() != token.domain)
                continue;
            store = backend->store;
            path = backend->cfg.path;
        }
        try {
            store->durability_barrier(token.generation, urgency);
            {
                std::lock_guard lock(backend->mutex);
                if (!backend->online || backend->store != store ||
                    backend->instance_id != token.backend_instance ||
                    !backend->durability_domain || backend->durability_domain->id() != token.domain)
                    throw std::runtime_error(
                        "storage durability placement changed while awaiting barrier");
            }
            return;
        } catch (const std::exception& error) {
            Log::warn("storage durability barrier failed " + path.string() + ": " + error.what());
            deactivate(backend, store, error.what());
            throw;
        }
    }
    throw std::runtime_error("storage durability placement is no longer available");
}

bool StoragePool::durability_covered(const DurabilityToken& token) const {
    if (!token.valid())
        return false;
    for (const auto& backend : snapshot()) {
        std::lock_guard lock(backend->mutex);
        if (!backend->online || !backend->store || !backend->durability_domain ||
            backend->instance_id != token.backend_instance ||
            backend->durability_domain->id() != token.domain)
            continue;
        return backend->durability_domain->durable_generation() >= token.generation;
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

    if (!Log::enabled(LogLevel::all))
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
    Log::trace("DIAG storage-get window_ms=" + std::to_string(window_ms) +
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

bool StoragePool::valid(const ObjectId& id) const {
    for (const auto& backend : ranked(id)) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (!store)
            continue;
        if (store->valid(id))
            return true;
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

bool StoragePool::older_than(const ObjectId& id, std::chrono::milliseconds age) const {
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
                if (store->valid(id)) {
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
                    if (preferred_store->put(id, *data) && preferred_store->valid(id)) {
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

StoragePool::MaintenanceResult
StoragePool::gc_step(const std::vector<ObjectId>& live,
                     const std::vector<ObjectId>& protected_ids,
                     std::chrono::milliseconds orphan_grace, size_t operation_budget,
                     const std::function<bool()>& should_yield,
                     const std::function<bool(const ObjectId&)>& is_retained) {
    MaintenanceResult result;
    while (!operation_budget || result.objects < operation_budget) {
        if (should_yield && should_yield()) {
            result.yielded = true;
            return result;
        }
        bool pass_complete = false;
        auto item = next_physical(gc_cursor_, pass_complete);
        if (!item) {
            result.complete = pass_complete;
            return result;
        }
        ++result.objects;
        const auto& id = item->id;
        if (std::binary_search(live.begin(), live.end(), id) ||
            std::binary_search(protected_ids.begin(), protected_ids.end(), id) ||
            (is_retained && is_retained(id)))
            continue;

        try {
            // Re-check immediately before irreversible deletion. A foreground
            // metadata publication may install a retention claim after this GC
            // slice acquired its inventory but before it reaches this object.
            if (is_retained && is_retained(id))
                continue;
            // Object age is the orphan grace for data that never reached a
            // committed metadata reference. LocalStore::put() refreshes this
            // timestamp when an existing content hash is reaffirmed, preventing
            // a new write from racing an ancient orphan with identical bytes.
            const auto bytes = item->store->stored_size(id);
            if (item->store->remove_if_older_than(id, orphan_grace))
                result.bytes += bytes;
            else
                result.deferred = true;
        } catch (const std::exception& error) {
            Log::debug("garbage collection skipped local object " + to_string(id) +
                       " on " + item->path.string() + ": " + error.what());
        }
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

size_t StoragePool::compact_packs(std::stop_token stop) {
    size_t visited = 0;
    for (const auto& backend : snapshot()) {
        if (stop.stop_requested()) break;
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
            (void)store->compact_packs(stop);
            ++visited;
        } catch (const std::exception& error) {
            // Compaction failure must not make authoritative bytes disappear.
            // Keep the backend online and retry later; reads still use the old
            // durable representation unless a replacement was already installed.
            Log::warn("DATA pack compaction deferred path=" + path.string() +
                      " error=" + error.what());
        }
    }
    return visited;
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
    return online_backends_cached_.load(std::memory_order_relaxed);
}

LocalStoreDiagnostics StoragePool::diagnostics() const {
    LocalStoreDiagnostics total;
    for (const auto& backend : snapshot()) {
        std::shared_ptr<LocalStore> store;
        {
            std::lock_guard lock(backend->mutex);
            if (backend->online)
                store = backend->store;
        }
        if (!store)
            continue;
        const auto current = store->diagnostics();
        total.loose_reaffirmation_fast_paths += current.loose_reaffirmation_fast_paths;
        total.loose_reaffirmation_full_validations +=
            current.loose_reaffirmation_full_validations;
    }
    return total;
}
} // namespace macha
