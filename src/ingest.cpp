// SPDX-License-Identifier: GPL-3.0-or-later
#include "ingest.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"
#include "media_catalogue.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace macha {
namespace {

uint64_t now_ms() { return unix_ms(); }

std::filesystem::path absolute_normal(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) absolute = path;
    return absolute.lexically_normal();
}

std::filesystem::path existing_real_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto real = std::filesystem::weakly_canonical(path, ec);
    if (ec) return absolute_normal(path);
    return real.lexically_normal();
}

bool path_under(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto p = absolute_normal(path);
    const auto r = absolute_normal(root);
    auto pi = p.begin();
    auto ri = r.begin();
    for (; ri != r.end(); ++ri, ++pi) {
        if (pi == p.end() || *pi != *ri) return false;
    }
    return true;
}

uint64_t directory_bytes(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return 0;
    uint64_t total = 0;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec) && !ec) {
            const auto size = it->file_size(ec);
            if (!ec && size <= UINT64_MAX - total) total += size;
        }
        ec.clear();
        it.increment(ec);
    }
    return total;
}

std::string safe_component(std::string value, std::string fallback = "Unknown") {
    for (char& c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || u < 0x20) c = ' ';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (value == "." || value == ".." || value.empty()) value = std::move(fallback);
    return value;
}

std::string lower_extension(const std::filesystem::path& path) {
    auto value = path.extension().string();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool sidecar_extension(const std::filesystem::path& path) {
    static const std::set<std::string> extensions{
        ".srt", ".ass", ".ssa", ".vtt", ".sub", ".idx", ".jpg", ".jpeg", ".png", ".webp"};
    return extensions.contains(lower_extension(path));
}

bool generic_artwork_name(const std::filesystem::path& path) {
    auto stem = path.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return stem == "folder" || stem == "cover" || stem == "poster" || stem == "front" ||
           stem == "backdrop" || stem == "fanart";
}

int64_t host_mtime(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(time.time_since_epoch().count());
}

std::string join_namespace(std::string_view root, const std::vector<std::string>& components) {
    std::string out = normalize_path(std::string(root));
    if (out == "/") out.clear();
    for (const auto& component : components) {
        out += '/';
        out += safe_component(component);
    }
    return normalize_path(out.empty() ? "/" : out);
}

std::string append_collision_suffix(std::string path, unsigned suffix) {
    std::filesystem::path p(path);
    const auto parent = p.parent_path().generic_string();
    const auto ext = p.extension().string();
    const auto stem = p.stem().string();
    return normalize_path(parent + "/" + stem + " (" + std::to_string(suffix) + ")" + ext);
}

Json file_json(const IngestFileProgress& file) {
    Json::Object o;
    o["source_path"] = file.source_path;
    o["destination_path"] = file.destination_path;
    o["temporary_path"] = file.temporary_path;
    o["size"] = file.size;
    o["copied"] = file.copied;
    o["source_mtime_ns"] = file.source_mtime_ns;
    o["completed"] = file.completed;
    o["skipped"] = file.skipped;
    return o;
}

Json job_json(const IngestJob& job) {
    Json::Object o;
    o["id"] = job.id;
    o["source_type"] = job.source_type;
    o["source_ref"] = job.source_ref;
    o["display_name"] = job.display_name;
    o["source_path"] = job.source_path.string();
    o["remove_source_on_complete"] = job.remove_source_on_complete;
    o["state"] = ingest_job_state_name(job.state);
    o["bytes_total"] = job.bytes_total;
    o["bytes_completed"] = job.bytes_completed;
    o["files_total"] = static_cast<uint64_t>(job.files_total);
    o["files_completed"] = static_cast<uint64_t>(job.files_completed);
    o["current_file"] = job.current_file;
    o["current_destination"] = job.current_destination;
    o["created_unix_ms"] = job.created_unix_ms;
    o["updated_unix_ms"] = job.updated_unix_ms;
    o["error"] = job.error;
    Json::Array files;
    files.reserve(job.files.size());
    for (const auto& file : job.files) files.push_back(file_json(file));
    o["files"] = std::move(files);
    return o;
}

uint64_t json_u64(const Json& object, std::string_view key, uint64_t fallback = 0) {
    const auto* value = object.find(key);
    return value ? value->asUInt64() : fallback;
}

std::string json_string(const Json& object, std::string_view key, std::string fallback = {}) {
    const auto* value = object.find(key);
    return value ? value->asString() : std::move(fallback);
}

bool json_bool(const Json& object, std::string_view key, bool fallback = false) {
    const auto* value = object.find(key);
    return value ? value->asBool() : fallback;
}

IngestFileProgress parse_file(const Json& value) {
    IngestFileProgress file;
    file.source_path = json_string(value, "source_path");
    file.destination_path = json_string(value, "destination_path");
    file.temporary_path = json_string(value, "temporary_path");
    file.size = json_u64(value, "size");
    file.copied = json_u64(value, "copied");
    if (const auto* mtime = value.find("source_mtime_ns")) file.source_mtime_ns = mtime->asInt64();
    file.completed = json_bool(value, "completed");
    file.skipped = json_bool(value, "skipped");
    return file;
}

IngestJob parse_job(const Json& value) {
    IngestJob job;
    job.id = json_string(value, "id");
    job.source_type = json_string(value, "source_type", "filesystem");
    job.source_ref = json_string(value, "source_ref");
    job.display_name = json_string(value, "display_name");
    job.source_path = json_string(value, "source_path");
    job.remove_source_on_complete = json_bool(value, "remove_source_on_complete");
    if (auto parsed = parse_ingest_job_state(json_string(value, "state"))) job.state = *parsed;
    job.bytes_total = json_u64(value, "bytes_total");
    job.bytes_completed = json_u64(value, "bytes_completed");
    job.files_total = static_cast<size_t>(json_u64(value, "files_total"));
    job.files_completed = static_cast<size_t>(json_u64(value, "files_completed"));
    job.current_file = json_string(value, "current_file");
    job.current_destination = json_string(value, "current_destination");
    job.created_unix_ms = json_u64(value, "created_unix_ms");
    job.updated_unix_ms = json_u64(value, "updated_unix_ms");
    job.error = json_string(value, "error");
    if (const auto* files = value.find("files")) {
        for (const auto& file : files->asArray()) job.files.push_back(parse_file(file));
    }
    return job;
}

} // namespace

std::string ingest_job_state_name(IngestJobState state) {
    switch (state) {
    case IngestJobState::queued: return "queued";
    case IngestJobState::scanning: return "scanning";
    case IngestJobState::importing: return "importing";
    case IngestJobState::paused: return "paused";
    case IngestJobState::blocked: return "blocked";
    case IngestJobState::completed: return "completed";
    case IngestJobState::cancelled: return "cancelled";
    case IngestJobState::failed: return "failed";
    }
    return "failed";
}

std::optional<IngestJobState> parse_ingest_job_state(std::string_view state) {
    if (state == "queued") return IngestJobState::queued;
    if (state == "scanning") return IngestJobState::scanning;
    if (state == "importing") return IngestJobState::importing;
    if (state == "paused") return IngestJobState::paused;
    if (state == "blocked") return IngestJobState::blocked;
    if (state == "completed") return IngestJobState::completed;
    if (state == "cancelled") return IngestJobState::cancelled;
    if (state == "failed") return IngestJobState::failed;
    return {};
}

StagingArea::StagingArea(IngestConfig config) : config_(std::move(config)) {
    if (!config_.staging_path.empty()) std::filesystem::create_directories(config_.staging_path);
}

uint64_t StagingArea::disk_usage_unlocked() const { return directory_bytes(config_.staging_path); }

void StagingArea::reconfigure_limit(uint64_t limit) {
    std::lock_guard lock(mutex_);
    config_.staging_limit = limit;
}

bool StagingArea::contains(const std::filesystem::path& path) const {
    return !config_.staging_path.empty() && path_under(path, config_.staging_path);
}

bool StagingArea::reserve(std::string owner, uint64_t bytes) {
    std::lock_guard lock(mutex_);
    uint64_t reserved = 0;
    for (const auto& [id, value] : reservations_) {
        if (id == owner) continue;
        if (value > UINT64_MAX - reserved) return false;
        reserved += value;
    }
    const auto disk = disk_usage_unlocked();
    if (disk > config_.staging_limit || reserved > config_.staging_limit - disk ||
        bytes > config_.staging_limit - disk - reserved)
        return false;
    reservations_[std::move(owner)] = bytes;
    return true;
}

void StagingArea::release(std::string_view owner) {
    std::lock_guard lock(mutex_);
    reservations_.erase(std::string(owner));
}

uint64_t StagingArea::reservation(std::string_view owner) const {
    std::lock_guard lock(mutex_);
    auto it = reservations_.find(std::string(owner));
    return it == reservations_.end() ? 0 : it->second;
}

StagingStatus StagingArea::status() const {
    std::lock_guard lock(mutex_);
    StagingStatus out;
    out.path = config_.staging_path;
    out.limit = config_.staging_limit;
    out.disk_bytes = disk_usage_unlocked();
    for (const auto& [_, value] : reservations_)
        if (value <= UINT64_MAX - out.reserved_bytes) out.reserved_bytes += value;
    out.accounted_bytes = out.disk_bytes > UINT64_MAX - out.reserved_bytes
                              ? UINT64_MAX
                              : out.disk_bytes + out.reserved_bytes;
    return out;
}

IngestManager::IngestManager(NodeRuntime& node, FileSystem& fs, IngestConfig config)
    : node_(node), fs_(fs), config_(std::move(config)), staging_(config_),
      state_file_(node_.config().state_path / "ingest" / "jobs.json") {
    if (config_.enabled) {
        std::filesystem::create_directories(state_file_.parent_path());
        if (!config_.staging_path.empty()) std::filesystem::create_directories(config_.staging_path);
        load_state();
    }
}

IngestManager::~IngestManager() { stop(); }

void IngestManager::load_state() {
    std::lock_guard lock(mutex_);
    std::ifstream in(state_file_, std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    try {
        auto root = Json::parse(text.str());
        const auto* jobs = root.find("jobs");
        if (!jobs) return;
        for (const auto& value : jobs->asArray()) {
            auto job = parse_job(value);
            if (job.id.empty()) continue;
            // Work interrupted by daemon exit is restartable. Explicit pauses and
            // terminal states remain exactly as the operator left them.
            if (job.state == IngestJobState::scanning || job.state == IngestJobState::importing)
                job.state = IngestJobState::queued;
            jobs_[job.id] = std::move(job);
        }
    } catch (const std::exception& e) {
        Log::warn("ingest state ignored: " + std::string(e.what()));
    }
}

void IngestManager::save_state_locked() const {
    if (!config_.enabled) return;
    Json::Array jobs;
    jobs.reserve(jobs_.size());
    for (const auto& [_, job] : jobs_) jobs.push_back(job_json(job));
    Json::Object root;
    root["version"] = static_cast<uint64_t>(1);
    root["jobs"] = std::move(jobs);
    const auto text = Json(std::move(root)).dump();
    const auto temp = state_file_.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write ingest state " + temp);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) throw std::runtime_error("cannot flush ingest state " + temp);
    }
    std::error_code ec;
    std::filesystem::rename(temp, state_file_, ec);
    if (ec) {
        std::filesystem::remove(state_file_, ec);
        ec.clear();
        std::filesystem::rename(temp, state_file_, ec);
    }
    if (ec) throw std::runtime_error("cannot replace ingest state: " + ec.message());
}

void IngestManager::start() {
    if (!config_.enabled || worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void IngestManager::request_stop() {
    if (worker_.joinable()) worker_.request_stop();
    cv_.notify_all();
}

void IngestManager::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
}

void IngestManager::reconfigure(IngestConfig config) {
    std::lock_guard lock(mutex_);
    // Paths define persisted/resumable job identity and cannot safely move live.
    if (config.staging_path != config_.staging_path || config.enabled != config_.enabled)
        Log::warn("ingest enabled/staging_path changes require restart");
    config_.copy_chunk_bytes = config.copy_chunk_bytes;
    config_.checkpoint_bytes = config.checkpoint_bytes;
    config_.blocked_retry = config.blocked_retry;
    config_.source_roots = std::move(config.source_roots);
    config_.staging_limit = config.staging_limit;
    staging_.reconfigure_limit(config.staging_limit);
}

bool IngestManager::allowed_external_source(const std::filesystem::path& path) const {
    for (const auto& root : config_.source_roots)
        if (path_under(path, existing_real_path(root))) return true;
    return false;
}

std::string IngestManager::submit_path(const std::filesystem::path& source,
                                       std::string source_type, std::string source_ref,
                                       std::string display_name, bool remove_source_on_complete,
                                       bool trusted_internal_source) {
    if (!config_.enabled) throw std::runtime_error("ingest is disabled");
    const auto normalized = existing_real_path(source);
    if (!trusted_internal_source && !allowed_external_source(normalized))
        throw std::runtime_error("source path is outside ingest.source_roots");
    if (trusted_internal_source && !staging_.contains(normalized))
        throw std::runtime_error("internal ingest source is outside staging area");

    std::error_code ec;
    if (!std::filesystem::exists(normalized, ec) || ec)
        throw std::runtime_error("ingest source does not exist");

    IngestJob job;
    job.id = to_string(random_node_id());
    job.source_type = std::move(source_type);
    job.source_ref = std::move(source_ref);
    job.display_name = display_name.empty() ? normalized.filename().string() : std::move(display_name);
    job.source_path = normalized;
    job.remove_source_on_complete = remove_source_on_complete;
    job.created_unix_ms = job.updated_unix_ms = now_ms();

    {
        std::lock_guard lock(mutex_);
        jobs_[job.id] = job;
        save_state_locked();
    }
    cv_.notify_all();
    Log::info("ingest queued id=" + job.id + " source=" + normalized.string());
    return job.id;
}

std::vector<IngestJob> IngestManager::jobs() const {
    std::lock_guard lock(mutex_);
    std::vector<IngestJob> out;
    out.reserve(jobs_.size());
    for (const auto& [_, job] : jobs_) out.push_back(job);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.created_unix_ms > b.created_unix_ms;
    });
    return out;
}

std::optional<IngestJob> IngestManager::job(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return {};
    return it->second;
}

bool IngestManager::pause(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == IngestJobState::completed || it->second.state == IngestJobState::cancelled ||
        it->second.state == IngestJobState::failed)
        return false;
    it->second.state = IngestJobState::paused;
    it->second.rate_bytes_per_second = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = now_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool IngestManager::resume(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state != IngestJobState::paused && it->second.state != IngestJobState::blocked &&
        it->second.state != IngestJobState::failed)
        return false;
    it->second.state = IngestJobState::queued;
    it->second.error.clear();
    it->second.updated_unix_ms = now_ms();
    save_state_locked();
    cv_.notify_all();
    return true;
}

bool IngestManager::cancel(std::string_view id) {
    IngestJob cancelled;
    bool active = false;
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(std::string(id));
        if (it == jobs_.end()) return false;
        if (it->second.state == IngestJobState::completed || it->second.state == IngestJobState::cancelled)
            return false;
        it->second.state = IngestJobState::cancelled;
        it->second.rate_bytes_per_second = 0;
        it->second.eta_seconds.reset();
        it->second.updated_unix_ms = now_ms();
        cancelled = it->second;
        active = active_job_id_ == it->first;
        save_state_locked();
    }
    if (!active) cleanup_partials(cancelled);
    cv_.notify_all();
    return true;
}

void IngestManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::string selected;
        {
            std::unique_lock lock(mutex_);
            const auto now = now_ms();
            for (const auto& [id, job] : jobs_) {
                if (job.state == IngestJobState::queued) {
                    selected = id;
                    break;
                }
                if (job.state == IngestJobState::blocked &&
                    now >= job.updated_unix_ms + static_cast<uint64_t>(config_.blocked_retry.count())) {
                    selected = id;
                    break;
                }
            }
            if (selected.empty()) {
                cv_.wait_for(lock, stop, std::chrono::milliseconds(500), [&] {
                    if (stop.stop_requested()) return true;
                    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& pair) {
                        return pair.second.state == IngestJobState::queued;
                    });
                });
                continue;
            }
        }
        {
            std::lock_guard lock(mutex_);
            active_job_id_ = selected;
        }
        process_job(selected, stop);
        {
            std::lock_guard lock(mutex_);
            if (active_job_id_ == selected) active_job_id_.clear();
        }
    }
}

void IngestManager::process_job(const std::string& id, std::stop_token stop) {
    IngestJob job;
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        job = it->second;
        if (job.state == IngestJobState::paused || job.state == IngestJobState::cancelled) return;
    }

    try {
        if (job.files.empty() && !plan_job(job, stop)) return;
        if (stop.stop_requested()) return;
        if (!import_job(job, stop)) return;
        if (job.state == IngestJobState::cancelled || job.state == IngestJobState::paused ||
            job.state == IngestJobState::blocked)
            return;

        if (job.remove_source_on_complete) {
            std::error_code ec;
            if (std::filesystem::is_directory(job.source_path, ec))
                std::filesystem::remove_all(job.source_path, ec);
            else
                std::filesystem::remove(job.source_path, ec);
            if (ec) Log::warn("ingest completed but source cleanup failed id=" + job.id + ": " + ec.message());
        }
        job.state = IngestJobState::completed;
        job.current_file.clear();
        job.current_destination.clear();
        job.rate_bytes_per_second = 0;
        job.eta_seconds = 0;
        job.error.clear();
        job.updated_unix_ms = now_ms();
        {
            std::lock_guard lock(mutex_);
            jobs_[id] = job;
            save_state_locked();
        }
        Log::info("ingest completed id=" + id + " files=" + std::to_string(job.files_completed) +
                  " bytes=" + std::to_string(job.bytes_completed));
    } catch (const std::exception& e) {
        if (stop.stop_requested()) {
            // Shutdown interruption is recoverable. Any committed hidden partial
            // is rediscovered by size when this job resumes after restart.
            job.updated_unix_ms = now_ms();
            std::lock_guard lock(mutex_);
            jobs_[id] = job;
            try { save_state_locked(); } catch (...) {}
            return;
        }
        job.state = IngestJobState::failed;
        job.error = e.what();
        job.rate_bytes_per_second = 0;
        job.eta_seconds.reset();
        job.updated_unix_ms = now_ms();
        std::lock_guard lock(mutex_);
        jobs_[id] = job;
        try { save_state_locked(); } catch (...) {}
        Log::warn("ingest failed id=" + id + ": " + e.what());
    }
}

bool IngestManager::plan_job(IngestJob& job, std::stop_token stop) {
    std::error_code ec;
    if (!std::filesystem::exists(job.source_path, ec) || ec) {
        set_blocked(job, "source path is unavailable");
        return false;
    }
    job.state = IngestJobState::scanning;
    job.error.clear();
    job.updated_unix_ms = now_ms();
    {
        std::lock_guard lock(mutex_);
        auto control = jobs_[job.id].state;
        if (control == IngestJobState::paused || control == IngestJobState::cancelled) return false;
        jobs_[job.id] = job;
        save_state_locked();
    }

    std::vector<std::filesystem::path> host_files;
    bool scan_incomplete = false;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(job.source_path, ec)) && !ec) {
        throw std::runtime_error("ingest source cannot be a symbolic link");
    }
    ec.clear();
    if (std::filesystem::is_regular_file(job.source_path, ec) && !ec) {
        host_files.push_back(job.source_path);
    } else if (std::filesystem::is_directory(job.source_path, ec) && !ec) {
        std::filesystem::recursive_directory_iterator it(
            job.source_path, std::filesystem::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            if (stop.stop_requested()) return false;
            {
                std::lock_guard lock(mutex_);
                auto state = jobs_[job.id].state;
                if (state == IngestJobState::paused || state == IngestJobState::cancelled) return false;
            }
            if (ec) {
                scan_incomplete = true;
                ec.clear();
            } else if (it->is_symlink(ec) && !ec) {
                // Never cross a removable/import root through a symlink.
            } else if (it->is_regular_file(ec) && !ec) {
                host_files.push_back(it->path());
            }
            it.increment(ec);
            if (ec) scan_incomplete = true;
        }
    } else {
        set_blocked(job, "source path is not a regular file or directory");
        return false;
    }

    if (scan_incomplete) {
        job.files.clear();
        job.bytes_total = job.bytes_completed = 0;
        job.files_total = job.files_completed = 0;
        set_blocked(job, "source scan was interrupted; source may be unavailable");
        return false;
    }
    for (const auto& source : host_files) {
        if (!std::filesystem::exists(source, ec) || ec) {
            job.files.clear();
            job.bytes_total = job.bytes_completed = 0;
            job.files_total = job.files_completed = 0;
            set_blocked(job, "source changed or disappeared during scan");
            return false;
        }
    }

    std::map<std::filesystem::path, std::vector<size_t>> media_by_parent;

    for (const auto& source : host_files) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(source, size_error);
        if (size_error || !size) continue;
        auto destination = choose_destination(source, size);
        if (destination.empty()) continue;

        // Resolve collisions once and persist the selected path. Resume never
        // re-runs this choice for a planned job.
        std::string candidate = destination;
        for (unsigned suffix = 2;; ++suffix) {
            try {
                (void)fs_.getattr(candidate);
                candidate = append_collision_suffix(destination, suffix);
            } catch (const FsError& e) {
                if (e.code() == ENOENT) break;
                throw;
            }
        }

        IngestFileProgress planned;
        planned.source_path = source.string();
        planned.destination_path = candidate;
        planned.temporary_path = candidate + ".macha-ingest-" + job.id.substr(0, 12) + ".part";
        planned.size = size;
        planned.source_mtime_ns = host_mtime(source);
        media_by_parent[source.parent_path()].push_back(job.files.size());
        job.files.push_back(std::move(planned));
        job.bytes_total += size;
    }

    // Preserve useful sidecars without making them catalogue entries. Associate
    // language subtitle/artwork names with the nearest media file in the same
    // source directory. Generic folder/cover/poster art is accepted only when
    // that directory resolves to one destination directory.
    for (const auto& source : host_files) {
        if (!sidecar_extension(source)) continue;
        const auto parent_it = media_by_parent.find(source.parent_path());
        if (parent_it == media_by_parent.end() || parent_it->second.empty()) continue;
        std::optional<std::string> destination_parent;
        const auto sidecar_stem = source.stem().string();
        for (const auto index : parent_it->second) {
            const auto& media_file = job.files[index];
            std::filesystem::path media_source(media_file.source_path);
            const auto media_stem = media_source.stem().string();
            const bool related = sidecar_stem.starts_with(media_stem) ||
                                 (generic_artwork_name(source) && parent_it->second.size() == 1);
            if (!related) continue;
            destination_parent = std::filesystem::path(media_file.destination_path).parent_path().generic_string();
            break;
        }
        if (!destination_parent) continue;
        std::error_code size_error;
        const auto size = std::filesystem::file_size(source, size_error);
        if (size_error) continue;
        IngestFileProgress planned;
        planned.source_path = source.string();
        const auto base_destination = normalize_path(
            *destination_parent + "/" + safe_component(source.filename().string()));
        planned.destination_path = base_destination;
        for (unsigned suffix = 2;; ++suffix) {
            try {
                (void)fs_.getattr(planned.destination_path);
                planned.destination_path = append_collision_suffix(base_destination, suffix);
            } catch (const FsError& e) {
                if (e.code() == ENOENT) break;
                throw;
            }
        }
        planned.temporary_path = planned.destination_path + ".macha-ingest-" + job.id.substr(0, 12) + ".part";
        planned.size = size;
        planned.source_mtime_ns = host_mtime(source);
        job.files.push_back(std::move(planned));
        job.bytes_total += size;
    }

    job.files_total = job.files.size();
    if (job.files.empty()) throw std::runtime_error("no supported media found in source");
    job.state = IngestJobState::queued;
    job.updated_unix_ms = now_ms();
    {
        std::lock_guard lock(mutex_);
        const auto control = jobs_[job.id].state;
        if (control == IngestJobState::paused || control == IngestJobState::cancelled) return false;
        jobs_[job.id] = job;
        save_state_locked();
    }
    return true;
}

std::string IngestManager::choose_destination(const std::filesystem::path& source, uint64_t size) {
    auto candidates = probe_host_media_candidates(source, size);
    if (candidates.empty()) return {};
    const auto& probe = candidates.front().probe;
    const auto& scanner = node_.config().catalogue.scanner;
    const auto filename = safe_component(source.filename().string(), "media");

    switch (probe.kind) {
    case MediaProbeKind::movie: {
        if (!scanner.movies.enabled || scanner.movies.roots.empty()) return {};
        auto title = safe_component(probe.title, source.stem().string());
        if (probe.year) title += " (" + std::to_string(*probe.year) + ")";
        return join_namespace(scanner.movies.roots.front(), {title, filename});
    }
    case MediaProbeKind::episode: {
        if (!scanner.tv.enabled || scanner.tv.roots.empty()) return {};
        auto series = safe_component(probe.series, "Unknown Series");
        std::string season = "Season Unknown";
        if (probe.season) {
            std::ostringstream formatted;
            formatted << "Season " << std::setfill('0') << std::setw(2) << *probe.season;
            season = formatted.str();
        }
        return join_namespace(scanner.tv.roots.front(), {series, season, filename});
    }
    case MediaProbeKind::track: {
        if (!scanner.music.enabled || scanner.music.roots.empty()) return {};
        auto artist = safe_component(probe.artist, "Unknown Artist");
        auto album = safe_component(probe.album, "Unknown Album");
        return join_namespace(scanner.music.roots.front(), {artist, album, filename});
    }
    }
    return {};
}

void IngestManager::ensure_namespace_parents(std::string_view path) {
    std::filesystem::path parent(std::filesystem::path(std::string(path)).parent_path());
    std::string current;
    for (const auto& part : parent) {
        const auto component = part.string();
        if (component.empty() || component == "/") continue;
        current += "/" + component;
        try {
            auto existing = fs_.getattr(current);
            if (existing.type != EntryType::directory)
                throw std::runtime_error("ingest destination parent is not a directory: " + current);
        } catch (const FsError& e) {
            if (e.code() != ENOENT) throw;
            const auto& policy = node_.config().filesystem;
            fs_.mkdir(current, 0755, policy.root_uid, policy.root_gid);
        }
    }
}

bool IngestManager::copy_file(IngestJob& job, IngestFileProgress& file, std::stop_token stop) {
    if (file.completed) return true;
    const std::filesystem::path source(file.source_path);
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || ec) {
        set_blocked(job, "source disappeared while importing: " + source.string());
        return false;
    }
    const auto actual_size = std::filesystem::file_size(source, ec);
    if (ec || actual_size != file.size || host_mtime(source) != file.source_mtime_ns) {
        set_blocked(job, "source changed while importing: " + source.string());
        return false;
    }

    ensure_namespace_parents(file.destination_path);
    try {
        auto partial = fs_.getattr(file.temporary_path);
        if (partial.type != EntryType::file) throw std::runtime_error("ingest partial is not a file");
        file.copied = std::min<uint64_t>(partial.size, file.size);
    } catch (const FsError& e) {
        if (e.code() != ENOENT) throw;
        // A crash may happen after the atomic partial->final rename but before
        // the job checkpoint is written. In that case the planned final path is
        // authoritative and a matching size completes the checkpoint cheaply.
        try {
            const auto final = fs_.getattr(file.destination_path);
            if (final.type == EntryType::file && final.size == file.size) {
                file.copied = file.size;
                file.completed = true;
                refresh_progress(job);
                return true;
            }
            throw std::runtime_error("ingest destination appeared with an unexpected type or size");
        } catch (const FsError& final_error) {
            if (final_error.code() != ENOENT) throw;
        }
        const auto& policy = node_.config().filesystem;
        fs_.create_file(file.temporary_path, 0644, policy.root_uid, policy.root_gid);
        file.copied = 0;
    }

    std::ifstream input(source, std::ios::binary);
    if (!input) {
        set_blocked(job, "source is not readable: " + source.string());
        return false;
    }
    input.seekg(static_cast<std::streamoff>(file.copied));
    if (!input) {
        set_blocked(job, "cannot seek source for resume: " + source.string());
        return false;
    }

    auto output = fs_.open_write(file.temporary_path, false, true);
    std::vector<uint8_t> buffer(config_.copy_chunk_bytes);
    uint64_t checkpoint_start = file.copied;
    auto sample_start = std::chrono::steady_clock::now();
    uint64_t sample_start_bytes = job.bytes_completed;

    while (file.copied < file.size && !stop.stop_requested()) {
        IngestJobState control = IngestJobState::importing;
        {
            std::lock_guard lock(mutex_);
            auto it = jobs_.find(job.id);
            if (it == jobs_.end()) return false;
            control = it->second.state;
        }
        if (control == IngestJobState::paused || control == IngestJobState::cancelled) {
            output->commit();
            file.copied = output->size();
            refresh_progress(job);
            job.state = control;
            job.updated_unix_ms = now_ms();
            if (control == IngestJobState::cancelled) {
                try { fs_.unlink(file.temporary_path); } catch (const FsError& e) {
                    if (e.code() != ENOENT) Log::warn("ingest cancel partial cleanup failed: " + std::string(e.what()));
                }
                file.copied = 0;
                refresh_progress(job);
            }
            std::lock_guard lock(mutex_);
            jobs_[job.id] = job;
            save_state_locked();
            return false;
        }

        const auto wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), file.size - file.copied));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted));
        const auto got = static_cast<size_t>(input.gcount());
        if (!got) {
            output->commit();
            file.copied = output->size();
            set_blocked(job, "short read from source: " + source.string());
            return false;
        }
        const auto written = output->write(file.copied, std::span<const uint8_t>(buffer.data(), got));
        if (written != got) throw std::runtime_error("short namespace write during ingest");
        file.copied += written;

        if (file.copied - checkpoint_start >= config_.checkpoint_bytes || file.copied == file.size) {
            output->commit();
            file.copied = output->size();
            checkpoint_start = file.copied;
            const auto sample_now = std::chrono::steady_clock::now();
            refresh_progress(job);
            const auto sample_bytes = job.bytes_completed >= sample_start_bytes
                                          ? job.bytes_completed - sample_start_bytes
                                          : 0;
            refresh_progress(job, sample_bytes, sample_now - sample_start);
            sample_start = sample_now;
            sample_start_bytes = job.bytes_completed;
            job.updated_unix_ms = now_ms();
            std::lock_guard lock(mutex_);
            const auto control = jobs_[job.id].state;
            if (control == IngestJobState::paused || control == IngestJobState::cancelled)
                job.state = control;
            jobs_[job.id] = job;
            save_state_locked();
            if (job.state != IngestJobState::importing) return false;
        }
    }
    if (stop.stop_requested()) {
        output->commit();
        file.copied = output->size();
        refresh_progress(job);
        job.updated_unix_ms = now_ms();
        std::lock_guard lock(mutex_);
        jobs_[job.id] = job;
        save_state_locked();
        return false;
    }

    output->commit();
    file.copied = output->size();
    if (file.copied != file.size) throw std::runtime_error("ingest committed size mismatch");
    fs_.rename(file.temporary_path, file.destination_path, true);
    file.completed = true;
    ++job.files_completed;
    refresh_progress(job);
    return true;
}

void IngestManager::refresh_progress(IngestJob& job, uint64_t sample_bytes,
                                     std::chrono::steady_clock::duration sample_time) {
    uint64_t total = 0;
    size_t completed = 0;
    for (const auto& file : job.files) {
        total += std::min(file.copied, file.size);
        if (file.completed) ++completed;
    }
    job.bytes_completed = total;
    job.files_completed = completed;
    if (sample_bytes && sample_time > std::chrono::steady_clock::duration::zero()) {
        const auto seconds = std::chrono::duration<double>(sample_time).count();
        const auto instantaneous = static_cast<uint64_t>(sample_bytes / std::max(0.001, seconds));
        job.rate_bytes_per_second = job.rate_bytes_per_second
                                        ? (job.rate_bytes_per_second * 3 + instantaneous) / 4
                                        : instantaneous;
    }
    if (job.rate_bytes_per_second && job.bytes_total >= job.bytes_completed)
        job.eta_seconds = (job.bytes_total - job.bytes_completed + job.rate_bytes_per_second - 1) /
                          job.rate_bytes_per_second;
    else
        job.eta_seconds.reset();
}

bool IngestManager::import_job(IngestJob& job, std::stop_token stop) {
    job.state = IngestJobState::importing;
    job.error.clear();
    {
        std::lock_guard lock(mutex_);
        const auto control = jobs_[job.id].state;
        if (control == IngestJobState::paused || control == IngestJobState::cancelled) return false;
        jobs_[job.id] = job;
        save_state_locked();
    }

    for (auto& file : job.files) {
        if (stop.stop_requested()) return false;
        if (file.completed) continue;
        job.current_file = file.source_path;
        job.current_destination = file.destination_path;
        job.updated_unix_ms = now_ms();
        {
            std::lock_guard lock(mutex_);
            const auto control = jobs_[job.id].state;
            if (control == IngestJobState::paused || control == IngestJobState::cancelled) return false;
            jobs_[job.id] = job;
            save_state_locked();
        }
        if (!copy_file(job, file, stop)) return false;
        job.updated_unix_ms = now_ms();
        {
            std::lock_guard lock(mutex_);
            jobs_[job.id] = job;
            save_state_locked();
        }
    }
    return true;
}

void IngestManager::set_blocked(IngestJob& job, std::string error) {
    job.state = IngestJobState::blocked;
    job.error = std::move(error);
    job.rate_bytes_per_second = 0;
    job.eta_seconds.reset();
    job.updated_unix_ms = now_ms();
    std::lock_guard lock(mutex_);
    jobs_[job.id] = job;
    save_state_locked();
}

void IngestManager::cleanup_partials(const IngestJob& job) {
    for (const auto& file : job.files) {
        if (file.completed || file.temporary_path.empty()) continue;
        try {
            fs_.unlink(file.temporary_path);
        } catch (const FsError& e) {
            if (e.code() != ENOENT)
                Log::warn("ingest partial cleanup failed path=" + file.temporary_path +
                          " reason=" + e.what());
        }
    }
}

bool IngestManager::should_pause_or_cancel(const IngestJob& job) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(job.id);
    return it == jobs_.end() || it->second.state == IngestJobState::paused ||
           it->second.state == IngestJobState::cancelled;
}

} // namespace macha
