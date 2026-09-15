// SPDX-License-Identifier: GPL-3.0-or-later
#include "subsystem_registry.hpp"

// std::unique_lock over the header's shared_mutex. libc++ happens to reach it
// through <shared_mutex>; libstdc++ (the cluster's compiler) does not.
#include <mutex>

namespace macha {

void SubsystemRegistry::publish_torrent(std::shared_ptr<TorrentService> service) {
    std::unique_lock lock(mutex_);
    torrent_ = std::move(service);
}

void SubsystemRegistry::withdraw_torrent(const TorrentService* service) {
    std::unique_lock lock(mutex_);
    if (torrent_.get() == service)
        torrent_.reset();
}

std::shared_ptr<TorrentService> SubsystemRegistry::torrent() const {
    std::shared_lock lock(mutex_);
    return torrent_;
}

void SubsystemRegistry::publish_fuse(std::shared_ptr<FuseFrontend> frontend) {
    std::unique_lock lock(mutex_);
    fuse_ = std::move(frontend);
}

void SubsystemRegistry::withdraw_fuse(const FuseFrontend* frontend) {
    std::unique_lock lock(mutex_);
    if (fuse_.get() == frontend)
        fuse_.reset();
}

std::shared_ptr<FuseFrontend> SubsystemRegistry::fuse() const {
    std::shared_lock lock(mutex_);
    return fuse_;
}

} // namespace macha
