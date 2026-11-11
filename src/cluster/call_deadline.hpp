// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>

// The deadlines a cluster call names: the latest it may end, reply or not. A stalled
// call ends sooner, by the no-progress watch; these bound one that keeps moving.
namespace macha::call_deadline {

inline constexpr std::chrono::milliseconds control{std::chrono::minutes(2)};
inline constexpr std::chrono::milliseconds metadata{std::chrono::minutes(5)};
inline constexpr std::chrono::milliseconds object_transfer{std::chrono::minutes(30)};
inline constexpr std::chrono::milliseconds presence_probe{std::chrono::minutes(2)};

} // namespace macha::call_deadline
