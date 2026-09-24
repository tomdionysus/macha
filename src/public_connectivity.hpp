// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "types.hpp"

#include <mutex>
#include <string>

namespace macha {

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

class PublicConnectivity {
    UpnpConfig upnp_config_;
    ExternalIpConfig external_ip_config_;
    ConnectivityCheckConfig check_config_;
    Endpoint configured_;
    NodeId node_id_;
    mutable std::mutex mutex_;
    PublicConnectivityStatus status_;
#ifdef MACHA_HAVE_MINIUPNPC
    bool mapping_owned_{};
    uint16_t owned_external_port_{};
#endif

    void refresh_locked();
    void probe_locked(bool force);
    void remove_owned_mapping_locked() noexcept;

  public:
    PublicConnectivity(const Config&, NodeId, Endpoint configured);
    ~PublicConnectivity();
    PublicConnectivity(const PublicConnectivity&) = delete;
    PublicConnectivity& operator=(const PublicConnectivity&) = delete;

    PublicConnectivityStatus refresh(bool probe, bool force_probe = false);
    PublicConnectivityStatus probe(bool force = false);
    PublicConnectivityStatus status() const;
};

} // namespace macha
