// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace macha::miniupnpc_compat {

// UPNP_GetValidIGD() status values. From API 18 it reports a connected IGD
// with a private WAN address separately; the values are spelled out because
// miniupnpc 2.2.8 (API 18) lacks the UPNP_CONNECTED_IGD/UPNP_PRIVATEIP_IGD macros.
inline constexpr int connected_igd = 1;
inline constexpr int private_wan_igd = 2;

constexpr bool private_wan(int api_version, int status) noexcept {
    return api_version >= 18 && status == private_wan_igd;
}

constexpr bool usable(int api_version, int status) noexcept {
    return status == connected_igd || private_wan(api_version, status);
}

} // namespace macha::miniupnpc_compat
