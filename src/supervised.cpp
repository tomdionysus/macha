// SPDX-License-Identifier: GPL-3.0-or-later
#include "supervised.hpp"
#include "log.hpp"

#include <exception>

namespace macha {

void run_supervised(std::string_view name, const std::function<void()>& body) noexcept {
    try {
        try {
            body();
        } catch (const std::exception& e) {
            Log::error("subsystem '" + std::string(name) +
                       "' thread stopped on exception: " + e.what());
        } catch (...) {
            Log::error("subsystem '" + std::string(name) + "' thread stopped on unknown exception");
        }
    } catch (...) {
        // Logging itself failed (e.g. allocation failure building the message).
        // Still must not let anything escape a thread entry point.
    }
}

} // namespace macha
