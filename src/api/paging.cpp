// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/paging.hpp"

#include <charconv>

namespace macha {

std::optional<HttpResponse> read_page_query(const HttpRequest& request, PageQuery& query) {
    if (const auto limit = request.query.find("limit");
        limit != request.query.end() && !limit->second.empty()) {
        size_t value = 0;
        const auto& text = limit->second;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size() || value == 0 ||
            value > page_limit_max)
            return http_error(400, "bad_limit", "limit must be 1..1000");
        query.limit = value;
    }
    if (const auto cursor = request.query.find("cursor");
        cursor != request.query.end() && !cursor->second.empty()) {
        query.after = page_cursor_key(cursor->second);
        if (!query.after)
            return http_error(400, "bad_cursor", "cursor is not one this server issued");
    }
    return {};
}

std::string page_cursor(std::string_view key) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(key.size() * 2);
    for (const unsigned char c : key) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::optional<std::string> page_cursor_key(std::string_view cursor) {
    if (cursor.size() % 2)
        return {};
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string key;
    key.reserve(cursor.size() / 2);
    for (size_t i = 0; i < cursor.size(); i += 2) {
        const auto high = nibble(cursor[i]);
        const auto low = nibble(cursor[i + 1]);
        if (high < 0 || low < 0)
            return {};
        key += static_cast<char>((high << 4) | low);
    }
    return key;
}

void page_json(Json::Array& entries, std::string_view key, const PageQuery& query,
               Json::Object& out) {
    const auto key_of = [&](const Json& entry) -> std::string_view {
        const auto* value = entry.find(key);
        return value && value->isString() ? std::string_view(value->asString())
                                          : std::string_view();
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const Json& a, const Json& b) { return key_of(a) < key_of(b); });
    const auto range = page_range(entries, key_of, query);
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(range.end), entries.end());
    entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(range.begin));
    out["next_cursor"] = range.next_cursor ? Json(*range.next_cursor) : Json(nullptr);
}

} // namespace macha
