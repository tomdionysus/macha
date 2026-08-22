// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue_hints.hpp"

#include "crypto.hpp"
#include "filesystem.hpp"
#include "json.hpp"
#include "log.hpp"
#include "types.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace macha {
namespace {

uint64_t now_ms() { return unix_ms(); }

std::string hint_id_for_path(std::string_view path) {
    const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(path.data()), path.size());
    auto digest = sha256(bytes);
    auto value = hex(digest.bytes);
    value.resize(32);
    return value;
}

Json origin_json(const CatalogueHintOrigin& origin) {
    Json::Object out;
    out["source"] = origin.source;
    out["source_ref"] = origin.source_ref;
    out["priority"] = static_cast<int64_t>(origin.priority);
    return Json(std::move(out));
}

Json hint_json(const CatalogueHint& hint) {
    Json::Object out;
    out["id"] = hint.id;
    out["path"] = hint.path;
    out["priority"] = static_cast<int64_t>(hint.priority);
    out["state"] = catalogue_hint_state_name(hint.state);
    out["attempts"] = static_cast<uint64_t>(hint.attempts);
    out["failures"] = static_cast<uint64_t>(hint.failures);
    out["candidate_cursor"] = static_cast<uint64_t>(hint.candidate_cursor);
    out["created_unix_ms"] = hint.created_unix_ms;
    out["updated_unix_ms"] = hint.updated_unix_ms;
    out["ready_after_unix_ms"] = hint.ready_after_unix_ms;
    out["provider"] = hint.provider;
    out["media_id"] = hint.media_id;
    out["result"] = hint.result;
    out["error"] = hint.error;
    Json::Array ids;
    for (const auto& id : hint.catalogue_item_ids) ids.emplace_back(id);
    out["catalogue_item_ids"] = std::move(ids);
    Json::Array origins;
    for (const auto& origin : hint.origins) origins.push_back(origin_json(origin));
    out["origins"] = std::move(origins);
    return Json(std::move(out));
}

std::string json_string(const Json& value, std::string_view key, std::string fallback = {}) {
    if (const auto* item = value.find(key)) return item->asString();
    return fallback;
}

uint64_t json_u64(const Json& value, std::string_view key, uint64_t fallback = 0) {
    if (const auto* item = value.find(key)) return item->asUInt64();
    return fallback;
}

std::string scheduling_lane(std::string_view path) {
    if (!path.empty() && path.front() == '/') path.remove_prefix(1);
    const auto slash = path.find('/');
    return std::string(path.substr(0, slash));
}

bool ephemeral_origin_source(std::string_view source) {
    return source == "scanner" || source == "namespace" || source == "manual";
}

int default_origin_priority(std::string_view source) {
    if (source == "ingest") return CatalogueHintPriority::ingest;
    if (source == "manual") return CatalogueHintPriority::manual_rescan;
    if (source == "namespace") return CatalogueHintPriority::namespace_mutation;
    if (source == "scanner") return CatalogueHintPriority::periodic_scan;
    return 0;
}

void recompute_priority(CatalogueHint& hint) {
    int priority = 0;
    for (const auto& origin : hint.origins) priority = std::max(priority, origin.priority);
    hint.priority = priority;
}

void discard_ephemeral_origins(CatalogueHint& hint) {
    std::erase_if(hint.origins, [](const auto& origin) {
        return ephemeral_origin_source(origin.source);
    });
    recompute_priority(hint);
}

CatalogueHint parse_hint(const Json& value) {
    CatalogueHint hint;
    hint.id = json_string(value, "id");
    hint.path = normalize_path(json_string(value, "path"));
    if (const auto* priority = value.find("priority")) hint.priority = static_cast<int>(priority->asInt64());
    if (auto state = parse_catalogue_hint_state(json_string(value, "state"))) hint.state = *state;
    hint.attempts = static_cast<unsigned>(json_u64(value, "attempts"));
    hint.failures = static_cast<unsigned>(json_u64(value, "failures"));
    hint.candidate_cursor = static_cast<size_t>(json_u64(value, "candidate_cursor"));
    hint.created_unix_ms = json_u64(value, "created_unix_ms");
    hint.updated_unix_ms = json_u64(value, "updated_unix_ms");
    hint.ready_after_unix_ms = json_u64(value, "ready_after_unix_ms");
    hint.provider = json_string(value, "provider");
    hint.media_id = json_string(value, "media_id");
    hint.result = json_string(value, "result");
    hint.error = json_string(value, "error");
    if (const auto* ids = value.find("catalogue_item_ids"))
        for (const auto& id : ids->asArray()) hint.catalogue_item_ids.push_back(id.asString());
    if (const auto* origins = value.find("origins")) {
        for (const auto& item : origins->asArray()) {
            CatalogueHintOrigin origin;
            origin.source = json_string(item, "source");
            origin.source_ref = json_string(item, "source_ref");
            if (const auto* priority = item.find("priority"))
                origin.priority = static_cast<int>(priority->asInt64());
            else
                origin.priority = default_origin_priority(origin.source);
            if (!origin.source.empty()) hint.origins.push_back(std::move(origin));
        }
    }
    if (!hint.origins.empty()) recompute_priority(hint);
    return hint;
}

} // namespace

std::string catalogue_hint_state_name(CatalogueHintState state) {
    switch (state) {
    case CatalogueHintState::queued: return "queued";
    case CatalogueHintState::processing: return "processing";
    case CatalogueHintState::deferred: return "deferred";
    case CatalogueHintState::catalogued: return "catalogued";
    case CatalogueHintState::no_match: return "no_match";
    case CatalogueHintState::failed: return "failed";
    }
    return "failed";
}

std::optional<CatalogueHintState> parse_catalogue_hint_state(std::string_view state) {
    if (state == "queued") return CatalogueHintState::queued;
    if (state == "processing") return CatalogueHintState::processing;
    if (state == "deferred") return CatalogueHintState::deferred;
    if (state == "catalogued") return CatalogueHintState::catalogued;
    if (state == "no_match") return CatalogueHintState::no_match;
    if (state == "failed") return CatalogueHintState::failed;
    return {};
}

bool CatalogueHintQueue::terminal(CatalogueHintState state) noexcept {
    return state == CatalogueHintState::catalogued || state == CatalogueHintState::no_match ||
           state == CatalogueHintState::failed;
}

bool CatalogueHintQueue::has_origin(const CatalogueHint& hint, std::string_view source,
                                    std::string_view source_ref) {
    return std::any_of(hint.origins.begin(), hint.origins.end(), [&](const auto& origin) {
        return origin.source == source && origin.source_ref == source_ref;
    });
}

void CatalogueHintQueue::changed_locked() {
    ++revision_;
    change_cv_.notify_all();
}

CatalogueHintQueue::CatalogueHintQueue(const std::filesystem::path& state_path)
    : state_file_(state_path / "catalogue" / "hints.json") {
    std::filesystem::create_directories(state_file_.parent_path());
    load_state();
    requeue_processing();
}

void CatalogueHintQueue::load_state() {
    std::lock_guard lock(mutex_);
    std::ifstream in(state_file_, std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    try {
        auto root = Json::parse(text.str());
        const auto* values = root.find("hints");
        if (!values) return;
        for (const auto& value : values->asArray()) {
            auto hint = parse_hint(value);
            if (hint.path.empty()) continue;
            if (hint.id.empty()) hint.id = hint_id_for_path(hint.path);
            hints_[hint.path] = std::move(hint);
        }
    } catch (const std::exception& e) {
        Log::warn("catalogue hint state ignored: " + std::string(e.what()));
    }
}

void CatalogueHintQueue::save_state_locked() const {
    Json::Array hints;
    hints.reserve(hints_.size());
    for (const auto& [_, hint] : hints_) hints.push_back(hint_json(hint));
    Json::Object root;
    root["version"] = static_cast<uint64_t>(2);
    root["hints"] = std::move(hints);
    const auto text = Json(std::move(root)).dump();
    const auto temp = state_file_.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write catalogue hint state " + temp);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) throw std::runtime_error("cannot flush catalogue hint state " + temp);
    }
    std::error_code ec;
    std::filesystem::rename(temp, state_file_, ec);
    if (ec) {
        std::filesystem::remove(state_file_, ec);
        ec.clear();
        std::filesystem::rename(temp, state_file_, ec);
    }
    if (ec) throw std::runtime_error("cannot replace catalogue hint state: " + ec.message());
}

std::vector<std::string> CatalogueHintQueue::submit_many(
    std::vector<CatalogueHintSubmission> submissions) {
    const auto now = now_ms();
    std::vector<std::string> ids;
    ids.reserve(submissions.size());
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (auto& submission : submissions) {
        auto path = normalize_path(std::move(submission.path));
        if (path.empty() || path == "/" || submission.source.empty()) continue;

        auto [it, inserted] = hints_.try_emplace(path);
        auto& hint = it->second;
        const int previous_priority = hint.priority;
        const bool was_terminal = !inserted && terminal(hint.state);
        const bool ephemeral = ephemeral_origin_source(submission.source);

        auto exact_origin = hint.origins.end();
        auto source_origin = hint.origins.end();
        if (!inserted) {
            exact_origin = std::find_if(hint.origins.begin(), hint.origins.end(),
                                        [&](const auto& origin) {
                                            return origin.source == submission.source &&
                                                   origin.source_ref == submission.source_ref;
                                        });
            source_origin = std::find_if(hint.origins.begin(), hint.origins.end(),
                                         [&](const auto& origin) {
                                             return origin.source == submission.source;
                                         });
        }
        const bool origin_known = exact_origin != hint.origins.end();
        const bool source_known = source_origin != hint.origins.end();
        const bool source_ref_changed = source_known && !origin_known &&
                                        source_origin->source_ref != submission.source_ref;

        if (inserted) {
            hint.id = hint_id_for_path(path);
            hint.path = path;
            hint.created_unix_ms = now;
        }

        // Terminal work is a path-level result cache as well as provenance.
        // Periodic/mutation rediscovery of the same media content must not turn
        // into repeated provider requests. The scanner uses the stable media id
        // as source_ref, so a changed file at the same path reopens the item.
        bool reopen_terminal = false;
        if (was_terminal) {
            if (submission.source == "manual") {
                reopen_terminal = true;
            } else if (submission.source == "scanner" || submission.source == "namespace") {
                if (!hint.media_id.empty() && !submission.source_ref.empty())
                    reopen_terminal = submission.source_ref != hint.media_id;
                else
                    reopen_terminal = source_ref_changed;
            } else {
                // Ingest and future explicit producers represent occurrences,
                // not observations. A new source_ref is therefore new work.
                reopen_terminal = !origin_known;
            }
        }

        // Do not attach passive ephemeral observations to a terminal result;
        // otherwise merely scanning an ingest result would keep the hint record
        // alive after the ingest job is cleared. Negative scanner results retain
        // their own origin when they are produced, so unchanged no-match paths
        // still remain deduplicated across scans.
        const bool passive_terminal_observation = was_terminal && ephemeral && !reopen_terminal;
        if (!passive_terminal_observation) {
            if (source_known && ephemeral && !origin_known) {
                source_origin->source_ref = submission.source_ref;
                source_origin->priority = std::max(source_origin->priority, submission.priority);
            } else if (origin_known) {
                exact_origin->priority = std::max(exact_origin->priority, submission.priority);
            } else {
                hint.origins.push_back({submission.source, submission.source_ref, submission.priority});
            }
            recompute_priority(hint);
        }

        if (inserted || reopen_terminal) {
            hint.state = CatalogueHintState::queued;
            hint.attempts = 0;
            hint.failures = 0;
            hint.candidate_cursor = 0;
            hint.ready_after_unix_ms = 0;
            hint.provider.clear();
            hint.media_id.clear();
            hint.catalogue_item_ids.clear();
            hint.result.clear();
            hint.error.clear();
        } else if (hint.state == CatalogueHintState::deferred &&
                   submission.priority > previous_priority) {
            // A stronger producer (for example ingest over a periodic scan)
            // makes a deferred path immediately eligible again.
            hint.state = CatalogueHintState::queued;
            hint.ready_after_unix_ms = 0;
        }

        hint.updated_unix_ms = now;
        ids.push_back(hint.id);
        changed = true;
    }
    if (changed) {
        save_state_locked();
        changed_locked();
    }
    return ids;
}

std::string CatalogueHintQueue::submit(std::string path, std::string source,
                                       std::string source_ref, int priority) {
    auto ids = submit_many({CatalogueHintSubmission{std::move(path), std::move(source),
                                                     std::move(source_ref), priority}});
    if (ids.empty()) throw std::runtime_error("catalogue hint submission is invalid");
    return ids.front();
}

std::optional<CatalogueHint> CatalogueHintQueue::claim_next() {
    const auto now = now_ms();
    std::lock_guard lock(mutex_);
    auto best = hints_.end();
    uint64_t best_lane_served = UINT64_MAX;
    for (auto it = hints_.begin(); it != hints_.end(); ++it) {
        auto& hint = it->second;
        if (hint.state != CatalogueHintState::queued && hint.state != CatalogueHintState::deferred)
            continue;
        if (hint.ready_after_unix_ms > now) continue;
        const auto lane = scheduling_lane(hint.path);
        const auto lane_it = lane_served_.find(lane);
        const auto served = lane_it == lane_served_.end() ? 0 : lane_it->second;
        if (best == hints_.end() || hint.priority > best->second.priority ||
            (hint.priority == best->second.priority && served < best_lane_served) ||
            (hint.priority == best->second.priority && served == best_lane_served &&
             hint.ready_after_unix_ms < best->second.ready_after_unix_ms) ||
            (hint.priority == best->second.priority && served == best_lane_served &&
             hint.ready_after_unix_ms == best->second.ready_after_unix_ms &&
             hint.created_unix_ms < best->second.created_unix_ms)) {
            best = it;
            best_lane_served = served;
        }
    }
    if (best == hints_.end()) return {};
    best->second.state = CatalogueHintState::processing;
    ++best->second.attempts;
    best->second.updated_unix_ms = now;
    lane_served_[scheduling_lane(best->second.path)] = ++schedule_sequence_;
    save_state_locked();
    return best->second;
}

std::optional<std::chrono::milliseconds> CatalogueHintQueue::next_ready_delay() const {
    const auto now = now_ms();
    std::lock_guard lock(mutex_);
    std::optional<uint64_t> earliest;
    for (const auto& [_, hint] : hints_) {
        if (hint.state != CatalogueHintState::queued && hint.state != CatalogueHintState::deferred)
            continue;
        if (hint.ready_after_unix_ms <= now)
            return std::chrono::milliseconds(0);
        if (!earliest || hint.ready_after_unix_ms < *earliest)
            earliest = hint.ready_after_unix_ms;
    }
    if (!earliest) return {};
    return std::chrono::milliseconds(*earliest - now);
}

uint64_t CatalogueHintQueue::revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

bool CatalogueHintQueue::wait_for_change(std::stop_token stop, uint64_t observed_revision,
                                         std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return change_cv_.wait_for(lock, stop, timeout, [&] {
        return revision_ != observed_revision;
    });
}

void CatalogueHintQueue::mark_catalogued(std::string_view id, std::string provider,
                                         std::string media_id,
                                         std::vector<std::string> catalogue_item_ids,
                                         std::string result) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(), [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return;
    auto& hint = it->second;
    hint.state = CatalogueHintState::catalogued;
    hint.failures = 0;
    hint.candidate_cursor = 0;
    hint.provider = std::move(provider);
    hint.media_id = std::move(media_id);
    hint.catalogue_item_ids = std::move(catalogue_item_ids);
    hint.result = std::move(result);
    hint.error.clear();
    hint.ready_after_unix_ms = 0;
    hint.updated_unix_ms = now_ms();
    discard_ephemeral_origins(hint);
    if (hint.origins.empty()) hints_.erase(it);
    save_state_locked();
    changed_locked();
}

void CatalogueHintQueue::mark_no_match(std::string_view id, std::string provider,
                                       std::string media_id, std::string result) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(), [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return;
    auto& hint = it->second;
    hint.state = CatalogueHintState::no_match;
    hint.failures = 0;
    hint.candidate_cursor = 0;
    hint.provider = std::move(provider);
    hint.media_id = std::move(media_id);
    hint.catalogue_item_ids.clear();
    hint.result = std::move(result);
    hint.error.clear();
    hint.ready_after_unix_ms = 0;
    hint.updated_unix_ms = now_ms();
    if (hint.origins.empty()) hints_.erase(it);
    save_state_locked();
    changed_locked();
}

void CatalogueHintQueue::advance_candidate(std::string_view id, size_t next_cursor) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(), [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return;
    auto& hint = it->second;
    hint.state = CatalogueHintState::queued;
    hint.attempts = 0;
    hint.failures = 0;
    hint.candidate_cursor = next_cursor;
    hint.error.clear();
    hint.ready_after_unix_ms = 0;
    hint.updated_unix_ms = now_ms();
    save_state_locked();
    changed_locked();
}

void CatalogueHintQueue::defer(std::string_view id, std::string error, uint64_t retry_after_unix_ms) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(), [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return;
    auto& hint = it->second;
    hint.state = CatalogueHintState::deferred;
    hint.error = std::move(error);
    hint.ready_after_unix_ms = retry_after_unix_ms;
    hint.updated_unix_ms = now_ms();
    save_state_locked();
    changed_locked();
}

size_t CatalogueHintQueue::defer_matching(
    const std::function<bool(const CatalogueHint&)>& predicate,
    std::string error, uint64_t retry_after_unix_ms) {
    const auto now = now_ms();
    std::lock_guard lock(mutex_);
    size_t deferred = 0;
    for (auto& [_, hint] : hints_) {
        if (hint.state != CatalogueHintState::queued &&
            hint.state != CatalogueHintState::deferred &&
            hint.state != CatalogueHintState::processing)
            continue;
        if (!predicate(hint))
            continue;
        hint.state = CatalogueHintState::deferred;
        hint.error = error;
        hint.ready_after_unix_ms = std::max(hint.ready_after_unix_ms, retry_after_unix_ms);
        hint.updated_unix_ms = now;
        ++deferred;
    }
    if (deferred) {
        // Provider outage is one scheduling event, not N independent hint
        // failures. Persist and wake once for the complete affected provider set.
        save_state_locked();
        changed_locked();
    }
    return deferred;
}

bool CatalogueHintQueue::record_failure(std::string_view id, std::string error,
                                        uint64_t retry_after_unix_ms,
                                        unsigned max_failures) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(),
                           [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return false;
    auto& hint = it->second;
    ++hint.failures;
    hint.error = std::move(error);
    hint.updated_unix_ms = now_ms();
    const bool gave_up = max_failures != 0 && hint.failures >= max_failures;
    if (gave_up) {
        hint.state = CatalogueHintState::failed;
        hint.candidate_cursor = 0;
        hint.ready_after_unix_ms = 0;
        Log::warn("catalogue hint gave up path=" + hint.path +
                  " failures=" + std::to_string(hint.failures) +
                  ": " + hint.error);
    } else {
        hint.state = CatalogueHintState::deferred;
        hint.ready_after_unix_ms = retry_after_unix_ms;
    }
    save_state_locked();
    changed_locked();
    return gave_up;
}

void CatalogueHintQueue::fail(std::string_view id, std::string error) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(), [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return;
    auto& hint = it->second;
    hint.state = CatalogueHintState::failed;
    hint.failures = std::max(1u, hint.failures);
    hint.candidate_cursor = 0;
    hint.error = std::move(error);
    hint.ready_after_unix_ms = 0;
    hint.updated_unix_ms = now_ms();
    // Failed hints are the persistent dead-letter/give-up list. Explicit origin
    // removal may still erase them later, but failure itself must remain visible.
    save_state_locked();
    changed_locked();
}

void CatalogueHintQueue::requeue_processing() {
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (auto& [_, hint] : hints_) {
        if (hint.state != CatalogueHintState::processing) continue;
        hint.state = CatalogueHintState::queued;
        hint.ready_after_unix_ms = 0;
        hint.updated_unix_ms = now_ms();
        changed = true;
    }
    if (changed) { save_state_locked(); changed_locked(); }
}

std::vector<CatalogueHint> CatalogueHintQueue::list() const {
    std::lock_guard lock(mutex_);
    std::vector<CatalogueHint> out;
    out.reserve(hints_.size());
    for (const auto& [_, hint] : hints_) out.push_back(hint);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.created_unix_ms < b.created_unix_ms;
    });
    return out;
}

std::optional<CatalogueHint> CatalogueHintQueue::get(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(hints_.begin(), hints_.end(), [&](const auto& pair) { return pair.second.id == id; });
    if (it == hints_.end()) return {};
    return it->second;
}

CatalogueHintSummary CatalogueHintQueue::summary(std::string_view source,
                                                 std::string_view source_ref) const {
    CatalogueHintSummary out;
    std::lock_guard lock(mutex_);
    for (const auto& [_, hint] : hints_) {
        if (!has_origin(hint, source, source_ref)) continue;
        ++out.total;
        out.hints.push_back(hint);
        switch (hint.state) {
        case CatalogueHintState::catalogued: ++out.catalogued; break;
        case CatalogueHintState::no_match: ++out.no_match; break;
        case CatalogueHintState::failed: ++out.failed; break;
        case CatalogueHintState::queued:
        case CatalogueHintState::processing:
        case CatalogueHintState::deferred: ++out.pending; break;
        }
    }
    return out;
}

size_t CatalogueHintQueue::erase_origin(std::string_view source, std::string_view source_ref) {
    std::lock_guard lock(mutex_);
    size_t removed = 0;
    for (auto it = hints_.begin(); it != hints_.end();) {
        auto& origins = it->second.origins;
        const auto before = origins.size();
        std::erase_if(origins, [&](const auto& origin) {
            return origin.source == source && origin.source_ref == source_ref;
        });
        removed += before - origins.size();
        recompute_priority(it->second);
        if (origins.empty() && terminal(it->second.state)) it = hints_.erase(it);
        else ++it;
    }
    if (removed) { save_state_locked(); changed_locked(); }
    return removed;
}

} // namespace macha
