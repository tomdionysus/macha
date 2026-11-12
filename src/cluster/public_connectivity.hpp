// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <string>

namespace macha {

// What the node reports of its public reachability, as Status shows it.
struct UpnpStatus {
    bool enabled{};
    bool support_built{};
    bool gateway_found{};
    bool mapping_active{};
    bool mapping_created{};
    bool mapping_owned{};
    bool private_wan{};
    std::string lan_address;
    std::string external_address;
    uint16_t internal_port{};
    uint16_t external_port{};
    uint32_t lease_seconds{};
    int igd_status{};
    // igd_not_connected, port_mapped_elsewhere, mapping_verification_failed,
    // add_mapping_failed, discovery_failed, support_not_built; `error` is the
    // English message beside it.
    std::string error_code;
    std::string error;
};

struct ExternalIpStatus {
    bool enabled{};
    bool attempted{};
    std::string address;
    std::string error_code; // lookup_failed
    std::string error;
};

struct PublicConnectivityStatus {
    Endpoint configured;
    Endpoint advertised;
    std::string advertised_source{"configured"};
    UpnpStatus upnp;
    ExternalIpStatus external_ip;
    bool check_enabled{};
    std::string self_probe{"not_run"};
    std::string self_probe_error;
    uint64_t checked_unix_ms{};
};

} // namespace macha
