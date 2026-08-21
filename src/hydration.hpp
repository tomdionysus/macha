// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "metadata.hpp"
#include "net.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace macha {

class CatalogueManager;
class DistributedStore;
class FileSystem;

struct PlaybackObservation {
    uint64_t session{};
    std::string path;
    FsEntry entry;
    std::optional<size_t> current_extent;
    Clock::time_point last_activity{};
};

class PlaybackTracker {
    mutable std::mutex mutex_;
    std::map<uint64_t, PlaybackObservation> sessions_;
    uint64_t next_session_{1};

  public:
    uint64_t open(std::string path, const FsEntry&);
    void progress(uint64_t session, size_t extent_index);
    void close(uint64_t session);
    std::vector<PlaybackObservation> active(std::chrono::milliseconds timeout) const;
};

struct HydrationHint {
    // Objects in one run are ordered. The hydrator will never jump over an
    // unavailable earlier object in order to fetch a later object from that run.
    std::string run_id;
    std::vector<ObjectId> objects;
    uint32_t priority{};
    std::string reason;
    FrameType frame_type{FrameType::speculative};
};

struct HydrationRequest {
    std::string run_id;
    ObjectId object{};
    size_t sequence_index{};
    uint32_t priority{};
    std::string reason;
    FrameType frame_type{FrameType::speculative};
};

class HydrationHintProvider {
  public:
    virtual ~HydrationHintProvider() = default;
    virtual std::string_view name() const = 0;
    virtual std::vector<HydrationHint> hints() = 0;
};

// Weighted fair scheduler for ordered runs. Overlapping hints for the same run
// are merged and their priorities reinforce each other. Fair virtual time means
// a high-priority current file advances faster without starving a lower-priority
// next-episode/next-film run.
class HydrationScheduler {
    std::map<std::string, double> virtual_finish_;

  public:
    std::optional<HydrationRequest>
    next(const std::vector<HydrationHint>&,
         const std::function<bool(const ObjectId&)>& present,
         const std::function<bool(const ObjectId&)>& blocked = {});
    void reset();
};

class ReadAheadHintProvider final : public HydrationHintProvider {
    PlaybackTracker& playback_;
    std::atomic_bool enabled_{true};
    std::atomic_uint32_t priority_{1000};
    std::atomic_size_t window_{3};
    std::atomic_int64_t timeout_ms_{30000};

  public:
    ReadAheadHintProvider(PlaybackTracker&, const HydrationConfig&, size_t window);
    std::string_view name() const override { return "read_ahead"; }
    std::vector<HydrationHint> hints() override;
    void reconfigure(const HydrationConfig&, size_t window);
};

class CurrentFileHintProvider final : public HydrationHintProvider {
    PlaybackTracker& playback_;
    std::atomic_bool enabled_{true};
    std::atomic_uint32_t priority_{700};
    std::atomic_int64_t timeout_ms_{30000};

  public:
    CurrentFileHintProvider(PlaybackTracker&, const HydrationConfig&);
    std::string_view name() const override { return "current_file"; }
    std::vector<HydrationHint> hints() override;
    void reconfigure(const HydrationConfig&);
};

class CatalogueSequenceHintProvider final : public HydrationHintProvider {
    PlaybackTracker& playback_;
    FileSystem& filesystem_;
    CatalogueManager& catalogue_;
    std::atomic_bool enabled_{true};
    std::atomic_uint32_t priority_{300};
    std::atomic_size_t lookahead_{1};
    std::atomic_int64_t timeout_ms_{30000};

  public:
    CatalogueSequenceHintProvider(PlaybackTracker&, FileSystem&, CatalogueManager&,
                                  const HydrationConfig&);
    std::string_view name() const override { return "catalogue"; }
    std::vector<HydrationHint> hints() override;
    void reconfigure(const HydrationConfig&);
};

struct HydrationStatus {
    bool enabled{};
    uint64_t requests{};
    uint64_t fetched{};
    uint64_t unavailable{};
    size_t in_flight{};
    size_t peak_in_flight{};
    std::optional<ObjectId> last_object;
    std::string last_reason;
};

class CacheHydrator {
    DistributedStore& store_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    HydrationConfig config_;
    std::vector<std::shared_ptr<HydrationHintProvider>> providers_;
    HydrationScheduler scheduler_;
    std::map<ObjectId, Clock::time_point> failed_until_;
    std::jthread worker_;
    HydrationStatus status_;

    std::vector<HydrationHint> collect_hints();
    void loop(std::stop_token);

  public:
    CacheHydrator(DistributedStore&, HydrationConfig);
    ~CacheHydrator();
    void add_provider(std::shared_ptr<HydrationHintProvider>);
    void remove_provider(const HydrationHintProvider*);
    void start();
    void request_stop();
    void stop();
    void reconfigure(HydrationConfig);
    bool run_once();
    HydrationStatus status() const;
    void wake();
};

class HydrationManager {
    std::shared_ptr<ReadAheadHintProvider> read_ahead_;
    std::shared_ptr<CurrentFileHintProvider> current_file_;
    std::shared_ptr<CatalogueSequenceHintProvider> catalogue_sequence_;
    CacheHydrator hydrator_;

  public:
    HydrationManager(DistributedStore&, PlaybackTracker&, FileSystem&, CatalogueManager&,
                     HydrationConfig, size_t read_ahead_extents);
    void start();
    void request_stop();
    void stop();
    void reconfigure(HydrationConfig, size_t read_ahead_extents);
    CacheHydrator& hydrator() { return hydrator_; }
    void add_provider(std::shared_ptr<HydrationHintProvider> provider) { hydrator_.add_provider(std::move(provider)); }
    void remove_provider(const HydrationHintProvider* provider) { hydrator_.remove_provider(provider); }
};

} // namespace macha
