// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string_view>

namespace macha {

// An import's `catalogue.state`, one rule for an ingest job and the torrent it
// came from: processing while the job catalogues or a file is pending; waiting
// while nothing is counted and the job is not complete; then
// completed_with_issues if a file failed or matched nothing, else completed.
constexpr std::string_view catalogue_state(size_t total, size_t pending, size_t no_match,
                                           size_t failed, bool cataloguing, bool complete) {
    if (cataloguing || pending)
        return "processing";
    if (!total && !complete)
        return "waiting";
    if (failed || no_match)
        return "completed_with_issues";
    return "completed";
}

} // namespace macha
