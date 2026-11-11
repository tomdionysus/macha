// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"

#include <cstdint>
#include <string>

namespace macha {

// How this node answers "can peers connect to me?" and "do I host extents?",
// with the configured modes beside the resolved values. `source` decided the
// inbound answer: "configured", "persisted", "default" (auto, no evidence yet,
// behaving as capable) or "probe:<peer>".
struct InboundResolution {
    Tristate inbound_capable_mode{Tristate::automatic};
    Tristate hosts_extents_mode{Tristate::automatic};
    bool inbound_capable{true};
    bool hosts_extents{true};
    std::string source{"default"};
    uint64_t decided_unix_ms{};
    uint64_t last_probe_unix_ms{};
    std::string last_probe_peer;
    std::string last_probe_error;
    unsigned consecutive_probe_failures{};
};

} // namespace macha
