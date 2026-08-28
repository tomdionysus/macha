// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_engine.hpp"

namespace macha {

std::unique_ptr<MediaEngine> make_libav_media_engine(const StreamingConfig&) {
    return {};
}

} // namespace macha
