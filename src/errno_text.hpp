// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <system_error>

namespace macha {

// The system's text for an errno value. Thread-safe, as strerror is not.
inline std::string errno_text(int error) {
    return std::system_category().message(error);
}

} // namespace macha
