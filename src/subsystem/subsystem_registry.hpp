// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <shared_mutex>

namespace macha {

class FuseFrontend;
class TorrentService;

// Where a subsystem plugin publishes the capability it implements and where
// core looks that capability up. Core never names a plugin's concrete class
// (TorrentManager lives in libmacha-torrent, not in macha_core); it holds
// only the abstract interface it finds here, and "no plugin loaded" is simply
// a null lookup rather than a compile-time fact.
//
// Lookups return a shared_ptr deliberately. A supervised subsystem is
// destroyed and reconstructed in place on fault, which would dangle a raw
// pointer an HTTP handler was already inside; a caller that took a copy keeps
// the old instance alive until it is done with it.
class SubsystemRegistry {
  public:
    void publish_torrent(std::shared_ptr<TorrentService>);
    // Takes the instance being withdrawn, not just a "clear it" command: a
    // faulted subsystem's stop() can land after its replacement has already
    // published, and must not withdraw the live one.
    void withdraw_torrent(const TorrentService*);
    std::shared_ptr<TorrentService> torrent() const;

    // FUSE publishes its concrete FuseFrontend rather than an abstract
    // service, because unlike TorrentManager it is core's own class: it lives
    // in macha_core and only the libfuse-facing adapter moves into a plugin
    // (see TODO/2026-09-14-fuse-supervised-subsystem-plan.md, decision 2).
    // Status and the manage endpoints therefore keep their concrete types, and
    // tests keep constructing a FuseFrontend directly.
    void publish_fuse(std::shared_ptr<FuseFrontend>);
    void withdraw_fuse(const FuseFrontend*);
    std::shared_ptr<FuseFrontend> fuse() const;

  private:
    mutable std::shared_mutex mutex_;
    std::shared_ptr<TorrentService> torrent_;
    std::shared_ptr<FuseFrontend> fuse_;
};

} // namespace macha
