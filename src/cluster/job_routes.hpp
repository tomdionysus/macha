// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <span>

namespace macha {

// Where the node sends its job RPCs: ingest and torrent job queries and
// actions, and torrent intents. The node is built before the components that
// answer them, so whoever constructs such a component binds its route once it
// is built and unbinds it before destroying it. unbind() waits for calls in
// flight, so no call reaches a destroyed component. Thread-safe.
//
// A handler answers from its component's local state only, never surveying
// peers, or one cluster-wide query fans out unboundedly; it runs under the
// route lock and must not bind or unbind.
class JobRoutes {
  public:
    using Handler = std::function<Bytes(std::span<const uint8_t> request_payload)>;
    enum class Route : uint8_t {
        ingest_jobs,
        ingest_action,
        torrent_jobs,
        torrent_action,
        torrent_intent,
    };

    void bind(Route route, Handler handler) {
        std::unique_lock lock(mutex_);
        handlers_[index(route)] = std::move(handler);
    }
    // Waits for calls in flight on any route.
    void unbind(Route route) {
        std::unique_lock lock(mutex_);
        handlers_[index(route)] = {};
    }
    // Empty when nothing is bound to `route`.
    std::optional<Bytes> call(Route route, std::span<const uint8_t> payload) const {
        std::shared_lock lock(mutex_);
        const auto& handler = handlers_[index(route)];
        if (!handler)
            return std::nullopt;
        return handler(payload);
    }

  private:
    static constexpr size_t route_count = 5;
    static size_t index(Route route) noexcept { return static_cast<size_t>(route); }

    mutable std::shared_mutex mutex_;
    std::array<Handler, route_count> handlers_;
};

} // namespace macha
