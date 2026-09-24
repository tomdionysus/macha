// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_information.hpp"
#include "namespace_tree.hpp"

#include "diagnostics.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <stdexcept>

namespace macha {
namespace {

class InformationInput final : public MediaInput {
    std::shared_ptr<ReadHandle> handle_;
    uint64_t size_{};
  public:
    InformationInput(std::shared_ptr<ReadHandle> handle, uint64_t size)
        : handle_(std::move(handle)), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination,
                Clock::time_point deadline, std::atomic_bool* cancelled) override {
        if (offset >= size_) return 0;
        const auto wanted = static_cast<size_t>(
            std::min<uint64_t>(destination.size(), size_ - offset));
        return handle_->read(offset, destination.first(wanted), deadline, cancelled);
    }
};

class InformationPreempted final : public std::runtime_error {
  public:
    InformationPreempted() : std::runtime_error("media information scan yielded to playback") {}
};

bool pending(CatalogueHintState state) {
    return state == CatalogueHintState::queued || state == CatalogueHintState::processing ||
           state == CatalogueHintState::deferred;
}

size_t profile_weight(std::string_view media_id, const MediaProbeResult& probe) {
    size_t bytes = media_id.size() + sizeof(probe) + probe.format.capacity();
    for (const auto& stream : probe.streams)
        bytes += sizeof(stream) + stream.codec.capacity() + stream.profile.capacity() +
                 stream.language.capacity();
    return bytes;
}

} // namespace

struct MediaInformationService::Flight {
    std::mutex mutex;
    std::condition_variable cv;
    bool owner{};
    bool owner_foreground{};
    bool foreground_takeover{};
    bool complete{};
    std::optional<MediaProbeResult> result;
    std::exception_ptr error;
    std::shared_ptr<std::atomic_bool> cancelled;
};

MediaInformationService::MediaInformationService(
    FileSystem& fs, CatalogueManager& catalogue, std::shared_ptr<MediaEngine> engine,
    const std::filesystem::path& state_path, ProfilePublisher profile_publisher)
    : fs_(fs), catalogue_(catalogue), engine_(std::move(engine)),
      hints_(state_path / "media-information"),
      profile_publisher_(std::move(profile_publisher)) {}

MediaInformationService::~MediaInformationService() { stop(); }

void MediaInformationService::queue_publication_locked(std::string media_id,
                                                        MediaProbeResult probe) {
    const auto weight = profile_weight(media_id, probe);
    if (weight > max_pending_publication_bytes_) {
        flights_.erase(media_id);
        return;
    }
    if (auto found = pending_publications_.find(media_id);
        found != pending_publications_.end()) {
        pending_publication_bytes_ -= profile_weight(found->first, found->second);
        found->second = std::move(probe);
        pending_publication_bytes_ += weight;
        return;
    }
    while (!pending_publications_.empty() &&
           (pending_publications_.size() >= max_pending_publications_ ||
            pending_publication_bytes_ > max_pending_publication_bytes_ - weight)) {
        auto victim = pending_publications_.begin();
        pending_publication_bytes_ -= profile_weight(victim->first, victim->second);
        flights_.erase(victim->first);
        pending_publications_.erase(victim);
    }
    pending_publications_.emplace(std::move(media_id), std::move(probe));
    pending_publication_bytes_ += weight;
}

void MediaInformationService::start() {
    if (started_ || !engine_ || !engine_->status().available) return;
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised("media-information", [this, stop] { loop(stop); });
    });
    started_ = true;
}

void MediaInformationService::request_stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        cv_.notify_all();
    }
}

void MediaInformationService::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
    started_ = false;
}

std::optional<std::pair<std::string, FsEntry>>
MediaInformationService::source_for(std::string_view media_id) const {
    auto view = fs_.available_snapshot_view();
    if (!view) return {};
    // file_media_id hashes the extent list, so this walk needs whole entries.
    auto nodes = fs_.namespace_nodes();
    std::optional<std::pair<std::string, FsEntry>> found;
    for_each_namespace_entry(*view->snapshot, &nodes,
                             [&](const std::string& path, const FsEntry& entry) {
        if (found)
            return;
        if (entry.type == EntryType::file && entry.size && file_media_id(entry) == media_id)
            found = std::pair{path, entry};
    });
    return found;
}

bool MediaInformationService::media_is_live(std::string_view media_id) const {
    return source_for(media_id).has_value();
}

size_t MediaInformationService::request(const std::vector<std::string>& media_ids,
                                        int priority, std::string source) {
    if (media_ids.empty() || !engine_ || !engine_->status().available) return 0;
    std::set<std::string> wanted(media_ids.begin(), media_ids.end());
    for (auto it = wanted.begin(); it != wanted.end();) {
        if (catalogue_.media_profile(*it))
            it = wanted.erase(it);
        else
            ++it;
    }
    if (wanted.empty()) return 0;
    auto view = fs_.available_snapshot_view();
    if (!view) return 0;

    const auto existing = hints_.list();
    std::map<std::string, CatalogueHint, std::less<>> by_path;
    for (const auto& hint : existing) by_path.emplace(hint.path, hint);
    std::vector<CatalogueHintSubmission> submissions;
    size_t outstanding = 0;
    auto nodes = fs_.namespace_nodes();
    for_each_namespace_entry(*view->snapshot, &nodes,
                             [&](const std::string& path, const FsEntry& entry) {
        if (entry.type != EntryType::file || !entry.size) return;
        const auto media_id = file_media_id(entry);
        if (!wanted.contains(media_id)) return;
        if (auto found = by_path.find(path); found != by_path.end()) {
            const auto& hint = found->second;
            const bool same = std::any_of(hint.origins.begin(), hint.origins.end(),
                                          [&](const auto& origin) {
                return origin.source.starts_with("media-information") &&
                       origin.source_ref == media_id;
            });
            if (same) {
                if (pending(hint.state)) {
                    ++outstanding;
                    if (priority > hint.priority)
                        submissions.push_back({path, source, media_id, priority});
                }
                return;
            }
        }
        submissions.push_back({path, source, media_id, priority});
    });
    outstanding += hints_.submit_many(std::move(submissions)).size();
    cv_.notify_all();
    return outstanding;
}

bool MediaInformationService::request_path(std::string path, int priority,
                                           std::string source) {
    path = normalize_path(path);
    auto view = fs_.available_snapshot_view();
    if (!view) return false;
    auto nodes = fs_.namespace_nodes();
    auto found = namespace_entry(*view->snapshot, &nodes, path);
    if (!found || found->type != EntryType::file || !found->size)
        return false;
    const auto media_id = file_media_id(*found);
    if (catalogue_.media_profile(media_id)) return true;
    (void)hints_.submit(path, std::move(source), media_id, priority);
    cv_.notify_all();
    return true;
}

MediaProbeResult MediaInformationService::resolve(
    std::string media_id, std::string path, FsEntry entry, bool foreground,
    Clock::time_point deadline) {
    if (auto stored = catalogue_.media_profile(media_id)) return *stored;

    std::shared_ptr<Flight> flight;
    {
        std::lock_guard lock(mutex_);
        auto [it, _] = flights_.try_emplace(media_id, std::make_shared<Flight>());
        flight = it->second;
    }

    for (;;) {
        bool owner = false;
        {
            std::unique_lock lock(flight->mutex);
            if (flight->complete) {
                if (flight->error) std::rethrow_exception(flight->error);
                return *flight->result;
            }
            if (!flight->owner) {
                flight->owner = true;
                flight->owner_foreground = foreground;
                flight->foreground_takeover = false;
                flight->cancelled = std::make_shared<std::atomic_bool>(false);
                owner = true;
            } else if (foreground && !flight->owner_foreground) {
                flight->foreground_takeover = true;
                if (flight->cancelled) flight->cancelled->store(true);
                flight->cv.wait_until(lock, deadline, [&] {
                    return flight->complete || !flight->owner;
                });
                if (!flight->complete && flight->owner)
                    throw std::runtime_error("timed out taking over speculative media scan");
                continue;
            } else {
                if (!flight->cv.wait_until(lock, deadline, [&] { return flight->complete; }))
                    throw std::runtime_error("timed out waiting for concurrent media scan");
                if (flight->error) std::rethrow_exception(flight->error);
                return *flight->result;
            }
        }
        if (!owner) continue;

        try {
            const auto frame = foreground ? FrameType::foreground : FrameType::speculative;
            auto cancellation = flight->cancelled;
            MediaSource source{
                media_id, path, entry.size,
                [this, entry, path, frame](MediaReadPurpose) -> std::shared_ptr<MediaInput> {
                    return std::make_shared<InformationInput>(
                        fs_.open_read(entry, path, frame == FrameType::foreground, frame),
                        entry.size);
                },
                cancellation};
            auto remaining = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            auto result = engine_->probe(source, remaining);
            {
                std::lock_guard lock(flight->mutex);
                if (!foreground && flight->foreground_takeover)
                    throw InformationPreempted();
                flight->result = result;
                flight->complete = true;
                flight->owner = false;
            }
            flight->cv.notify_all();
            {
                std::lock_guard lock(mutex_);
                queue_publication_locked(media_id, result);
            }
            cv_.notify_all();
            return result;
        } catch (const InformationPreempted&) {
            {
                std::lock_guard lock(flight->mutex);
                flight->owner = false;
            }
            flight->cv.notify_all();
            throw;
        } catch (...) {
            const bool preempted = !foreground && flight->cancelled &&
                                   flight->cancelled->load() && flight->foreground_takeover;
            {
                std::lock_guard lock(flight->mutex);
                flight->owner = false;
                if (!preempted) {
                    flight->error = std::current_exception();
                    flight->complete = true;
                }
            }
            flight->cv.notify_all();
            if (preempted) throw InformationPreempted();
            {
                std::lock_guard lock(mutex_);
                auto it = flights_.find(media_id);
                if (it != flights_.end() && it->second == flight) flights_.erase(it);
            }
            throw;
        }
    }
}

MediaProbeResult MediaInformationService::resolve_playback(
    std::string media_id, std::string path, FsEntry entry, Clock::time_point deadline) {
    return resolve(std::move(media_id), std::move(path), std::move(entry), true, deadline);
}

void MediaInformationService::process_hint(const CatalogueHint& hint,
                                           std::stop_token stop) {
    auto origin = std::find_if(hint.origins.begin(), hint.origins.end(), [](const auto& value) {
        return value.source.starts_with("media-information");
    });
    if (origin == hint.origins.end()) {
        hints_.fail(hint.id, "no_immutable_identity", "media-information hint has no immutable identity");
        return;
    }
    const auto media_id = origin->source_ref;
    try {
        if (catalogue_.media_profile(media_id)) {
            hints_.mark_catalogued(hint.id, "media-information", media_id, {}, "already_stored");
            return;
        }
        auto source = source_for(media_id);
        if (!source) {
            hints_.mark_no_match(hint.id, "media-information", media_id,
                                 "media_not_live");
            request_prune();
            return;
        }
        auto deadline = Clock::now() + std::chrono::seconds(30);
        (void)resolve(media_id, source->first, source->second, false, deadline);
        hints_.mark_catalogued(hint.id, "media-information", media_id, {}, "profile_prepared");
    } catch (const InformationPreempted&) {
        hints_.defer(hint.id, "yielded_to_playback", "yielded to playback", 0);
    } catch (const std::exception& e) {
        if (!stop.stop_requested())
            hints_.record_failure(hint.id, "media_information_error", e.what(), 0, 3);
    }
}

void MediaInformationService::publish_one(std::string media_id, MediaProbeResult probe) {
    if (!media_is_live(media_id)) {
        request_prune();
        std::lock_guard lock(mutex_);
        flights_.erase(media_id);
        return;
    }
    if (profile_publisher_)
        profile_publisher_(media_id, std::move(probe));
    else
        catalogue_.put_media_profile(media_id, std::move(probe));
    std::lock_guard lock(mutex_);
    flights_.erase(media_id);
}

void MediaInformationService::request_prune() {
    {
        std::lock_guard lock(mutex_);
        prune_requested_ = true;
    }
    cv_.notify_all();
}

void MediaInformationService::prune() {
    auto view = fs_.available_snapshot_view();
    if (!view) return;
    std::set<std::string> live;
    auto nodes = fs_.namespace_nodes();
    for_each_namespace_entry(*view->snapshot, &nodes, [&](const std::string&, const FsEntry& entry) {
        if (entry.type == EntryType::file && entry.size)
            live.insert(file_media_id(entry));
    });
    const auto removed = catalogue_.prune_media_profiles(live);
    if (removed)
        Log::debug("media information pruned profiles=" + std::to_string(removed));
}

void MediaInformationService::loop(std::stop_token stop) {
    set_thread_name("macha-media-info");
    while (!stop.stop_requested()) {
        std::optional<std::pair<std::string, MediaProbeResult>> publication;
        bool do_prune = false;
        {
            std::lock_guard lock(mutex_);
            const auto now = Clock::now();
            if (!pending_publications_.empty() &&
                (!publication_retry_at_ || now >= *publication_retry_at_)) {
                auto it = pending_publications_.begin();
                publication = *it;
                pending_publication_bytes_ -= profile_weight(it->first, it->second);
                pending_publications_.erase(it);
                if (pending_publications_.empty()) publication_retry_at_.reset();
            } else if (prune_requested_) {
                prune_requested_ = false;
                do_prune = true;
            }
        }
        if (publication) {
            try {
                // Retain the completed probe until publication commits. A
                // catalogue CAS conflict must retry this result, not discard
                // it and force a second media scan.
                publish_one(publication->first, publication->second);
            } catch (const std::exception& e) {
                Log::warn("media information publication failed: " + std::string(e.what()));
                {
                    std::lock_guard lock(mutex_);
                    queue_publication_locked(publication->first, publication->second);
                    publication_retry_at_ = Clock::now() + publication_retry_delay_;
                }
                cv_.notify_all();
            }
            continue;
        }
        if (do_prune) {
            try { prune(); }
            catch (const std::exception& e) {
                Log::warn("media information prune deferred: " + std::string(e.what()));
            }
            continue;
        }
        if (auto hint = hints_.claim_next()) {
            process_hint(*hint, stop);
            continue;
        }
        std::unique_lock lock(mutex_);
        const auto ready_delay = hints_.next_ready_delay();
        const auto changed = [&] {
            return prune_requested_ ||
                   (!pending_publications_.empty() &&
                    (!publication_retry_at_ || Clock::now() >= *publication_retry_at_));
        };
        std::optional<std::chrono::milliseconds> wait_delay = ready_delay;
        if (publication_retry_at_) {
            const auto retry_delay = std::max(
                std::chrono::milliseconds(0),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *publication_retry_at_ - Clock::now()));
            if (!wait_delay || retry_delay < *wait_delay) wait_delay = retry_delay;
        }
        if (wait_delay)
            cv_.wait_for(lock, stop, *wait_delay, changed);
        else
            cv_.wait(lock, stop, changed);
    }
}

} // namespace macha
