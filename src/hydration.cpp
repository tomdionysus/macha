// SPDX-License-Identifier: GPL-3.0-or-later
#include "hydration.hpp"
#include "diagnostics.hpp"

#include "catalogue.hpp"
#include "distributed_store.hpp"
#include "filesystem.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <tuple>

namespace macha {
namespace {

std::string playback_run(uint64_t session) {
    return "playback:" + std::to_string(session);
}

std::vector<ObjectId> extent_objects(const FsEntry& entry, size_t first, size_t maximum = 0) {
    std::vector<ObjectId> out;
    for (size_t i = first; i < entry.extents.size(); ++i) {
        const auto& extent = entry.extents[i];
        if (extent.hole)
            continue;
        out.push_back(extent.id);
        if (maximum && out.size() >= maximum)
            break;
    }
    return out;
}

const CatalogueItem* find_current_catalogue_item(const CatalogueSnapshot& snapshot,
                                                  const PlaybackObservation& observation) {
    const auto stable = file_media_id(observation.entry);
    const auto path_id = "path:" + observation.path;
    for (const auto& [_, item] : snapshot.items) {
        if (std::find(item.media_ids.begin(), item.media_ids.end(), stable) != item.media_ids.end() ||
            std::find(item.media_ids.begin(), item.media_ids.end(), path_id) != item.media_ids.end() ||
            std::find(item.media_ids.begin(), item.media_ids.end(), observation.path) != item.media_ids.end())
            return &item;
    }
    return nullptr;
}

const CatalogueItem* next_episode(const CatalogueSnapshot& snapshot, const CatalogueItem& current) {
    if (current.kind != CatalogueKind::episode)
        return nullptr;

    const CatalogueItem* season = nullptr;
    if (current.parent_id) {
        auto it = snapshot.items.find(*current.parent_id);
        if (it != snapshot.items.end() && it->second.kind == CatalogueKind::season)
            season = &it->second;
    }

    std::vector<const CatalogueItem*> same_season;
    for (const auto& [_, candidate] : snapshot.items) {
        if (candidate.kind != CatalogueKind::episode)
            continue;
        if (season) {
            if (candidate.parent_id && *candidate.parent_id == season->id)
                same_season.push_back(&candidate);
        } else if (candidate.parent_id == current.parent_id &&
                   candidate.season_number == current.season_number) {
            same_season.push_back(&candidate);
        }
    }
    auto episode_order = [](const CatalogueItem* a, const CatalogueItem* b) {
        return std::tie(a->episode_number, a->title, a->id) <
               std::tie(b->episode_number, b->title, b->id);
    };
    std::sort(same_season.begin(), same_season.end(), episode_order);
    auto here = std::find_if(same_season.begin(), same_season.end(),
                             [&](const auto* item) { return item->id == current.id; });
    if (here != same_season.end() && ++here != same_season.end())
        return *here;

    if (!season || !season->parent_id)
        return nullptr;

    std::vector<const CatalogueItem*> seasons;
    for (const auto& [_, candidate] : snapshot.items) {
        if (candidate.kind == CatalogueKind::season && candidate.parent_id == season->parent_id)
            seasons.push_back(&candidate);
    }
    std::sort(seasons.begin(), seasons.end(), [](const CatalogueItem* a, const CatalogueItem* b) {
        return std::tie(a->season_number, a->title, a->id) <
               std::tie(b->season_number, b->title, b->id);
    });
    auto current_season = std::find_if(seasons.begin(), seasons.end(),
                                       [&](const auto* item) { return item->id == season->id; });
    if (current_season == seasons.end())
        return nullptr;
    for (++current_season; current_season != seasons.end(); ++current_season) {
        std::vector<const CatalogueItem*> episodes;
        for (const auto& [_, candidate] : snapshot.items) {
            if (candidate.kind == CatalogueKind::episode && candidate.parent_id &&
                *candidate.parent_id == (*current_season)->id)
                episodes.push_back(&candidate);
        }
        if (!episodes.empty()) {
            std::sort(episodes.begin(), episodes.end(), episode_order);
            return episodes.front();
        }
    }
    return nullptr;
}

const CatalogueItem* next_movie(const CatalogueSnapshot& snapshot, const CatalogueItem& current) {
    if (current.kind != CatalogueKind::movie)
        return nullptr;

    std::optional<std::pair<std::string, std::string>> collection;
    for (const auto* key : {"collection", "tmdb_collection"}) {
        auto it = current.external_ids.find(key);
        if (it != current.external_ids.end() && !it->second.empty()) {
            collection = std::pair{std::string(key), it->second};
            break;
        }
    }
    if (!current.parent_id && !collection)
        return nullptr;

    std::vector<const CatalogueItem*> movies;
    for (const auto& [_, candidate] : snapshot.items) {
        if (candidate.kind != CatalogueKind::movie)
            continue;
        bool same = current.parent_id && candidate.parent_id == current.parent_id;
        if (!same && collection) {
            auto it = candidate.external_ids.find(collection->first);
            same = it != candidate.external_ids.end() && it->second == collection->second;
        }
        if (same)
            movies.push_back(&candidate);
    }
    std::sort(movies.begin(), movies.end(), [](const CatalogueItem* a, const CatalogueItem* b) {
        return std::tie(a->year, a->sort_title, a->title, a->id) <
               std::tie(b->year, b->sort_title, b->title, b->id);
    });
    auto here = std::find_if(movies.begin(), movies.end(),
                             [&](const auto* item) { return item->id == current.id; });
    if (here != movies.end() && ++here != movies.end())
        return *here;
    return nullptr;
}

const CatalogueItem* next_catalogue_item(const CatalogueSnapshot& snapshot,
                                         const CatalogueItem& current) {
    if (current.kind == CatalogueKind::episode)
        return next_episode(snapshot, current);
    if (current.kind == CatalogueKind::movie)
        return next_movie(snapshot, current);
    return nullptr;
}

} // namespace

uint64_t PlaybackTracker::open(std::string path, const FsEntry& entry) {
    std::lock_guard lock(mutex_);
    const auto session = next_session_++;
    sessions_.emplace(session, PlaybackObservation{session, std::move(path), entry, {}, Clock::now()});
    return session;
}

void PlaybackTracker::progress(uint64_t session, size_t extent_index) {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(session);
    if (it == sessions_.end())
        return;
    it->second.current_extent = extent_index;
    it->second.last_activity = Clock::now();
}

void PlaybackTracker::close(uint64_t session) {
    std::lock_guard lock(mutex_);
    sessions_.erase(session);
}

std::vector<PlaybackObservation> PlaybackTracker::active(std::chrono::milliseconds timeout) const {
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    std::vector<PlaybackObservation> out;
    for (const auto& [_, observation] : sessions_) {
        if (observation.current_extent && now - observation.last_activity <= timeout)
            out.push_back(observation);
    }
    return out;
}

void HydrationScheduler::reset() {
    virtual_finish_.clear();
}

std::optional<HydrationRequest>
HydrationScheduler::next(const std::vector<HydrationHint>& hints,
                         const std::function<bool(const ObjectId&)>& present,
                         const std::function<bool(const ObjectId&)>& blocked) {
    struct Candidate {
        ObjectId object{};
        uint64_t priority{};
        uint32_t reason_priority{};
        std::string reason;
        FrameType frame_type{FrameType::speculative};
    };
    struct Run {
        std::vector<Candidate> objects;
        std::map<ObjectId, size_t> positions;
    };

    std::map<std::string, Run> runs;
    for (const auto& hint : hints) {
        if (hint.run_id.empty() || hint.objects.empty() || !hint.priority)
            continue;
        auto& run = runs[hint.run_id];
        for (const auto& object : hint.objects) {
            auto found = run.positions.find(object);
            size_t position = 0;
            if (found == run.positions.end()) {
                position = run.objects.size();
                run.positions.emplace(object, position);
                run.objects.push_back({object, 0, 0, {}, hint.frame_type});
            } else {
                position = found->second;
            }
            auto& candidate = run.objects[position];
            candidate.priority = std::min<uint64_t>(
                std::numeric_limits<uint32_t>::max(), candidate.priority + hint.priority);
            if (hint.priority >= candidate.reason_priority) {
                candidate.reason_priority = hint.priority;
                candidate.reason = hint.reason;
            }
            if (frame_type_priority(hint.frame_type) <
                frame_type_priority(candidate.frame_type))
                candidate.frame_type = hint.frame_type;
        }
    }

    std::set<std::string> active;
    struct Ready {
        std::string run_id;
        size_t sequence_index{};
        Candidate candidate;
        double finish{};
    };
    std::vector<Ready> ready;

    double base = 0.0;
    bool have_base = false;
    for (const auto& [run_id, _] : runs) {
        auto it = virtual_finish_.find(run_id);
        if (it != virtual_finish_.end() && (!have_base || it->second < base)) {
            base = it->second;
            have_base = true;
        }
    }

    for (const auto& [run_id, run] : runs) {
        std::optional<std::pair<size_t, Candidate>> first_missing;
        for (size_t i = 0; i < run.objects.size(); ++i) {
            const auto& candidate = run.objects[i];
            if (present(candidate.object))
                continue;
            // Ordered runs do not jump over a temporarily unavailable prefix.
            if (blocked && blocked(candidate.object))
                break;
            first_missing = std::pair{i, candidate};
            break;
        }
        if (!first_missing)
            continue;
        active.insert(run_id);
        auto [state, inserted] = virtual_finish_.try_emplace(run_id, have_base ? base : 0.0);
        (void)inserted;
        const auto priority = std::max<uint64_t>(1, first_missing->second.priority);
        const double finish = state->second + 1000000.0 / static_cast<double>(priority);
        ready.push_back({run_id, first_missing->first, first_missing->second, finish});
    }

    std::erase_if(virtual_finish_, [&](const auto& item) { return !active.contains(item.first); });
    if (ready.empty())
        return {};

    auto best = std::min_element(ready.begin(), ready.end(), [](const Ready& a, const Ready& b) {
        if (a.finish != b.finish)
            return a.finish < b.finish;
        return a.run_id < b.run_id;
    });
    virtual_finish_[best->run_id] = best->finish;
    return HydrationRequest{best->run_id,
                            best->candidate.object,
                            best->sequence_index,
                            static_cast<uint32_t>(best->candidate.priority),
                            best->candidate.reason,
                            best->candidate.frame_type};
}

ReadAheadHintProvider::ReadAheadHintProvider(PlaybackTracker& playback,
                                             const HydrationConfig& config, size_t window)
    : playback_(playback) {
    reconfigure(config, window);
}

void ReadAheadHintProvider::reconfigure(const HydrationConfig& config, size_t window) {
    enabled_.store(config.read_ahead.enabled);
    priority_.store(config.read_ahead.priority);
    window_.store(window);
    timeout_ms_.store(config.active_timeout.count());
}

std::vector<HydrationHint> ReadAheadHintProvider::hints() {
    if (!enabled_.load() || !window_.load() || !priority_.load())
        return {};
    std::vector<HydrationHint> out;
    for (const auto& observation :
         playback_.active(std::chrono::milliseconds(timeout_ms_.load()))) {
        auto objects = extent_objects(observation.entry, *observation.current_extent + 1,
                                      window_.load());
        if (!objects.empty())
            out.push_back({playback_run(observation.session), std::move(objects), priority_.load(),
                           "read_ahead", FrameType::read_ahead});
    }
    return out;
}

CurrentFileHintProvider::CurrentFileHintProvider(PlaybackTracker& playback,
                                                 const HydrationConfig& config)
    : playback_(playback) {
    reconfigure(config);
}

void CurrentFileHintProvider::reconfigure(const HydrationConfig& config) {
    enabled_.store(config.current_file.enabled);
    priority_.store(config.current_file.priority);
    timeout_ms_.store(config.active_timeout.count());
}

std::vector<HydrationHint> CurrentFileHintProvider::hints() {
    if (!enabled_.load() || !priority_.load())
        return {};
    std::vector<HydrationHint> out;
    for (const auto& observation :
         playback_.active(std::chrono::milliseconds(timeout_ms_.load()))) {
        auto objects = extent_objects(observation.entry, *observation.current_extent + 1);
        if (!objects.empty())
            out.push_back({playback_run(observation.session), std::move(objects), priority_.load(),
                           "current_file", FrameType::read_ahead});
    }
    return out;
}

CatalogueSequenceHintProvider::CatalogueSequenceHintProvider(PlaybackTracker& playback,
                                                             FileSystem& filesystem,
                                                             CatalogueManager& catalogue,
                                                             const HydrationConfig& config)
    : playback_(playback), filesystem_(filesystem), catalogue_(catalogue) {
    reconfigure(config);
}

void CatalogueSequenceHintProvider::reconfigure(const HydrationConfig& config) {
    enabled_.store(config.catalogue.enabled);
    priority_.store(config.catalogue.priority);
    lookahead_.store(config.catalogue_lookahead);
    timeout_ms_.store(config.active_timeout.count());
}

std::vector<HydrationHint> CatalogueSequenceHintProvider::hints() {
    if (!enabled_.load() || !priority_.load() || !lookahead_.load())
        return {};

    // Do not touch or copy the catalogue when there is no active playback.
    // The hydrator wakes frequently by design; previously this provider called
    // CatalogueManager::snapshot() every interval even on an idle node, which
    // repairs metadata and copies the complete catalogue merely to discover
    // there is no current media to predict from.
    auto active = playback_.active(std::chrono::milliseconds(timeout_ms_.load()));
    if (active.empty())
        return {};

    CatalogueSnapshot snapshot;
    try {
        snapshot = catalogue_.snapshot();
    } catch (...) {
        return {};
    }

    std::vector<HydrationHint> out;
    for (const auto& observation : active) {
        const auto* current = find_current_catalogue_item(snapshot, observation);
        if (!current)
            continue;
        for (size_t distance = 0; distance < lookahead_.load(); ++distance) {
            current = next_catalogue_item(snapshot, *current);
            if (!current)
                break;

            std::optional<std::pair<std::string, FsEntry>> media;
            std::string selected_media_id;
            for (const auto& media_id : current->media_ids) {
                media = filesystem_.find_media(media_id);
                if (media) {
                    selected_media_id = media_id;
                    break;
                }
            }
            if (!media)
                break;

            auto objects = extent_objects(media->second, 0);
            if (objects.empty())
                break;
            const auto effective = std::max<uint32_t>(1, priority_.load() /
                                                             static_cast<uint32_t>(distance + 1));
            out.push_back({"catalogue:" + std::to_string(observation.session) + ":" +
                               selected_media_id,
                           std::move(objects), effective,
                           current->kind == CatalogueKind::episode ? "next_episode" : "next_movie",
                           FrameType::speculative});
        }
    }
    return out;
}

CacheHydrator::CacheHydrator(DistributedStore& store, HydrationConfig config)
    : store_(store), config_(std::move(config)) {
    status_.enabled = config_.enabled;
}

CacheHydrator::~CacheHydrator() {
    stop();
}

void CacheHydrator::add_provider(std::shared_ptr<HydrationHintProvider> provider) {
    if (!provider) return;
    std::lock_guard lock(mutex_);
    if (std::none_of(providers_.begin(), providers_.end(), [&](const auto& existing) {
            return existing.get() == provider.get();
        }))
        providers_.push_back(std::move(provider));
    cv_.notify_all();
}

void CacheHydrator::remove_provider(const HydrationHintProvider* provider) {
    std::lock_guard lock(mutex_);
    providers_.erase(std::remove_if(providers_.begin(), providers_.end(),
                                    [&](const auto& existing) { return existing.get() == provider; }),
                     providers_.end());
}

void CacheHydrator::start() {
    std::lock_guard lock(mutex_);
    if (worker_.joinable())
        return;
    worker_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void CacheHydrator::stop() {
    request_stop();
    if (worker_.joinable())
        worker_.join();
}

void CacheHydrator::request_stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        cv_.notify_all();
    }
}

void CacheHydrator::reconfigure(HydrationConfig config) {
    std::lock_guard lock(mutex_);
    config_ = std::move(config);
    status_.enabled = config_.enabled;
    cv_.notify_all();
}

std::vector<HydrationHint> CacheHydrator::collect_hints() {
    std::vector<std::shared_ptr<HydrationHintProvider>> providers;
    {
        std::lock_guard lock(mutex_);
        providers = providers_;
    }
    std::vector<HydrationHint> out;
    for (const auto& provider : providers) {
        try {
            auto current = provider->hints();
            out.insert(out.end(), std::make_move_iterator(current.begin()),
                       std::make_move_iterator(current.end()));
        } catch (const std::exception& error) {
            Log::debug("hydration hint provider " + std::string(provider->name()) + ": " +
                       error.what());
        }
    }
    return out;
}

bool CacheHydrator::run_once() {
    {
        std::lock_guard lock(mutex_);
        if (!config_.enabled)
            return false;
        const auto now = Clock::now();
        std::erase_if(failed_until_, [&](const auto& item) { return item.second <= now; });
    }
    if (!store_.hydration_available())
        return false;

    auto hints = collect_hints();
    const auto now = Clock::now();
    auto request = scheduler_.next(
        hints,
        [&](const ObjectId& id) { return store_.locally_available(id); },
        [&](const ObjectId& id) {
            std::lock_guard lock(mutex_);
            auto it = failed_until_.find(id);
            return it != failed_until_.end() && it->second > now;
        });
    if (!request)
        return false;

    {
        std::lock_guard lock(mutex_);
        ++status_.requests;
        status_.last_object = request->object;
        status_.last_reason = request->reason;
    }

    if (store_.hydrate(request->object, request->sequence_index, request->frame_type)) {
        std::lock_guard lock(mutex_);
        ++status_.fetched;
        failed_until_.erase(request->object);
        return true;
    }

    {
        std::lock_guard lock(mutex_);
        ++status_.unavailable;
        failed_until_[request->object] = Clock::now() + std::chrono::seconds(2);
    }
    return false;
}

HydrationStatus CacheHydrator::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void CacheHydrator::wake() {
    cv_.notify_all();
}

void CacheHydrator::loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-hydrator", std::chrono::seconds(5), true);
    struct Pending {
        HydrationRequest request;
        std::future<bool> future;
    };
    std::vector<Pending> pending;

    auto complete = [&](Pending& item) {
        bool fetched = false;
        try {
            fetched = item.future.get();
        } catch (const std::exception& error) {
            Log::debug("hydration fetch " + to_string(item.request.object) + ": " + error.what());
        } catch (...) {
            Log::debug("hydration fetch " + to_string(item.request.object) + ": unknown error");
        }

        std::lock_guard lock(mutex_);
        if (status_.in_flight)
            --status_.in_flight;
        if (fetched) {
            ++status_.fetched;
            failed_until_.erase(item.request.object);
        } else {
            ++status_.unavailable;
            failed_until_[item.request.object] = Clock::now() + std::chrono::seconds(2);
        }
    };

    while (!stop.stop_requested()) {
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            complete(*it);
            it = pending.erase(it);
        }

        HydrationConfig config;
        {
            std::lock_guard lock(mutex_);
            config = config_;
            const auto now = Clock::now();
            std::erase_if(failed_until_, [&](const auto& item) { return item.second <= now; });
        }

        if (config.enabled && store_.hydration_available()) {
            while (!stop.stop_requested() && pending.size() < config.max_inflight) {
                auto hints = collect_hints();
                const auto now = Clock::now();
                auto request = scheduler_.next(
                    hints,
                    [&](const ObjectId& id) {
                        if (store_.locally_available(id))
                            return true;
                        return std::any_of(pending.begin(), pending.end(), [&](const Pending& item) {
                            return item.request.object == id;
                        });
                    },
                    [&](const ObjectId& id) {
                        std::lock_guard lock(mutex_);
                        auto it = failed_until_.find(id);
                        return it != failed_until_.end() && it->second > now;
                    });
                if (!request)
                    break;

                {
                    std::lock_guard lock(mutex_);
                    ++status_.requests;
                    ++status_.in_flight;
                    status_.peak_in_flight = std::max(status_.peak_in_flight, status_.in_flight);
                    status_.last_object = request->object;
                    status_.last_reason = request->reason;
                }

                pending.push_back({
                    *request,
                    std::async(std::launch::async, [this, request = *request] {
                        return store_.hydrate(request.object, request.sequence_index,
                                              request.frame_type);
                    }),
                });
            }
        }

        cpu_reporter.tick();
        std::unique_lock lock(mutex_);
        const auto interval = config_.interval;
        cv_.wait_for(lock, stop, interval, [] { return false; });
    }

    for (auto& item : pending) {
        item.future.wait();
        complete(item);
    }
}

HydrationManager::HydrationManager(DistributedStore& store, PlaybackTracker& playback,
                                   FileSystem& filesystem, CatalogueManager& catalogue,
                                   HydrationConfig config, size_t read_ahead_extents)
    : read_ahead_(std::make_shared<ReadAheadHintProvider>(playback, config, read_ahead_extents)),
      current_file_(std::make_shared<CurrentFileHintProvider>(playback, config)),
      catalogue_sequence_(std::make_shared<CatalogueSequenceHintProvider>(playback, filesystem, catalogue, config)),
      hydrator_(store, config) {
    hydrator_.add_provider(read_ahead_);
    hydrator_.add_provider(current_file_);
    hydrator_.add_provider(catalogue_sequence_);
}

void HydrationManager::start() {
    hydrator_.start();
}

void HydrationManager::request_stop() {
    hydrator_.request_stop();
}

void HydrationManager::stop() {
    hydrator_.stop();
}

void HydrationManager::reconfigure(HydrationConfig config, size_t read_ahead_extents) {
    read_ahead_->reconfigure(config, read_ahead_extents);
    current_file_->reconfigure(config);
    catalogue_sequence_->reconfigure(config);
    hydrator_.reconfigure(std::move(config));
}

} // namespace macha
