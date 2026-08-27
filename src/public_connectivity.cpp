// SPDX-License-Identifier: GPL-3.0-or-later
#include "public_connectivity.hpp"

#include "log.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MACHA_HAVE_MINIUPNPC
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

namespace macha {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    if (first)
        value.erase(0, first);
    return value;
}

bool ipv4_literal(std::string_view value) {
    std::array<unsigned char, 4> bytes{};
    const std::string text(value);
    return ::inet_pton(AF_INET, text.c_str(), bytes.data()) == 1;
}

size_t curl_write(char* data, size_t size, size_t count, void* opaque) {
    const size_t bytes = size * count;
    auto* out = static_cast<std::string*>(opaque);
    if (out->size() + bytes > 256)
        return 0;
    out->append(data, bytes);
    return bytes;
}

std::string aws_external_ip(std::chrono::milliseconds timeout) {
    static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
    if (initialized != CURLE_OK)
        throw std::runtime_error("curl_global_init failed");

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("curl_easy_init failed");

    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, "https://checkip.amazonaws.com/");
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "macha-public-connectivity/1");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);

    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK)
        throw std::runtime_error(std::string("AWS external IP request failed: ") +
                                 curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200)
        throw std::runtime_error("AWS external IP request returned HTTP " +
                                 std::to_string(status));

    body = trim(std::move(body));
    if (!ipv4_literal(body))
        throw std::runtime_error("AWS external IP response was not an IPv4 literal");
    return body;
}

bool tcp_probe(const Endpoint& endpoint, std::chrono::milliseconds timeout, std::string& error) {
    if (endpoint.host.empty() || !endpoint.port) {
        error = "advertised endpoint is incomplete";
        return false;
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* raw = nullptr;
    const auto service = std::to_string(endpoint.port);
    const int resolved = getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &raw);
    if (resolved != 0) {
        error = std::string("resolve failed: ") + gai_strerror(resolved);
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);

    const auto deadline = Clock::now() + timeout;
    std::string last_error = "connection failed";
    for (auto* address = addresses.get(); address; address = address->ai_next) {
        const auto now = Clock::now();
        if (now >= deadline)
            break;
        int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = std::strerror(errno);
            ::close(fd);
            continue;
        }

        int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc == 0) {
            ::close(fd);
            return true;
        }
        if (errno != EINPROGRESS) {
            last_error = std::strerror(errno);
            ::close(fd);
            continue;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd item{fd, POLLOUT, 0};
        do {
            rc = ::poll(&item, 1, static_cast<int>(std::max<int64_t>(1, remaining.count())));
        } while (rc < 0 && errno == EINTR);
        if (rc > 0) {
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                socket_error == 0) {
                ::close(fd);
                return true;
            }
            last_error = socket_error ? std::strerror(socket_error) : "connect failed";
        } else if (rc == 0) {
            last_error = "timeout";
        } else {
            last_error = std::strerror(errno);
        }
        ::close(fd);
    }
    error = std::move(last_error);
    return false;
}

#ifdef MACHA_HAVE_MINIUPNPC

struct UpnpSession {
    UPNPDev* devices{};
    UPNPUrls urls{};
    IGDdatas data{};
    std::array<char, 64> lan{};
    std::array<char, 64> wan{};
    int igd_status{};

    ~UpnpSession() {
        if (devices)
            freeUPNPDevlist(devices);
        if (igd_status > 0)
            FreeUPNPUrls(&urls);
    }
};

std::unique_ptr<UpnpSession> discover_upnp(std::chrono::milliseconds timeout,
                                           std::string& error) {
    auto session = std::make_unique<UpnpSession>();
    int discovery_error = 0;
#if MINIUPNPC_API_VERSION >= 14
    session->devices = upnpDiscover(static_cast<int>(timeout.count()), nullptr, nullptr,
                                    UPNP_LOCAL_PORT_ANY, 0, 2, &discovery_error);
#else
    session->devices = upnpDiscover(static_cast<int>(timeout.count()), nullptr, nullptr,
                                    UPNP_LOCAL_PORT_ANY, 0, &discovery_error);
#endif
    if (!session->devices) {
        error = "UPnP discovery failed code=" + std::to_string(discovery_error);
        return {};
    }
#if MINIUPNPC_API_VERSION >= 18
    session->igd_status = UPNP_GetValidIGD(session->devices, &session->urls, &session->data,
                                           session->lan.data(),
                                           static_cast<int>(session->lan.size()),
                                           session->wan.data(),
                                           static_cast<int>(session->wan.size()));
#else
    session->igd_status = UPNP_GetValidIGD(session->devices, &session->urls, &session->data,
                                           session->lan.data(),
                                           static_cast<int>(session->lan.size()));
#endif
    if (session->igd_status <= 0) {
        error = "no valid UPnP IGD found";
        return {};
    }
#if MINIUPNPC_API_VERSION < 18
    if (UPNP_GetExternalIPAddress(session->urls.controlURL, session->data.first.servicetype,
                                  session->wan.data()) != UPNPCOMMAND_SUCCESS)
        session->wan[0] = '\0';
#endif
    return session;
}

struct ExistingMapping {
    bool exists{};
    std::string internal_client;
    uint16_t internal_port{};
};

ExistingMapping get_mapping(UpnpSession& session, uint16_t external_port) {
    const auto external = std::to_string(external_port);
    std::array<char, 64> client{};
    std::array<char, 16> port{};
#if MINIUPNPC_API_VERSION >= 10
    const int result = UPNP_GetSpecificPortMappingEntry(
        session.urls.controlURL, session.data.first.servicetype, external.c_str(), "TCP", nullptr,
        client.data(), port.data(), nullptr, nullptr, nullptr);
#else
    const int result = UPNP_GetSpecificPortMappingEntry(
        session.urls.controlURL, session.data.first.servicetype, external.c_str(), "TCP",
        client.data(), port.data(), nullptr, nullptr, nullptr);
#endif
    if (result != UPNPCOMMAND_SUCCESS)
        return {};

    unsigned parsed = 0;
    const auto text = std::string_view(port.data());
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size() || parsed > 65535)
        return {true, client.data(), 0};
    return {true, client.data(), static_cast<uint16_t>(parsed)};
}

#endif

} // namespace

PublicConnectivity::PublicConnectivity(const Config& config, NodeId node_id, Endpoint configured)
    : upnp_config_(config.upnp), external_ip_config_(config.external_ip),
      check_config_(config.connectivity_check), configured_(std::move(configured)),
      node_id_(node_id) {
    status_.configured = configured_;
    status_.advertised = configured_;
    status_.upnp.enabled = upnp_config_.enabled;
#ifdef MACHA_HAVE_MINIUPNPC
    status_.upnp.support_built = true;
#endif
    status_.upnp.internal_port = configured_.port;
    status_.upnp.external_port =
        upnp_config_.external_port ? upnp_config_.external_port : configured_.port;
    status_.upnp.lease_seconds = upnp_config_.lease_seconds;
    status_.external_ip.enabled = external_ip_config_.enabled;
    status_.check_enabled = check_config_.enabled;
}

PublicConnectivity::~PublicConnectivity() {
    std::lock_guard lock(mutex_);
    remove_owned_mapping_locked();
}

PublicConnectivityStatus PublicConnectivity::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

PublicConnectivityStatus PublicConnectivity::refresh(bool should_probe, bool force_probe) {
    std::lock_guard lock(mutex_);
    refresh_locked();
    if (should_probe)
        probe_locked(force_probe);
    return status_;
}

PublicConnectivityStatus PublicConnectivity::probe(bool force) {
    std::lock_guard lock(mutex_);
    probe_locked(force);
    return status_;
}

void PublicConnectivity::refresh_locked() {
    status_.advertised = configured_;
    status_.advertised_source = "configured";
    status_.upnp.gateway_found = false;
    status_.upnp.mapping_active = false;
    status_.upnp.mapping_created = false;
    status_.upnp.mapping_owned = false;
    status_.upnp.private_wan = false;
    status_.upnp.lan_address.clear();
    status_.upnp.external_address.clear();
    status_.upnp.igd_status = 0;
    status_.upnp.error.clear();
    status_.external_ip.attempted = false;
    status_.external_ip.address.clear();
    status_.external_ip.error.clear();
    status_.self_probe = "not_run";
    status_.self_probe_error.clear();

    const uint16_t external_port =
        upnp_config_.external_port ? upnp_config_.external_port : configured_.port;

    if (upnp_config_.enabled) {
#ifdef MACHA_HAVE_MINIUPNPC
        std::string discovery_error;
        if (auto session = discover_upnp(upnp_config_.discovery_timeout, discovery_error)) {
            status_.upnp.gateway_found = true;
            status_.upnp.igd_status = session->igd_status;
            status_.upnp.lan_address = session->lan.data();
            status_.upnp.external_address = session->wan.data();
            bool usable_igd = true;
#if MINIUPNPC_API_VERSION >= 18
            status_.upnp.private_wan = session->igd_status == UPNP_PRIVATEIP_IGD;
            usable_igd = session->igd_status == UPNP_CONNECTED_IGD || status_.upnp.private_wan;
            if (!usable_igd)
                status_.upnp.error = "UPnP IGD is not connected status=" +
                                     std::to_string(session->igd_status);
#endif
            if (usable_igd) {
                const auto ext = std::to_string(external_port);
                const auto in = std::to_string(configured_.port);
                const auto lease = std::to_string(upnp_config_.lease_seconds);
                const std::string description = "Macha " + to_string(node_id_).substr(0, 12);

                const auto existing = get_mapping(*session, external_port);
                bool active = existing.exists && existing.internal_port == configured_.port &&
                              existing.internal_client == status_.upnp.lan_address;
                if (existing.exists && !active) {
                    status_.upnp.error = "UPnP external port " + ext +
                                         " is already mapped to " +
                                         existing.internal_client + ":" +
                                         std::to_string(existing.internal_port);
                } else if (!active) {
                    const int result = UPNP_AddPortMapping(
                        session->urls.controlURL, session->data.first.servicetype, ext.c_str(),
                        in.c_str(), session->lan.data(), description.c_str(), "TCP", nullptr,
                        lease.c_str());
                    if (result == UPNPCOMMAND_SUCCESS) {
                        const auto created = get_mapping(*session, external_port);
                        active = created.exists && created.internal_port == configured_.port &&
                                 created.internal_client == status_.upnp.lan_address;
                        if (active) {
                            mapping_owned_ = true;
                            owned_external_port_ = external_port;
                            status_.upnp.mapping_created = true;
                            status_.upnp.mapping_owned = true;
                        } else {
                            status_.upnp.error =
                                "UPnP AddPortMapping succeeded but mapping verification failed";
                        }
                    } else {
                        status_.upnp.error = "UPnP AddPortMapping failed code=" +
                                             std::to_string(result) + " (" +
                                             strupnperror(result) + ")";
                    }
                }
                status_.upnp.mapping_active = active;
                status_.upnp.mapping_owned = active && mapping_owned_;
                if (active && ipv4_literal(status_.upnp.external_address)) {
                    status_.advertised = {status_.upnp.external_address, external_port};
                    status_.advertised_source = "upnp";
                }
            }
        } else {
            status_.upnp.error = std::move(discovery_error);
        }
#else
        status_.upnp.error = "UPnP support was not built (miniupnpc not found)";
#endif
    }

    const bool needs_external_ip =
        external_ip_config_.enabled &&
        (status_.advertised_source == "configured" || status_.upnp.private_wan ||
         (status_.upnp.mapping_active && !ipv4_literal(status_.upnp.external_address)));
    if (needs_external_ip) {
        status_.external_ip.attempted = true;
        try {
            status_.external_ip.address = aws_external_ip(external_ip_config_.timeout);
            status_.advertised.host = status_.external_ip.address;
            status_.advertised.port = status_.upnp.mapping_active ? external_port : configured_.port;
            status_.advertised_source =
                status_.upnp.mapping_active ? "upnp+external_ip" : "external_ip";
        } catch (const std::exception& error) {
            status_.external_ip.error = error.what();
        }
    }

    status_.checked_unix_ms = unix_ms();

    if (upnp_config_.enabled) {
        if (status_.upnp.mapping_active) {
            Log::info("UPnP port mapping active gateway_external=" +
                      status_.upnp.external_address + ":" +
                      std::to_string(status_.upnp.external_port) + " internal=" +
                      status_.upnp.lan_address + ":" + std::to_string(configured_.port) +
                      " lease_s=" + std::to_string(upnp_config_.lease_seconds));
            if (status_.upnp.private_wan)
                Log::warn("UPnP gateway WAN address is private address=" +
                          status_.upnp.external_address + " probable_cgnat=true");
        } else if (!status_.upnp.error.empty()) {
            Log::warn(status_.upnp.error);
        }
    }
    if (status_.external_ip.attempted) {
        if (!status_.external_ip.address.empty())
            Log::info("external IP discovered source=aws address=" + status_.external_ip.address);
        else if (!status_.external_ip.error.empty())
            Log::warn(status_.external_ip.error);
    }
}

void PublicConnectivity::probe_locked(bool force) {
    status_.self_probe_error.clear();
    if (!check_config_.enabled && !force) {
        status_.self_probe = "disabled";
        status_.checked_unix_ms = unix_ms();
        return;
    }

    std::string error;
    if (tcp_probe(status_.advertised, check_config_.timeout, error)) {
        status_.self_probe = "reachable";
        Log::info("connectivity self probe reachable endpoint=" + status_.advertised.host + ":" +
                  std::to_string(status_.advertised.port) + " external_verification=unknown");
    } else {
        status_.self_probe = "unreachable";
        status_.self_probe_error = std::move(error);
        Log::warn("connectivity self probe failed endpoint=" + status_.advertised.host + ":" +
                  std::to_string(status_.advertised.port) +
                  " external_reachability=unknown reason=\"" + status_.self_probe_error +
                  "\" (NAT loopback may be unavailable)");
    }
    status_.checked_unix_ms = unix_ms();
}

void PublicConnectivity::remove_owned_mapping_locked() noexcept {
#ifdef MACHA_HAVE_MINIUPNPC
    if (!mapping_owned_ || !owned_external_port_)
        return;
    try {
        std::string error;
        auto session = discover_upnp(upnp_config_.discovery_timeout, error);
        if (!session)
            return;
        const auto ext = std::to_string(owned_external_port_);
        const auto existing = get_mapping(*session, owned_external_port_);
        if (!existing.exists || existing.internal_port != configured_.port ||
            existing.internal_client != session->lan.data()) {
            Log::debug("UPnP owned mapping no longer matches; leaving external_port=" + ext);
            mapping_owned_ = false;
            owned_external_port_ = 0;
            return;
        }
        const int result = UPNP_DeletePortMapping(session->urls.controlURL,
                                                  session->data.first.servicetype,
                                                  ext.c_str(), "TCP", nullptr);
        if (result == UPNPCOMMAND_SUCCESS)
            Log::debug("UPnP port mapping removed external_port=" + ext);
        else
            Log::debug("UPnP port mapping removal failed code=" + std::to_string(result));
    } catch (...) {
    }
    mapping_owned_ = false;
    owned_external_port_ = 0;
#endif
}

} // namespace macha
