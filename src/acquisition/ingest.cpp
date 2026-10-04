// SPDX-License-Identifier: GPL-3.0-or-later
#include "acquisition/ingest.hpp"
#include "durable_file.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"
#include "catalogue/media_catalogue.hpp"
#include "catalogue/media_information.hpp"
#include "supervised.hpp"

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

// ASCII case folding: names differing only by case are one name to Windows
// and to anyone browsing the library.
std::string fold_case(std::string_view value) {
    std::string out(value);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}


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
    o["catalogue_candidate"] = file.catalogue_candidate;
    return o;
}

Json job_json(const IngestJob& job) {
    Json::Object o;
    o["id"] = job.id;
    o["source_type"] = job.source_type;
    o["source_ref"] = job.source_ref;
    o["display_name"] = job.display_name;
    o["source_path"] = job.source_path.string();
    o["source_owned"] = job.source_owned;
    o["delete_source_on_clear"] = job.delete_source_on_clear;
    o["state"] = ingest_job_state_name(job.state);
    o["bytes_total"] = job.bytes_total;
    o["bytes_completed"] = job.bytes_completed;
    o["files_total"] = static_cast<uint64_t>(job.files_total);
    o["files_completed"] = static_cast<uint64_t>(job.files_completed);
    o["catalogue_total"] = static_cast<uint64_t>(job.catalogue_total);
    o["catalogue_pending"] = static_cast<uint64_t>(job.catalogue_pending);
    o["catalogue_catalogued"] = static_cast<uint64_t>(job.catalogue_catalogued);
    o["catalogue_no_match"] = static_cast<uint64_t>(job.catalogue_no_match);
    o["catalogue_failed"] = static_cast<uint64_t>(job.catalogue_failed);
    o["current_file"] = job.current_file;
    o["current_destination"] = job.current_destination;
    o["created_unix_ms"] = job.created_unix_ms;
    o["updated_unix_ms"] = job.updated_unix_ms;
    o["error_code"] = job.error_code;
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
    file.catalogue_candidate = json_bool(value, "catalogue_candidate", !sidecar_extension(file.source_path));
    return file;
}

IngestJob parse_job(const Json& value) {
    IngestJob job;
    job.id = json_string(value, "id");
    job.source_type = json_string(value, "source_type", "filesystem");
    job.source_ref = json_string(value, "source_ref");
    job.display_name = json_string(value, "display_name");
    job.source_path = json_string(value, "source_path");
    job.source_owned = json_bool(value, "source_owned", job.source_type == "torrent");
    job.delete_source_on_clear = json_bool(
        value, "delete_source_on_clear", json_bool(value, "remove_source_on_complete", false));
    if (auto parsed = parse_ingest_job_state(json_string(value, "state"))) job.state = *parsed;
    job.bytes_total = json_u64(value, "bytes_total");
    job.bytes_completed = json_u64(value, "bytes_completed");
    job.files_total = static_cast<size_t>(json_u64(value, "files_total"));
    job.files_completed = static_cast<size_t>(json_u64(value, "files_completed"));
    job.catalogue_total = static_cast<size_t>(json_u64(value, "catalogue_total"));
    job.catalogue_pending = static_cast<size_t>(json_u64(value, "catalogue_pending"));
    job.catalogue_catalogued = static_cast<size_t>(json_u64(value, "catalogue_catalogued"));
    job.catalogue_no_match = static_cast<size_t>(json_u64(value, "catalogue_no_match"));
    job.catalogue_failed = static_cast<size_t>(json_u64(value, "catalogue_failed"));
    job.current_file = json_string(value, "current_file");
    job.current_destination = json_string(value, "current_destination");
    job.created_unix_ms = json_u64(value, "created_unix_ms");
    job.updated_unix_ms = json_u64(value, "updated_unix_ms");
    job.error_code = json_string(value, "error_code");
    job.error = json_string(value, "error");
    // A record with an error but no code gets one: an error never shows without a code.
    if (!job.error.empty() && job.error_code.empty()) job.error_code = "import_failed";
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
    case IngestJobState::cataloguing: return "cataloguing";
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
    if (state == "cataloguing") return IngestJobState::cataloguing;
    if (state == "paused") return IngestJobState::paused;
    if (state == "blocked") return IngestJobState::blocked;
    if (state == "completed") return IngestJobState::completed;
    if (state == "cancelled") return IngestJobState::cancelled;
    if (state == "failed") return IngestJobState::failed;
    return {};
}

Json optional_u64(const std::optional<uint64_t>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json catalogue_summary_json(const IngestJob& job, const CatalogueHintSummary* detail) {
    Json::Object out;
    out["total"] = static_cast<uint64_t>(job.catalogue_total);
    out["pending"] = static_cast<uint64_t>(job.catalogue_pending);
    out["catalogued"] = static_cast<uint64_t>(job.catalogue_catalogued);
    out["no_match"] = static_cast<uint64_t>(job.catalogue_no_match);
    out["failed"] = static_cast<uint64_t>(job.catalogue_failed);
    if (job.state == IngestJobState::cataloguing || job.catalogue_pending)
        out["state"] = "processing";
    else if (!job.catalogue_total && job.state != IngestJobState::completed)
        out["state"] = "waiting";
    else if (job.catalogue_failed || job.catalogue_no_match)
        out["state"] = "completed_with_issues";
    else
        out["state"] = "completed";

    if (detail) {
        Json::Array items;
        items.reserve(detail->hints.size());
        for (const auto& hint : detail->hints) {
            Json::Object item;
            item["id"] = hint.id;
            item["path"] = hint.path;
            item["state"] = catalogue_hint_state_name(hint.state);
            item["provider"] = hint.provider.empty() ? Json(nullptr) : Json(hint.provider);
            item["media_id"] = hint.media_id.empty() ? Json(nullptr) : Json(hint.media_id);
            item["priority"] = static_cast<int64_t>(hint.priority);
            item["attempts"] = static_cast<uint64_t>(hint.attempts);
            item["result"] = hint.result.empty() ? Json(nullptr) : Json(hint.result);
            item["error_code"] = hint.error_code.empty() ? Json(nullptr) : Json(hint.error_code);
            item["error"] = hint.error.empty() ? Json(nullptr) : Json(hint.error);
            Json::Array ids;
            for (const auto& id : hint.catalogue_item_ids) ids.emplace_back(id);
            item["catalogue_item_ids"] = std::move(ids);
            items.emplace_back(std::move(item));
        }
        out["items"] = std::move(items);
    }
    return Json(std::move(out));
}

Json ingest_job_json(const IngestJob& job, bool include_files,
                     const CatalogueHintSummary* catalogue_detail) {
    Json::Object out;
    out["id"] = job.id;
    out["source_type"] = job.source_type;
    out["source_ref"] = job.source_ref.empty() ? Json(nullptr) : Json(job.source_ref);
    out["display_name"] = job.display_name;
    out["source_path"] = job.source_path.string();
    out["source_owned"] = job.source_owned;
    out["delete_source_on_clear"] = job.delete_source_on_clear;
    out["state"] = ingest_job_state_name(job.state);
    out["bytes_total"] = job.bytes_total;
    out["bytes_completed"] = job.bytes_completed;
    out["files_total"] = static_cast<uint64_t>(job.files_total);
    out["files_completed"] = static_cast<uint64_t>(job.files_completed);
    out["rate_bytes_per_second"] = job.rate_bytes_per_second;
    out["eta_seconds"] = optional_u64(job.eta_seconds);
    out["progress"] = job.bytes_total
                          ? Json(std::min(1.0, static_cast<double>(job.bytes_completed) /
                                                   static_cast<double>(job.bytes_total)))
                          : Json(nullptr);
    out["current_file"] = job.current_file.empty() ? Json(nullptr) : Json(job.current_file);
    out["current_destination"] =
        job.current_destination.empty() ? Json(nullptr) : Json(job.current_destination);
    out["catalogue"] = catalogue_summary_json(job, catalogue_detail);
    out["created_unix_ms"] = job.created_unix_ms;
    out["updated_unix_ms"] = job.updated_unix_ms;
    out["error_code"] = job.error_code.empty() ? Json(nullptr) : Json(job.error_code);
    out["error"] = job.error.empty() ? Json(nullptr) : Json(job.error);

    if (include_files) {
        Json::Array files;
        files.reserve(job.files.size());
        for (const auto& file : job.files) {
            Json::Object item;
            item["source_path"] = file.source_path;
            item["destination_path"] = file.destination_path;
            item["size"] = file.size;
            item["copied"] = file.copied;
            item["completed"] = file.completed;
            item["skipped"] = file.skipped;
            item["catalogue_candidate"] = file.catalogue_candidate;
            files.emplace_back(std::move(item));
        }
        out["files"] = std::move(files);
    }
    return Json(std::move(out));
}

// The persistence shape plus the transient rate and ETA, which are never
// persisted but a live peer still reports.
Json ingest_job_wire_json(const IngestJob& job) {
    auto out = job_json(job);
    out["rate_bytes_per_second"] = job.rate_bytes_per_second;
    out["eta_seconds"] = optional_u64(job.eta_seconds);
    return out;
}

IngestJob parse_ingest_job_wire(const Json& value) {
    auto job = parse_job(value);
    job.rate_bytes_per_second = json_u64(value, "rate_bytes_per_second");
    if (const auto* eta = value.find("eta_seconds"); eta && !eta->isNull())
        job.eta_seconds = eta->asUInt64();
    return job;
}

Json staging_capacity_json(const StagingStatus& status) {
    Json::Object out;
    out["limit_bytes"] = status.limit;
    out["disk_bytes"] = status.disk_bytes;
    out["reserved_bytes"] = status.reserved_bytes;
    out["accounted_bytes"] = status.accounted_bytes;
    out["free_bytes"] = status.limit > status.accounted_bytes ? status.limit - status.accounted_bytes : 0;
    return Json(std::move(out));
}

StagingArea::StagingArea(IngestConfig config)
    : config_(std::move(config)), limit_(config_.staging_limit) {
    if (!config_.staging_path.empty()) std::filesystem::create_directories(config_.staging_path);
}

uint64_t StagingArea::disk_usage_locked() const { return directory_bytes(config_.staging_path); }

void StagingArea::reconfigure_limit(uint64_t limit) {
    limit_.store(limit, std::memory_order_relaxed);
}

bool StagingArea::contains(const std::filesystem::path& path) const {
    return !config_.staging_path.empty() && path_under(path, config_.staging_path);
}

bool StagingArea::reserve(std::string owner, uint64_t bytes) {
    Lock lock(mutex_);
    uint64_t reserved = 0;
    for (const auto& [id, value] : reservations_) {
        if (id == owner) continue;
        if (value > UINT64_MAX - reserved) return false;
        reserved += value;
    }
    const auto disk = disk_usage_locked();
    const auto limit = limit_.load(std::memory_order_relaxed);
    if (disk > limit || reserved > limit - disk || bytes > limit - disk - reserved)
        return false;
    reservations_[std::move(owner)] = bytes;
    return true;
}

void StagingArea::release(std::string_view owner) {
    Lock lock(mutex_);
    reservations_.erase(std::string(owner));
}

uint64_t StagingArea::reservation(std::string_view owner) const {
    Lock lock(mutex_);
    auto it = reservations_.find(std::string(owner));
    return it == reservations_.end() ? 0 : it->second;
}

std::filesystem::path StagingArea::trash_path() const { return config_.staging_path / ".trash"; }

bool StagingArea::discard(const std::filesystem::path& path) {
    if (config_.staging_path.empty() || !contains(path)) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    const auto trash = trash_path();
    std::filesystem::create_directories(trash, ec);
    const auto target = trash / (to_string(random_node_id()) + "-" + path.filename().string());
    std::filesystem::rename(path, target, ec);
    if (ec) {
        Log::warn("staging discard could not move " + path.string() + " to trash: " + ec.message());
        return false;
    }
    {
        Lock lock(trash_mutex_);
        trash_pending_ = true;
    }
    trash_cv_.notify_all();
    return true;
}

size_t StagingArea::empty_trash() {
    const auto trash = trash_path();
    std::error_code ec;
    if (!std::filesystem::exists(trash, ec)) return 0;
    size_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(trash, ec)) {
        std::error_code remove_error;
        std::filesystem::remove_all(entry.path(), remove_error);
        if (remove_error)
            Log::warn("staging trash entry not deleted yet " + entry.path().string() + ": " +
                      remove_error.message());
        else
            ++removed;
    }
    return removed;
}

void StagingArea::wait_for_trash(std::stop_token stop, std::chrono::milliseconds interval) {
    Lock lock(trash_mutex_);
    trash_cv_.wait_for(lock.native(), stop, interval,
                       [this]() MACHA_REQUIRES(trash_mutex_) { return trash_pending_; });
    trash_pending_ = false;
}

StagingStatus StagingArea::status() const {
    Lock lock(mutex_);
    StagingStatus out;
    out.path = config_.staging_path;
    out.limit = limit_.load(std::memory_order_relaxed);
    out.disk_bytes = disk_usage_locked();
    for (const auto& [_, value] : reservations_)
        if (value <= UINT64_MAX - out.reserved_bytes) out.reserved_bytes += value;
    out.accounted_bytes = out.disk_bytes > UINT64_MAX - out.reserved_bytes
                              ? UINT64_MAX
                              : out.disk_bytes + out.reserved_bytes;
    return out;
}

IngestManager::IngestManager(NodeRuntime& node, FileSystem& fs, CatalogueHintQueue& hints,
                             IngestConfig config, MediaInformationService* media_information)
    : node_(node), fs_(fs), hints_(hints), media_information_(media_information),
      config_(std::move(config)), enabled_(config_.enabled),
      max_concurrent_jobs_(config_.max_concurrent_jobs), staging_(config_),
      state_file_(node_.config().state_path / "ingest" / "jobs.json") {
    if (config_.enabled) {
        std::filesystem::create_directories(state_file_.parent_path());
        if (!config_.staging_path.empty()) std::filesystem::create_directories(config_.staging_path);
        load_state();
    }
}

IngestManager::~IngestManager() { stop(); }

Bytes IngestManager::handle_jobs_query(std::span<const uint8_t> request_payload) const {
    std::string job_id;
    if (!request_payload.empty()) {
        try {
            const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                                   request_payload.size());
            auto request = Json::parse(text);
            if (const auto* id = request.find("job_id"); id && id->isString())
                job_id = id->asString();
        } catch (const std::exception&) {
            // A malformed request lists all rather than failing the peer.
        }
    }
    Json::Array out_jobs;
    if (job_id.empty()) {
        for (const auto& job : jobs()) out_jobs.push_back(ingest_job_wire_json(job));
    } else if (auto found = job(job_id)) {
        out_jobs.push_back(ingest_job_wire_json(*found));
    }
    Json::Object out;
    out["jobs"] = std::move(out_jobs);
    const auto text = Json(std::move(out)).dump();
    return Bytes(text.begin(), text.end());
}

Bytes IngestManager::handle_job_action(std::span<const uint8_t> request_payload) {
    std::string job_id, action;
    try {
        const std::string text(reinterpret_cast<const char*>(request_payload.data()),
                               request_payload.size());
        auto request = Json::parse(text);
        if (const auto* id = request.find("job_id"); id && id->isString()) job_id = id->asString();
        if (const auto* act = request.find("action"); act && act->isString())
            action = act->asString();
    } catch (const std::exception&) {
    }
    Json::Object out;
    const bool exists = job(job_id).has_value();
    out["exists"] = exists;
    bool changed = false;
    if (exists) {
        if (action == "pause") changed = pause(job_id);
        else if (action == "resume") changed = resume(job_id);
        else if (action == "cancel") changed = cancel(job_id);
        else if (action == "clear") changed = clear(job_id);
    }
    out["changed"] = changed;
    if (auto updated = job(job_id))
        out["job"] = ingest_job_wire_json(*updated);
    else
        out["job"] = Json(nullptr);
    const auto text = Json(std::move(out)).dump();
    return Bytes(text.begin(), text.end());
}


void IngestManager::load_state() {
    std::vector<std::pair<std::string, std::string>> migration_hints;
    {
        Lock lock(mutex_);
        std::ifstream in(state_file_, std::ios::binary);
        if (!in) return;
        std::ostringstream text;
        text << in.rdbuf();
        try {
            auto root = Json::parse(text.str());
            const auto version = json_u64(root, "version", 1);
            const auto* jobs = root.find("jobs");
            if (!jobs) return;
            for (const auto& value : jobs->asArray()) {
                auto job = parse_job(value);
                if (job.id.empty()) continue;
                // Interrupted work restarts; pauses and terminal states stand.
                if (job.state == IngestJobState::scanning || job.state == IngestJobState::importing)
                    job.state = IngestJobState::queued;

                // A version-1 state file completed jobs without the catalogue
                // phase: send them back to it once.
                if (version < 2 && job.state == IngestJobState::completed) {
                    bool queued_catalogue = false;
                    for (const auto& file : job.files) {
                        if (file.completed && file.catalogue_candidate) {
                            migration_hints.emplace_back(job.id, file.destination_path);
                            queued_catalogue = true;
                        }
                    }
                    if (queued_catalogue) job.state = IngestJobState::cataloguing;
                }
                jobs_[job.id] = std::move(job);
            }
        } catch (const std::exception& e) {
            Log::warn("ingest state ignored: " + std::string(e.what()));
        }
    }
    for (const auto& [job_id, path] : migration_hints) {
        (void)hints_.submit(path, "ingest", job_id, CatalogueHintPriority::ingest);
        if (media_information_) (void)media_information_->request_path(path);
    }
}

void IngestManager::save_state_locked() const {
    if (!enabled_) return;
    Json::Array jobs;
    jobs.reserve(jobs_.size());
    for (const auto& [_, job] : jobs_) jobs.push_back(job_json(job));
    Json::Object root;
    root["version"] = static_cast<uint64_t>(2);
    root["jobs"] = std::move(jobs);
    durable_replace_file(state_file_, Json(std::move(root)).dump());
}

void IngestManager::start() {
    if (!enabled_ || !workers_.empty()) return;
    const size_t count = std::max<size_t>(1, max_concurrent_jobs_);
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this](std::stop_token stop) {
            run_supervised_loop("ingest", stop, [this, stop] { loop(stop); });
        });
    }
    catalogue_worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("ingest-catalogue", stop, [this, stop] { catalogue_loop(stop); });
    });
    // Deletes discarded payloads off the request path, including any a
    // previous run left in the trash.
    trash_worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("staging-trash", stop, [this, stop] {
            while (!stop.stop_requested()) {
                if (const auto removed = staging_.empty_trash())
                    Log::info("staging trash emptied entries=" + std::to_string(removed));
                staging_.wait_for_trash(stop, std::chrono::seconds(30));
            }
        });
    });
    Log::info("ingest started workers=" + std::to_string(count));
}

void IngestManager::request_stop() {
    for (auto& worker : workers_)
        if (worker.joinable()) worker.request_stop();
    if (catalogue_worker_.joinable()) catalogue_worker_.request_stop();
    if (trash_worker_.joinable()) trash_worker_.request_stop();
    cv_.notify_all();
}

void IngestManager::stop() {
    request_stop();
    for (auto& worker : workers_)
        if (worker.joinable()) worker.join();
    workers_.clear();
    if (catalogue_worker_.joinable()) catalogue_worker_.join();
    if (trash_worker_.joinable()) trash_worker_.join();
}

void IngestManager::reconfigure(IngestConfig config) {
    Lock lock(mutex_);
    // Paths define persisted/resumable job identity and cannot safely move live.
    if (config.staging_path != config_.staging_path || config.enabled != config_.enabled)
        Log::warn("ingest enabled/staging_path changes require restart");
    // The pool is sized at start(): resizing live would stop threads mid-copy.
    if (config.max_concurrent_jobs != config_.max_concurrent_jobs)
        Log::warn("ingest.max_concurrent_jobs change requires restart (running with " +
                  std::to_string(config_.max_concurrent_jobs) + ")");
    config_.copy_chunk_bytes = config.copy_chunk_bytes;
    config_.checkpoint_bytes = config.checkpoint_bytes;
    config_.blocked_retry = config.blocked_retry;
    config_.source_roots = std::move(config.source_roots);
    config_.staging_limit = config.staging_limit;
    config_.delete_owned_source_on_clear = config.delete_owned_source_on_clear;
    config_.delete_external_source_on_clear = config.delete_external_source_on_clear;
    config_.delete_owned_source_on_cancel = config.delete_owned_source_on_cancel;
    staging_.reconfigure_limit(config.staging_limit);
}

bool IngestManager::allowed_external_source(const std::filesystem::path& path) const {
    std::vector<std::filesystem::path> roots;
    {
        Lock lock(mutex_);
        roots = config_.source_roots;
    }
    for (const auto& root : roots)
        if (path_under(path, existing_real_path(root))) return true;
    return false;
}

std::string IngestManager::submit_path(const std::filesystem::path& source,
                                       std::string source_type, std::string source_ref,
                                       std::string display_name,
                                       std::optional<bool> delete_source_on_clear,
                                       bool trusted_internal_source, bool source_owned) {
    if (!enabled_) throw std::runtime_error("ingest is disabled");
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
    job.source_owned = source_owned;
    job.delete_source_on_clear = delete_source_on_clear.value_or(
        source_owned ? delete_owned_source_on_clear() : delete_external_source_on_clear());
    job.created_unix_ms = job.updated_unix_ms = now_ms();

    {
        Lock lock(mutex_);
        jobs_[job.id] = job;
        try {
            save_state_locked();
        } catch (...) {
            jobs_.erase(job.id);
            throw;
        }
    }
    cv_.notify_all();
    Log::info("ingest queued id=" + job.id + " source=" + normalized.string());
    return job.id;
}

size_t IngestManager::active_jobs() const {
    Lock lock(mutex_);
    return active_job_ids_.size();
}

size_t IngestManager::peak_active_jobs() const {
    Lock lock(mutex_);
    return peak_active_jobs_;
}

std::vector<IngestJob> IngestManager::jobs() const {
    Lock lock(mutex_);
    std::vector<IngestJob> out;
    out.reserve(jobs_.size());
    for (const auto& [_, job] : jobs_) out.push_back(job);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.created_unix_ms > b.created_unix_ms;
    });
    return out;
}

std::optional<IngestJob> IngestManager::job(std::string_view id) const {
    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return {};
    return it->second;
}

bool IngestManager::pause(std::string_view id) {
    Lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state == IngestJobState::cataloguing ||
        it->second.state == IngestJobState::completed ||
        it->second.state == IngestJobState::cancelled ||
        it->second.state == IngestJobState::failed)
        return false;
    const auto previous = it->second;
    it->second.state = IngestJobState::paused;
    it->second.rate_bytes_per_second = 0;
    it->second.eta_seconds.reset();
    it->second.updated_unix_ms = now_ms();
    try {
        save_state_locked();
    } catch (...) {
        it->second = previous;
        throw;
    }
    cv_.notify_all();
    return true;
}

void IngestManager::set_resume_listener(std::function<void(std::string_view)> listener) {
    Lock lock(resume_listener_mutex_);
    resume_listener_ = std::move(listener);
}

bool IngestManager::resume(std::string_view id) {
    {
        Lock lock(mutex_);
        if (!resume_locked(id)) return false;
    }
    Lock listener_lock(resume_listener_mutex_);
    if (resume_listener_) resume_listener_(id);
    return true;
}

bool IngestManager::resume_locked(std::string_view id) {
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return false;
    if (it->second.state != IngestJobState::paused && it->second.state != IngestJobState::blocked &&
        it->second.state != IngestJobState::failed)
        return false;
    const auto previous = it->second;
    it->second.state = IngestJobState::queued;
    it->second.error.clear();
    it->second.error_code.clear();
    it->second.updated_unix_ms = now_ms();
    try {
        save_state_locked();
    } catch (...) {
        it->second = previous;
        throw;
    }
    cv_.notify_all();
    return true;
}

void IngestManager::cleanup_source(const IngestJob& job) {
    if (job.source_path.empty()) return;
    if (job.source_owned) {
        if (!staging_.contains(job.source_path))
            throw std::runtime_error("refusing to delete owned ingest source outside staging area");
        std::error_code ec;
        if (!std::filesystem::exists(job.source_path, ec)) return;
        ec.clear();
        // Into the trash, not deleted here: this runs in the clear request.
        if (staging_.discard(job.source_path)) return;
        if (std::filesystem::is_directory(job.source_path, ec) && !ec)
            std::filesystem::remove_all(job.source_path, ec);
        else {
            ec.clear();
            std::filesystem::remove(job.source_path, ec);
        }
        if (ec) throw std::runtime_error("cannot remove owned ingest source: " + ec.message());
        return;
    }

    if (!allowed_external_source(job.source_path))
        throw std::runtime_error("refusing to delete ingest source outside configured source roots");

    // Removes only the files in the persisted plan, never the whole source
    // tree, which may hold files not imported; then prunes empty directories
    // up to the submitted source root.
    std::vector<std::filesystem::path> imported;
    imported.reserve(job.files.size());
    for (const auto& file : job.files) {
        if (!file.completed || file.source_path.empty()) continue;
        const auto source = existing_real_path(file.source_path);
        if (!path_under(source, job.source_path) || !allowed_external_source(source))
            throw std::runtime_error("refusing to delete planned source outside submitted ingest root");
        imported.push_back(source);
    }
    std::sort(imported.begin(), imported.end(), [](const auto& a, const auto& b) {
        return a.native().size() > b.native().size();
    });
    for (const auto& source : imported) {
        std::error_code ec;
        std::filesystem::remove(source, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            throw std::runtime_error("cannot remove imported source file: " + ec.message());
    }

    std::error_code ec;
    if (std::filesystem::is_regular_file(job.source_path, ec)) return;
    ec.clear();
    std::vector<std::filesystem::path> parents;
    for (const auto& source : imported) {
        auto parent = source.parent_path();
        while (path_under(parent, job.source_path) && parent != job.source_path) {
            parents.push_back(parent);
            parent = parent.parent_path();
        }
    }
    std::sort(parents.begin(), parents.end(), [](const auto& a, const auto& b) {
        return a.native().size() > b.native().size();
    });
    parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
    for (const auto& parent : parents) {
        ec.clear();
        (void)std::filesystem::remove(parent, ec); // only succeeds when empty
    }
    ec.clear();
    (void)std::filesystem::remove(job.source_path, ec); // submitted directory, if now empty
}

bool IngestManager::cancel(std::string_view id) {
    IngestJob cancelled;
    bool active = false;
    {
        Lock lock(mutex_);
        auto it = jobs_.find(std::string(id));
        if (it == jobs_.end()) return false;
        if (it->second.state == IngestJobState::completed || it->second.state == IngestJobState::cancelled)
            return false;
        const auto previous = it->second;
        it->second.state = IngestJobState::cancelled;
        it->second.rate_bytes_per_second = 0;
        it->second.eta_seconds.reset();
        it->second.updated_unix_ms = now_ms();
        cancelled = it->second;
        active = active_job_ids_.contains(it->first);
        try {
            save_state_locked();
        } catch (...) {
            it->second = previous;
            throw;
        }
    }
    if (!active) {
        cleanup_partials(cancelled);
        if (cancelled.source_owned && delete_owned_source_on_cancel()) {
            try { cleanup_source(cancelled); }
            catch (const std::exception& e) {
                Log::warn("ingest cancel source cleanup failed id=" + cancelled.id + ": " + e.what());
            }
        }
    }
    cv_.notify_all();
    return true;
}

bool IngestManager::clear(std::string_view id) {
    IngestJob terminal_job;
    bool active = false;
    {
        Lock lock(mutex_);
        auto it = jobs_.find(std::string(id));
        if (it == jobs_.end()) return false;
        if (it->second.state != IngestJobState::completed &&
            it->second.state != IngestJobState::cancelled &&
            it->second.state != IngestJobState::failed)
            return false;
        terminal_job = it->second;
        active = active_job_ids_.contains(it->first);
    }

    const bool delete_source = terminal_job.delete_source_on_clear &&
        (terminal_job.state == IngestJobState::completed || terminal_job.source_owned);
    // A cancelled job's worker owns its files until it lets go at its next
    // control check.
    if (!active) {
        if (delete_source) cleanup_source(terminal_job);
        cleanup_partials(terminal_job);
    }
    (void)hints_.erase_origin("ingest", terminal_job.id);

    bool owed = false;
    {
        Lock lock(mutex_);
        auto it = jobs_.find(std::string(id));
        if (it == jobs_.end()) return false;
        if (it->second.state != terminal_job.state) return false;
        jobs_.erase(it);
        save_state_locked();
        if (active) {
            if (active_job_ids_.contains(terminal_job.id))
                cleared_while_active_[terminal_job.id] = {terminal_job, delete_source};
            else
                owed = true; // the worker let go in between
        }
    }
    if (owed) {
        if (delete_source) cleanup_source(terminal_job);
        cleanup_partials(terminal_job);
    }
    Log::info("ingest cleared id=" + terminal_job.id +
              (delete_source ? " source_deleted=true" : " source_deleted=false"));
    return true;
}

CatalogueHintSummary IngestManager::catalogue_summary(std::string_view id) const {
    return hints_.summary("ingest", id);
}

bool IngestManager::delete_owned_source_on_clear() const {
    Lock lock(mutex_);
    return config_.delete_owned_source_on_clear;
}

bool IngestManager::delete_external_source_on_clear() const {
    Lock lock(mutex_);
    return config_.delete_external_source_on_clear;
}

bool IngestManager::delete_owned_source_on_cancel() const {
    Lock lock(mutex_);
    return config_.delete_owned_source_on_cancel;
}

void IngestManager::refresh_catalogue_jobs() {
    std::vector<std::string> ids;
    {
        Lock lock(mutex_);
        for (const auto& [id, job] : jobs_)
            if (job.state == IngestJobState::cataloguing) ids.push_back(id);
    }

    for (const auto& id : ids) {
        auto summary = hints_.summary("ingest", id);
        if (!summary.total) {
            IngestJob job;
            {
                Lock lock(mutex_);
                auto it = jobs_.find(id);
                if (it == jobs_.end() || it->second.state != IngestJobState::cataloguing) continue;
                job = it->second;
            }
            enqueue_catalogue_hints(job);
            summary = hints_.summary("ingest", id);
        }

        Lock lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.state != IngestJobState::cataloguing) continue;
        auto& job = it->second;
        const auto previous = std::tuple{job.catalogue_total, job.catalogue_pending,
                                         job.catalogue_catalogued, job.catalogue_no_match,
                                         job.catalogue_failed, job.state};
        job.catalogue_total = summary.total;
        job.catalogue_pending = summary.pending;
        job.catalogue_catalogued = summary.catalogued;
        job.catalogue_no_match = summary.no_match;
        job.catalogue_failed = summary.failed;
        if ((summary.total == 0 || summary.pending == 0) && job.files_completed == job.files_total) {
            job.state = IngestJobState::completed;
            job.current_file.clear();
            job.current_destination.clear();
            job.rate_bytes_per_second = 0;
            job.eta_seconds = 0;
            job.error.clear();
            job.error_code.clear();
            Log::info("ingest completed id=" + id + " files=" +
                      std::to_string(job.files_completed) + " catalogue_matched=" +
                      std::to_string(job.catalogue_catalogued) + " catalogue_no_match=" +
                      std::to_string(job.catalogue_no_match) + " catalogue_failed=" +
                      std::to_string(job.catalogue_failed));
        }
        const auto current = std::tuple{job.catalogue_total, job.catalogue_pending,
                                        job.catalogue_catalogued, job.catalogue_no_match,
                                        job.catalogue_failed, job.state};
        if (current != previous) {
            job.updated_unix_ms = now_ms();
            save_state_locked();
        }
    }
}

void IngestManager::enqueue_catalogue_hints(IngestJob& job) {
    for (const auto& file : job.files) {
        if (!file.completed || !file.catalogue_candidate || file.destination_path.empty()) continue;
        (void)hints_.submit(file.destination_path, "ingest", job.id, CatalogueHintPriority::ingest);
        if (media_information_)
            (void)media_information_->request_path(file.destination_path);
    }
}

std::string IngestManager::select_job_locked() const {
    const auto now = now_ms();
    for (const auto& [id, job] : jobs_) {
        // Claimed by another worker.
        if (active_job_ids_.contains(id)) continue;
        if (job.state == IngestJobState::queued) return id;
        if (job.state == IngestJobState::blocked &&
            now >= job.updated_unix_ms + static_cast<uint64_t>(config_.blocked_retry.count()))
            return id;
    }
    return {};
}

void IngestManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::string selected;
        {
            Lock lock(mutex_);
            selected = select_job_locked();
            if (selected.empty()) {
                std::optional<uint64_t> blocked_ready_ms;
                const auto now = now_ms();
                for (const auto& [_, job] : jobs_) {
                    if (job.state != IngestJobState::blocked) continue;
                    const auto ready = job.updated_unix_ms +
                        static_cast<uint64_t>(config_.blocked_retry.count());
                    if (!blocked_ready_ms || ready < *blocked_ready_ms) blocked_ready_ms = ready;
                }

                // Wake only for a job this worker could claim, not one another
                // worker already holds.
                auto claimable = [&]() MACHA_REQUIRES(mutex_) {
                    return !select_job_locked().empty();
                };

                if (blocked_ready_ms) {
                    const auto remaining_ms = *blocked_ready_ms > now ? *blocked_ready_ms - now : 0;
                    cv_.wait_for(lock.native(), stop, std::chrono::milliseconds(remaining_ms),
                                 claimable);
                } else {
                    // Only terminal or paused jobs: new work and API changes notify.
                    cv_.wait(lock.native(), stop, claimable);
                }
                continue;
            }
            // Claim under the lock that selected it, so no two workers take it.
            active_job_ids_.insert(selected);
            peak_active_jobs_ = std::max(peak_active_jobs_, active_job_ids_.size());
        }
        process_job(selected, stop);
        IngestJob after;
        bool have_after = false;
        std::optional<ClearedWhileActive> cleared;
        {
            Lock lock(mutex_);
            active_job_ids_.erase(selected);
            if (auto it = jobs_.find(selected); it != jobs_.end()) {
                after = it->second;
                have_after = true;
            }
            if (auto owed = cleared_while_active_.find(selected); owed != cleared_while_active_.end()) {
                cleared = std::move(owed->second);
                cleared_while_active_.erase(owed);
            }
        }
        if (cleared) {
            try {
                if (cleared->delete_source) cleanup_source(cleared->job);
                cleanup_partials(cleared->job);
            } catch (const std::exception& e) {
                Log::warn("ingest clear cleanup failed id=" + cleared->job.id + ": " + e.what());
            }
        }
        // A released claim can make a job selectable to a parked worker.
        cv_.notify_all();
        if (have_after && after.state == IngestJobState::cancelled) {
            cleanup_partials(after);
            if (after.source_owned && delete_owned_source_on_cancel()) {
                try { cleanup_source(after); }
                catch (const std::exception& e) {
                    Log::warn("ingest cancel source cleanup failed id=" + after.id + ": " + e.what());
                }
            }
        }
    }
}

void IngestManager::catalogue_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        refresh_catalogue_jobs();
        Lock lock(mutex_);
        auto cataloguing = [&]() MACHA_REQUIRES(mutex_) {
            return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& pair) {
                return pair.second.state == IngestJobState::cataloguing;
            });
        };
        if (cataloguing()) {
            // The hint queue records catalogue completion; poll only while a
            // copied job awaits it.
            cv_.wait_for(lock.native(), stop, std::chrono::milliseconds(500),
                         [&] { return stop.stop_requested(); });
        } else {
            cv_.wait(lock.native(), stop, cataloguing);
        }
    }
}

void IngestManager::process_job(const std::string& id, std::stop_token stop) {
    // The job's extent journal is cached only for the life of this call.
    struct ForgetExtentJournal {
        IngestManager& self;
        const std::string& id;
        ~ForgetExtentJournal() {
            Lock lock(self.extent_journals_mutex_);
            self.extent_journals_.erase(id);
        }
    } forget_extent_journal{*this, id};
    IngestJob job;
    {
        Lock lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        job = it->second;
        if (job.state == IngestJobState::paused || job.state == IngestJobState::cancelled ||
            job.state == IngestJobState::cataloguing)
            return;
    }
    const bool retrying_metadata =
        job.state == IngestJobState::blocked && job.error_code == "metadata_unavailable";

    try {
        if (job.files.empty() && !plan_job(job, stop)) return;
        if (stop.stop_requested()) return;
        if (!import_job(job, stop)) return;
        if (job.state == IngestJobState::cancelled || job.state == IngestJobState::paused ||
            job.state == IngestJobState::blocked)
            return;

        enqueue_catalogue_hints(job);
        const auto summary = hints_.summary("ingest", job.id);
        job.catalogue_total = summary.total;
        job.catalogue_pending = summary.pending;
        job.catalogue_catalogued = summary.catalogued;
        job.catalogue_no_match = summary.no_match;
        job.catalogue_failed = summary.failed;
        job.state = summary.pending ? IngestJobState::cataloguing : IngestJobState::completed;
        job.current_file.clear();
        job.current_destination.clear();
        job.rate_bytes_per_second = 0;
        job.eta_seconds = 0;
        job.error.clear();
        job.error_code.clear();
        job.updated_unix_ms = now_ms();
        // Paused or cancelled after its last copy: not finished.
        if (!record_progress(job))
            return;
        if (job.state == IngestJobState::cataloguing)
            Log::info("ingest copied id=" + id + " files=" + std::to_string(job.files_completed) +
                      " catalogue_pending=" + std::to_string(job.catalogue_pending));
        else
            Log::info("ingest completed id=" + id + " files=" + std::to_string(job.files_completed) +
                      " bytes=" + std::to_string(job.bytes_completed));
    } catch (const std::exception& e) {
        if (stop.stop_requested()) {
            job.updated_unix_ms = now_ms();
            Lock lock(mutex_);
            if (auto it = jobs_.find(id); it != jobs_.end()) {
                it->second = job;
                try { save_state_locked(); } catch (...) {}
            }
            return;
        }
        // Metadata that cannot be written now (no namespace yet, or the bytes
        // a commit brings in are held by no node present): the job blocks as
        // metadata_unavailable and retries after blocked_retry.
        if (dynamic_cast<const MetadataNotReady*>(&e)) {
            // Said once when the job blocks, not on every retry.
            const auto line = "ingest blocked id=" + id + ": " + e.what() + "; retrying";
            if (retrying_metadata)
                Log::debug(line);
            else
                Log::warn(line);
            try {
                set_blocked(job, "metadata_unavailable", e.what());
            } catch (const std::exception& save) {
                Log::warn("ingest state save failed id=" + id + ": " + save.what());
            }
            return;
        }
        job.state = IngestJobState::failed;
        job.error = e.what();
        if (const auto* failure = dynamic_cast<const IngestError*>(&e))
            job.error_code = failure->code();
        else if (dynamic_cast<const FsError*>(&e))
            job.error_code = "filesystem_error";
        else
            job.error_code = "import_failed";
        job.rate_bytes_per_second = 0;
        job.eta_seconds.reset();
        job.updated_unix_ms = now_ms();
        Lock lock(mutex_);
        auto it = jobs_.find(id);
        // A cancel stands over a failure met before the worker noticed it.
        if (it == jobs_.end() || it->second.state == IngestJobState::cancelled)
            return;
        it->second = job;
        try { save_state_locked(); } catch (...) {}
        Log::warn("ingest failed id=" + id + ": " + e.what());
    }
}

bool IngestManager::plan_job(IngestJob& job, std::stop_token stop) {
    std::error_code ec;
    if (!std::filesystem::exists(job.source_path, ec) || ec) {
        set_blocked(job, "source_unavailable", "source path is unavailable");
        return false;
    }
    job.state = IngestJobState::scanning;
    job.error.clear();
    job.error_code.clear();
    job.updated_unix_ms = now_ms();
    {
        Lock lock(mutex_);
        auto it = jobs_.find(job.id);
        if (it == jobs_.end() || it->second.state == IngestJobState::paused ||
            it->second.state == IngestJobState::cancelled)
            return false;
        it->second = job;
        save_state_locked();
    }

    std::vector<std::filesystem::path> host_files;
    bool scan_incomplete = false;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(job.source_path, ec)) && !ec) {
        throw IngestError("source_is_symlink", "ingest source cannot be a symbolic link");
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
                Lock lock(mutex_);
                auto it = jobs_.find(job.id);
                if (it == jobs_.end() || it->second.state == IngestJobState::paused ||
                    it->second.state == IngestJobState::cancelled)
                    return false;
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
        set_blocked(job, "source_not_regular", "source path is not a regular file or directory");
        return false;
    }

    if (scan_incomplete) {
        job.files.clear();
        job.bytes_total = job.bytes_completed = 0;
        job.files_total = job.files_completed = 0;
        set_blocked(job, "source_scan_interrupted", "source scan was interrupted; source may be unavailable");
        return false;
    }
    for (const auto& source : host_files) {
        if (!std::filesystem::exists(source, ec) || ec) {
            job.files.clear();
            job.bytes_total = job.bytes_completed = 0;
            job.files_total = job.files_completed = 0;
            set_blocked(job, "source_changed_during_scan", "source changed or disappeared during scan");
            return false;
        }
    }

    std::map<std::filesystem::path, std::vector<size_t>> media_by_parent;
    // A destination is taken if the filesystem has it or an earlier file of
    // this job was given it, ignoring case.
    std::set<std::string> planned_destinations;
    const auto taken = [&](const std::string& path) {
        if (planned_destinations.contains(fold_case(path))) return true;
        return exists_ignoring_case(path);
    };

    for (const auto& source : host_files) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(source, size_error);
        if (size_error || !size) continue;
        auto destination = choose_destination(source, size);
        if (destination.empty()) continue;
        destination = with_existing_case(destination);

        // Chosen once and persisted; resume never re-chooses.
        std::string candidate = destination;
        for (unsigned suffix = 2; taken(candidate); ++suffix)
            candidate = append_collision_suffix(destination, suffix);
        planned_destinations.insert(fold_case(candidate));

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

    // Sidecars are kept but not catalogued. Language subtitles and artwork
    // follow the nearest media file in their source directory; generic
    // folder/cover/poster art only when that directory maps to one destination.
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
        for (unsigned suffix = 2; taken(planned.destination_path); ++suffix)
            planned.destination_path = append_collision_suffix(base_destination, suffix);
        planned_destinations.insert(fold_case(planned.destination_path));
        planned.temporary_path = planned.destination_path + ".macha-ingest-" + job.id.substr(0, 12) + ".part";
        planned.size = size;
        planned.source_mtime_ns = host_mtime(source);
        planned.catalogue_candidate = false;
        job.files.push_back(std::move(planned));
        job.bytes_total += size;
    }

    job.files_total = job.files.size();
    if (job.files.empty()) throw IngestError("no_supported_media", "no supported media found in source");
    job.state = IngestJobState::queued;
    job.updated_unix_ms = now_ms();
    {
        Lock lock(mutex_);
        auto it = jobs_.find(job.id);
        if (it == jobs_.end() || it->second.state == IngestJobState::paused ||
            it->second.state == IngestJobState::cancelled)
            return false;
        it->second = job;
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

// The destination with each existing folder spelt as it already is, so a
// differently cased release joins the existing folder.
std::string IngestManager::with_existing_case(const std::string& destination) {
    const std::filesystem::path path(destination);
    std::vector<std::string> parts;
    for (const auto& part : path) {
        const auto component = part.string();
        if (!component.empty() && component != "/") parts.push_back(component);
    }
    if (parts.empty()) return destination;
    std::string current;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const auto parent = current.empty() ? std::string("/") : current;
        std::string chosen = parts[i];
        try {
            (void)fs_.getattr(current + "/" + parts[i]);
        } catch (const FsError& e) {
            if (e.code() != ENOENT) throw;
            try {
                const auto wanted = fold_case(parts[i]);
                for (const auto& [name, entry] : fs_.readdir(parent))
                    if (entry.type == EntryType::directory && fold_case(name) == wanted) {
                        chosen = name;
                        break;
                    }
            } catch (const FsError& missing) {
                if (missing.code() != ENOENT) throw;
            }
        }
        current += "/" + chosen;
    }
    return current + "/" + parts.back();
}

bool IngestManager::exists_ignoring_case(const std::string& path) {
    try {
        (void)fs_.getattr(path);
        return true;
    } catch (const FsError& e) {
        if (e.code() != ENOENT) throw;
    }
    const std::filesystem::path p(path);
    const auto wanted = fold_case(p.filename().string());
    try {
        for (const auto& [name, _] : fs_.readdir(p.parent_path().string()))
            if (fold_case(name) == wanted) return true;
    } catch (const FsError& e) {
        if (e.code() != ENOENT && e.code() != ENOTDIR) throw;
    }
    return false;
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
                throw IngestError("destination_parent_not_directory", "ingest destination parent is not a directory: " + current);
        } catch (const FsError& e) {
            if (e.code() != ENOENT) throw;
            const auto& policy = node_.config().filesystem;
            try {
                fs_.mkdir(current, 0755, policy.root_uid, policy.root_gid);
            } catch (const FsError& created) {
                // Another worker created it concurrently: EEXIST is success.
                if (created.code() != EEXIST) throw;
                const auto existing = fs_.getattr(current);
                if (existing.type != EntryType::directory)
                    throw IngestError("destination_parent_not_directory", "ingest destination parent is not a directory: " +
                                             current);
            }
        }
    }
}

bool IngestManager::copy_file(IngestJob& job, IngestFileProgress& file, std::stop_token stop) {
    if (file.completed) return true;
    const std::filesystem::path source(file.source_path);
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || ec) {
        set_blocked(job, "source_disappeared", "source disappeared while importing: " + source.string());
        return false;
    }
    const auto actual_size = std::filesystem::file_size(source, ec);
    if (ec || actual_size != file.size || host_mtime(source) != file.source_mtime_ns) {
        set_blocked(job, "source_changed", "source changed while importing: " + source.string());
        return false;
    }

    ensure_namespace_parents(file.destination_path);
    try {
        auto partial = fs_.getattr(file.temporary_path);
        if (partial.type != EntryType::file) throw IngestError("partial_not_file", "ingest partial is not a file");
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
                if (file.catalogue_candidate) {
                    (void)hints_.submit(file.destination_path, "ingest", job.id, CatalogueHintPriority::ingest);
                    if (media_information_)
                        (void)media_information_->request_path(file.destination_path);
                }
                refresh_progress(job);
                return true;
            }
            throw IngestError("destination_conflict", "ingest destination appeared with an unexpected type or size");
        } catch (const FsError& final_error) {
            if (final_error.code() != ENOENT) throw;
        }
        const auto& policy = node_.config().filesystem;
        fs_.create_file(file.temporary_path, 0644, policy.root_uid, policy.root_gid);
        file.copied = 0;
    }

    // A torrent usually published every extent as its pieces verified: commit
    // by naming them instead of copying. The retention barrier refuses a
    // manifest naming objects the cluster lacks, so a refusal costs a copy,
    // never data.
    if (file.copied == 0) {
        if (auto extents = published_extents(job, file)) {
            try {
                const auto partial = fs_.getattr(file.temporary_path);
                fs_.commit_file(file.temporary_path, partial, file.size, *extents, nullptr);
                fs_.rename(file.temporary_path, file.destination_path, true);
                file.copied = file.size;
                file.completed = true;
                Log::info("ingest adopted published extents path=" + file.destination_path +
                          " extents=" + std::to_string(extents->size()) +
                          " bytes=" + std::to_string(file.size));
                if (file.catalogue_candidate) {
                    (void)hints_.submit(file.destination_path, "ingest", job.id, CatalogueHintPriority::ingest);
                    if (media_information_)
                        (void)media_information_->request_path(file.destination_path);
                }
                ++job.files_completed;
                refresh_progress(job);
                return true;
            } catch (const std::exception& e) {
                Log::warn("ingest could not adopt published extents path=" + file.destination_path +
                          "; copying instead: " + e.what());
                try {
                    fs_.truncate_file(file.temporary_path, 0);
                } catch (const std::exception&) {
                }
            }
        }
    }

    std::ifstream input(source, std::ios::binary);
    if (!input) {
        set_blocked(job, "source_unreadable", "source is not readable: " + source.string());
        return false;
    }
    input.seekg(static_cast<std::streamoff>(file.copied));
    if (!input) {
        set_blocked(job, "source_seek_failed", "cannot seek source for resume: " + source.string());
        return false;
    }

    auto output = fs_.open_write(file.temporary_path, false, true);
    size_t copy_chunk_bytes = 0;
    {
        Lock lock(mutex_);
        copy_chunk_bytes = config_.copy_chunk_bytes;
    }
    std::vector<uint8_t> buffer(copy_chunk_bytes);
    uint64_t checkpoint_start = file.copied;
    auto sample_start = std::chrono::steady_clock::now();
    uint64_t sample_start_bytes = job.bytes_completed;

    while (file.copied < file.size && !stop.stop_requested()) {
        IngestJobState control = IngestJobState::importing;
        uint64_t checkpoint_bytes = 0;
        {
            Lock lock(mutex_);
            auto it = jobs_.find(job.id);
            if (it == jobs_.end()) return false;
            control = it->second.state;
            checkpoint_bytes = config_.checkpoint_bytes;
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
            Lock lock(mutex_);
            if (auto it = jobs_.find(job.id); it != jobs_.end()) {
                it->second = job;
                save_state_locked();
            }
            return false;
        }

        const auto wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), file.size - file.copied));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted));
        const auto got = static_cast<size_t>(input.gcount());
        if (!got) {
            output->commit();
            file.copied = output->size();
            set_blocked(job, "source_short_read", "short read from source: " + source.string());
            return false;
        }
        const auto written = output->write(file.copied, std::span<const uint8_t>(buffer.data(), got));
        if (written != got) throw IngestError("namespace_short_write", "short namespace write during ingest");
        file.copied += written;

        if (file.copied - checkpoint_start >= checkpoint_bytes || file.copied == file.size) {
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
            Lock lock(mutex_);
            auto it = jobs_.find(job.id);
            if (it == jobs_.end()) return false;
            if (it->second.state == IngestJobState::paused || it->second.state == IngestJobState::cancelled)
                job.state = it->second.state;
            it->second = job;
            save_state_locked();
            if (job.state != IngestJobState::importing) return false;
        }
    }
    if (stop.stop_requested()) {
        output->commit();
        file.copied = output->size();
        refresh_progress(job);
        job.updated_unix_ms = now_ms();
        Lock lock(mutex_);
        if (auto it = jobs_.find(job.id); it != jobs_.end()) {
            it->second = job;
            save_state_locked();
        }
        return false;
    }

    output->commit();
    file.copied = output->size();
    if (file.copied != file.size) throw IngestError("size_mismatch", "ingest committed size mismatch");
    fs_.rename(file.temporary_path, file.destination_path, true);
    file.completed = true;
    if (file.catalogue_candidate) {
        (void)hints_.submit(file.destination_path, "ingest", job.id, CatalogueHintPriority::ingest);
        if (media_information_)
            (void)media_information_->request_path(file.destination_path);
    }
    ++job.files_completed;
    refresh_progress(job);
    return true;
}

std::optional<std::vector<ExtentRef>> IngestManager::published_extents(
    const IngestJob& job, const IngestFileProgress& file) {
    std::error_code ec;
    if (!std::filesystem::is_directory(job.source_path, ec)) return std::nullopt;
    Lock lock(extent_journals_mutex_);
    auto found = extent_journals_.find(job.id);
    // Loaded once per run, released when process_job returns: a torrent's
    // ingest is submitted only once publication is done or stalled, so the
    // journal is final.
    if (found == extent_journals_.end())
        found = extent_journals_.emplace(job.id, TorrentExtentJournal::load(job.source_path)).first;
    if (found->second.empty()) return std::nullopt;
    // The source has a journal: a copy from here is a missed adoption and logs why.
    const auto relative =
        std::filesystem::path(file.source_path).lexically_relative(job.source_path).generic_string();
    const auto entry = found->second.find(relative);
    if (entry == found->second.end()) {
        Log::info("ingest copying path=" + file.destination_path + ": no published extents journalled");
        return std::nullopt;
    }
    auto manifest = TorrentExtentJournal::manifest(entry->second, file.size);
    if (!manifest) {
        uint64_t journalled = 0;
        for (const auto& [_, extent] : entry->second.extents) journalled += extent.length;
        Log::info("ingest copying path=" + file.destination_path + ": extent journal incomplete bytes=" +
                  std::to_string(journalled) + " of " + std::to_string(file.size) +
                  (entry->second.size != file.size ? " (journalled size " + std::to_string(entry->second.size) + ")"
                                                   : std::string{}));
    }
    return manifest;
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

void IngestManager::resolve_duplicate_destinations(IngestJob& job) {
    // Discipline 3: resolved, logged and persisted rather than failing every
    // retry. Completed files keep their paths; an unfinished file sharing one
    // gets the next free suffix and a fresh partial, as a shared partial
    // cannot be trusted.
    std::set<std::string> used;
    for (const auto& file : job.files)
        if (file.completed) used.insert(file.destination_path);
    for (auto& file : job.files) {
        if (file.completed) continue;
        if (!used.contains(file.destination_path)) {
            used.insert(file.destination_path);
            continue;
        }
        const auto base = file.destination_path;
        std::string candidate = base;
        for (unsigned suffix = 2;; ++suffix) {
            candidate = append_collision_suffix(base, suffix);
            if (used.contains(candidate)) continue;
            try {
                (void)fs_.getattr(candidate);
            } catch (const FsError& e) {
                if (e.code() == ENOENT) break;
                throw;
            }
        }
        Log::info("ingest destination shared within job id=" + job.id + " path=" + base +
                  "; planned " + candidate + " instead");
        file.destination_path = candidate;
        file.temporary_path = candidate + ".macha-ingest-" + job.id.substr(0, 12) + ".part";
        file.copied = 0;
        used.insert(candidate);
    }
}

bool IngestManager::import_job(IngestJob& job, std::stop_token stop) {
    resolve_duplicate_destinations(job);
    job.state = IngestJobState::importing;
    job.error.clear();
    job.error_code.clear();
    {
        Lock lock(mutex_);
        auto it = jobs_.find(job.id);
        if (it == jobs_.end() || it->second.state == IngestJobState::paused ||
            it->second.state == IngestJobState::cancelled)
            return false;
        it->second = job;
        save_state_locked();
    }

    for (auto& file : job.files) {
        if (stop.stop_requested()) return false;
        if (file.completed) continue;
        job.current_file = file.source_path;
        job.current_destination = file.destination_path;
        job.updated_unix_ms = now_ms();
        {
            Lock lock(mutex_);
            auto it = jobs_.find(job.id);
            if (it == jobs_.end() || it->second.state == IngestJobState::paused ||
                it->second.state == IngestJobState::cancelled)
                return false;
            it->second = job;
            save_state_locked();
        }
        if (!copy_file(job, file, stop)) return false;
        job.updated_unix_ms = now_ms();
        if (!record_progress(job)) return false;
    }
    return true;
}

bool IngestManager::record_progress(IngestJob& job) {
    Lock lock(mutex_);
    const auto it = jobs_.find(job.id);
    if (it == jobs_.end())
        return false;
    // A pause or cancel accepted since the worker last looked stands: what
    // the worker finished is kept under that state.
    const auto held = it->second.state;
    const bool overridden = held == IngestJobState::paused || held == IngestJobState::cancelled;
    if (overridden) {
        job.state = held;
        job.rate_bytes_per_second = 0;
        job.eta_seconds.reset();
    }
    it->second = job;
    save_state_locked();
    return !overridden;
}

void IngestManager::set_blocked(IngestJob& job, std::string code, std::string message) {
    job.state = IngestJobState::blocked;
    job.error_code = std::move(code);
    job.error = std::move(message);
    job.rate_bytes_per_second = 0;
    job.eta_seconds.reset();
    job.updated_unix_ms = now_ms();
    Lock lock(mutex_);
    if (auto it = jobs_.find(job.id); it != jobs_.end()) {
        it->second = job;
        save_state_locked();
    }
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
    Lock lock(mutex_);
    auto it = jobs_.find(job.id);
    return it == jobs_.end() || it->second.state == IngestJobState::paused ||
           it->second.state == IngestJobState::cancelled;
}

} // namespace macha
