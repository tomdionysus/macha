// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <shared_mutex>

namespace macha {

class FuseFrontend;
class TorrentService;

// Where subsystem plugins publish their capabilities and core looks them up by
// abstract interface; "no plugin loaded" is a null lookup. Lookups return
// shared_ptr because a faulted subsystem is rebuilt in place: a caller's copy
// keeps the old instance alive until it is done.
class SubsystemRegistry {
  public:
    void publish_torrent(std::shared_ptr<TorrentService>);
    // Withdraws only if the given instance is current: a faulted subsystem's stop()
    // can run after its replacement has published.
    void withdraw_torrent(const TorrentService*);
    std::shared_ptr<TorrentService> torrent() const;

    // Concrete type: FuseFrontend lives in macha_core; only the libfuse
    // adapter is a plugin.
    void publish_fuse(std::shared_ptr<FuseFrontend>);
    void withdraw_fuse(const FuseFrontend*);
    std::shared_ptr<FuseFrontend> fuse() const;

  private:
    mutable std::shared_mutex mutex_;
    std::shared_ptr<TorrentService> torrent_;
    std::shared_ptr<FuseFrontend> fuse_;
};

} // namespace macha
