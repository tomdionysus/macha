// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "filesystem.hpp"
#include "media_engine.hpp"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>

namespace macha {

namespace MediaInformationPriority {
inline constexpr int background = 10;
inline constexpr int requested = 50;
}

// Event-driven immutable media-profile discovery. Producers submit hints and
// never perform optional inspection themselves. All background reads remain
// speculative (below loader globally); foreground playback can take over the
// one per-media flight rather than waiting behind optional work.
class MediaInformationService {
    struct Flight;

    FileSystem& fs_;
    CatalogueManager& catalogue_;
    std::shared_ptr<MediaEngine> engine_;
    CatalogueHintQueue hints_;
    std::jthread worker_;

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, std::shared_ptr<Flight>, std::less<>> flights_;
    std::map<std::string, MediaProbeResult, std::less<>> pending_publications_;
    bool prune_requested_{true};
    bool started_{};

    std::optional<std::pair<std::string, FsEntry>> source_for(std::string_view media_id) const;
    bool media_is_live(std::string_view media_id) const;
    MediaProbeResult resolve(std::string media_id, std::string path, FsEntry entry,
                             bool foreground, Clock::time_point deadline);
    void process_hint(const CatalogueHint&, std::stop_token);
    void publish_one(std::string media_id, MediaProbeResult);
    void prune();
    void loop(std::stop_token);

  public:
    MediaInformationService(FileSystem&, CatalogueManager&, std::shared_ptr<MediaEngine>,
                            const std::filesystem::path& state_path);
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

    CatalogueHintQueue& hints() noexcept { return hints_; }
};

} // namespace macha
