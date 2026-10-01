// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The component contract (the object ledger spec, A5): what a core component
// implements to take part in the composition root. A component declares the
// contracts it requires and provides, receives its dependencies as
// constructor parameters, and has three lifecycle steps: start, a stop
// request that never blocks, and a stop that joins. Faults its own threads
// discover go to the sink the root attaches.
//
// Subsystem (src/subsystem/subsystem.hpp) is the plugin form of the same
// lifecycle; the two meet when plugins move into the root.
namespace macha {

class Component {
  public:
    using FaultSink = std::function<void(std::string reason)>;

    virtual ~Component() = default;

    virtual std::string_view name() const noexcept = 0;
    // The contracts this component needs, by name: each is provided by
    // another component in the root, or declared external to it.
    virtual std::vector<std::string> required() const { return {}; }
    // The contracts this component offers the others in the root.
    virtual std::vector<std::string> provided() const { return {}; }

    // Called once, after every component it requires has started.
    virtual void start() = 0;
    // Asks the component to stop. Must not block: it signals and returns.
    // May be called more than once, and before stop().
    virtual void request_stop() noexcept = 0;
    // Stops the component -- requesting the stop itself if nobody has -- and
    // waits for its threads. Called once, before anything it requires is
    // stopped.
    virtual void stop() = 0;

    // Installed by the root before start(). A component whose faults all
    // surface from start() can ignore it.
    virtual void attach_fault_sink(FaultSink sink) { (void)sink; }
};

} // namespace macha
