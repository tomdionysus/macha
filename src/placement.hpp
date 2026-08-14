// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
namespace macha {
std::vector<NodeInfo> rendezvous_nodes(std::span<const uint8_t>, const std::vector<NodeInfo>&,
                                       size_t);
}
