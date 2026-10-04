// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdexcept>

#include "catalogue/catalogue.hpp"
#include "contract/thread_safety.hpp"
#include "catalogue/catalogue_hints.hpp"
#include "filesystem/filesystem.hpp"
#include "media/media_engine.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stop_token>
#include <thread>

namespace macha {

// This node has no media engine (streaming is off), so it cannot probe.
class MediaEngineUnavailable : public std::runtime_error {
  public:
    MediaEngineUnavailable() : std::runtime_error("this node has no media engine") {}
};


// The container keeps no byte index (only MP4 and Matroska do).
class KeyframeIndexUnsupported : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

namespace MediaInformationPriority {
inline constexpr int background = 10;
inline constexpr int requested = 50;
}

// Event-driven discovery of immutable media profiles. Producers only submit
// hints. Background reads are speculative (below loader); foreground playback
// takes over the per-media flight rather than waiting behind it.
class MediaInformationService {
    struct Flight;

    FileSystem& fs_;
    CatalogueManager& catalogue_;
    std::shared_ptr<MediaEngine> engine_;
    CatalogueHintQueue hints_;
    // worker_ and started_ belong to the instantiator's thread (start/stop).
    std::jthread worker_;

    // Held across hints_.next_ready_delay(), which waits on the hint queue's
    // I/O mutex.
    mutable IoMutex mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, std::shared_ptr<Flight>, std::less<>> flights_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, MediaProbeResult, std::less<>> pending_publications_
        MACHA_GUARDED_BY(mutex_);
    size_t pending_publication_bytes_ MACHA_GUARDED_BY(mutex_){};
    static constexpr size_t max_pending_publications_ = 128;
    static constexpr size_t max_pending_publication_bytes_ = 4ULL * 1024 * 1024;
    std::optional<Clock::time_point> publication_retry_at_ MACHA_GUARDED_BY(mutex_);
    const std::chrono::milliseconds publication_retry_delay_{250};
    bool prune_requested_ MACHA_GUARDED_BY(mutex_){true};
    bool started_{};
    const std::function<void(std::string, MediaProbeResult)> profile_publisher_;
    // One keyframe index build at a time per node; a second request for the
    // same file waits here, then finds it stored. Guards nothing; held across
    // the media read, keyframe probe and catalogue commit.
    IoMutex keyframe_index_mutex_;

    std::optional<std::pair<std::string, FsEntry>> source_for(std::string_view media_id) const;
    bool media_is_live(std::string_view media_id) const;
    MediaProbeResult resolve(std::string media_id, std::string path, FsEntry entry,
                             bool foreground, Clock::time_point deadline);
    void process_hint(const CatalogueHint&, std::stop_token);
    void queue_publication_locked(std::string media_id, MediaProbeResult) MACHA_REQUIRES(mutex_);
    void publish_one(std::string media_id, MediaProbeResult);
    void prune();
    void loop(std::stop_token);

  public:
    using ProfilePublisher = std::function<void(std::string, MediaProbeResult)>;

    MediaInformationService(FileSystem&, CatalogueManager&, std::shared_ptr<MediaEngine>,
                            const std::filesystem::path& state_path,
                            ProfilePublisher profile_publisher = {});
    ~MediaInformationService();

    void start();
    void request_stop();
    void stop();

    size_t request(const std::vector<std::string>& media_ids,
                   int priority = MediaInformationPriority::background,
                   std::string source = "media-information");
    bool request_path(std::string path,
                      int priority = MediaInformationPriority::background,
                      std::string source = "media-information-ingest");
    MediaProbeResult resolve_playback(std::string media_id, std::string path, FsEntry entry,
                                      Clock::time_point deadline);
    void request_prune();
    // A media's keyframe byte index: stored, or built, stored and returned.
    // Empty when the media is not in the namespace. Throws MediaError when the
    // file cannot be read, KeyframeIndexUnsupported for a container without one.
    std::optional<Bytes> keyframe_index(const std::string& media_id, Clock::time_point deadline);

    CatalogueHintQueue& hints() noexcept { return hints_; }
};

} // namespace macha
