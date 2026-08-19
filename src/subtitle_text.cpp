// SPDX-License-Identifier: GPL-3.0-or-later
#include "subtitle_text.hpp"

#include <cctype>

namespace macha {

std::string plain_ass_subtitle_text(std::string text) {
    // FFmpeg exposes decoded ASS/SSA rectangles as an event payload, not the
    // full source-file Dialogue line. Its canonical event form is:
    // ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
    // (eight commas before Text). Treat an optional Dialogue: prefix as input
    // tolerance, then strip exactly those event fields.
    if (text.starts_with("Dialogue:")) {
        text.erase(0, 9);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.erase(text.begin());
    }

    size_t commas = 0;
    size_t pos = 0;
    for (; pos < text.size(); ++pos) {
        if (text[pos] == ',' && ++commas == 8) {
            ++pos;
            break;
        }
    }
    if (commas == 8) text.erase(0, pos);

    std::string out;
    out.reserve(text.size());
    bool tag = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            tag = true;
            continue;
        }
        if (text[i] == '}' && tag) {
            tag = false;
            continue;
        }
        if (tag) continue;
        if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n')) {
            out.push_back('\n');
            ++i;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

} // namespace macha
