// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace macha::miniupnpc_compat {

// MINIUPNPC_API_VERSION 18 changed UPNP_GetValidIGD() to distinguish a
// connected IGD with a private/reserved WAN address. The documented ABI values
// are stable, but miniupnpc 2.2.8/API 18 shipped before the corresponding
// UPNP_CONNECTED_IGD and UPNP_PRIVATEIP_IGD macros were added. Do not make API
// 18 support depend on those later header aliases.
inline constexpr int connected_igd = 1;
inline constexpr int private_wan_igd = 2;

constexpr bool private_wan(int api_version, int status) noexcept {
    return api_version >= 18 && status == private_wan_igd;
}

constexpr bool usable(int api_version, int status) noexcept {
    return status == connected_igd || private_wan(api_version, status);
}

} // namespace macha::miniupnpc_compat
