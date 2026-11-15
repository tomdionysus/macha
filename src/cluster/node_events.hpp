// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dannan/status/events.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <stop_token>

namespace macha {

// What has happened on this node, one monotonic count per kind: Dannan's, owned
// by its node. Producers only notify; consumers keep the counts they last saw
// and react to the difference.
using NodeEvents = dannan::status::NodeEvents;

// The kinds Macha counts. Dannan's own: membership (a node joined or left, or
// its place, capabilities or incarnation changed) and topology (the same, and
// any advert). The store node's, named the same by Dagda: storage (what this
// node holds changed: an object stored or removed, a disk come back),
// metadata, and claims (what this node's claims protect changed, not what it
// holds).
namespace node_event {
inline constexpr NodeEvents::Kind topology = NodeEvents::topology;
inline constexpr NodeEvents::Kind membership = NodeEvents::membership;
inline constexpr NodeEvents::Kind storage = NodeEvents::dannan_kinds;
inline constexpr NodeEvents::Kind metadata = NodeEvents::dannan_kinds + 1;
inline constexpr NodeEvents::Kind claims = NodeEvents::dannan_kinds + 2;

// Names storage, metadata and claims on `events`, in that order, before
// anything else names a kind, so each lands at its number above. Throws
// std::logic_error when one does not.
inline void name_kinds(NodeEvents& events) {
    if (events.kind("storage") != storage || events.kind("metadata") != metadata ||
        events.kind("claims") != claims)
        throw std::logic_error("node event kinds named out of order");
}
} // namespace node_event

// Waits until `ready()` holds, `stop` is requested, or `deadline` passes
// (none for time_point::max()), waking on every event; whether `ready()`
// held. A change to `ready()` that no event announces is seen only at the
// deadline.
template <class Ready>
bool wait_for_events(NodeEvents& events, const std::stop_token& stop,
                     std::chrono::steady_clock::time_point deadline, Ready&& ready) {
    while (true) {
        const auto seen = events.total();
        if (ready())
            return true;
        if (stop.stop_requested())
            return false;
        auto timeout = std::chrono::milliseconds::max();
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return ready();
            timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        }
        (void)events.wait_for_change(seen, timeout, stop);
    }
}

} // namespace macha
