// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/conversions.hpp"
#include "config.hpp"
#include "contract/thread_safety.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <thread>

namespace macha {

// Book conversion with Calibre's ebook-convert, run as a child process at
// low priority, at most `max_concurrent` at a time. The source is copied to
// a working file first, so the child never reads the cluster. Converted
// copies are kept under the cache path until the cache passes its limit,
// when the least recently served go first. A failure is remembered until
// asked again.
class BookConversions final : public MediaConversions {
    struct Key {
        std::string media_id;
        std::string format;
        auto operator<=>(const Key&) const = default;
    };
    struct Job {
        Key key;
        MediaContentSource source;
    };

    const BookConversionConfig config_;
    const std::filesystem::path cache_path_;
    Mutex mutex_;
    std::condition_variable_any wake_;
    std::map<Key, ConversionState> states_ MACHA_GUARDED_BY(mutex_);
    std::deque<Job> queue_ MACHA_GUARDED_BY(mutex_);
    // The running children, killed on stop.
    std::map<Key, pid_t> running_ MACHA_GUARDED_BY(mutex_);
    bool stopping_ MACHA_GUARDED_BY(mutex_){};
    // Owned by the instantiator's thread (start/stop).
    std::vector<std::jthread> workers_;

    std::filesystem::path output_path(const Key&) const;
    void work(std::stop_token);
    ConversionState convert(const Job&);
    void evict();

  public:
    static constexpr std::array<std::string_view, 5> supported{"epub", "azw3", "mobi", "pdf", "fb2"};

    BookConversions(BookConversionConfig, const std::filesystem::path& state_path);
    ~BookConversions() override;
    void start();
    void stop();

    std::vector<std::string> formats() const override;
    ConversionState request(const std::string& media_id, const std::string& format,
                            const MediaContentSource& source) override;
    std::optional<ConversionState> state(const std::string& media_id,
                                         const std::string& format) override;
    std::optional<MediaContentSource> content(const std::string& media_id,
                                              const std::string& format) override;
    bool erase(const std::string& media_id, const std::string& format) override;
};

} // namespace macha
