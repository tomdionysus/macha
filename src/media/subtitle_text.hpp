// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace macha {

// Convert FFmpeg's decoded ASS/SSA event payload into plain subtitle text.
std::string plain_ass_subtitle_text(std::string text);

} // namespace macha
