// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "http/http.hpp"
#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// A list call's page: `limit` entries after `cursor`. Without `limit` the
// whole list is one page.
struct PageQuery {
    std::optional<size_t> limit;
    // The key the previous page ended at.
    std::optional<std::string> after;
};

inline constexpr size_t page_limit_max = 1000;

// Reads `limit` (1..page_limit_max) and `cursor` from the request. A bad
// value is answered with the returned response: 400 bad_limit or bad_cursor.
std::optional<HttpResponse> read_page_query(const HttpRequest&, PageQuery&);

// The cursor that resumes after `key`, and the key it names. Opaque to
// clients.
std::string page_cursor(std::string_view key);
std::optional<std::string> page_cursor_key(std::string_view cursor);

// The slice of `sorted` (ascending by `key`, keys unique) a query selects.
struct PageRange {
    size_t begin{};
    size_t end{};
    // Absent on the last page.
    std::optional<std::string> next_cursor;
};

template <class T, class KeyOf>
PageRange page_range(const std::vector<T>& sorted, KeyOf key_of, const PageQuery& query) {
    PageRange range;
    range.end = sorted.size();
    if (query.after)
        range.begin = static_cast<size_t>(
            std::upper_bound(sorted.begin(), sorted.end(), *query.after,
                             [&](const std::string& after, const T& entry) {
                                 return after < std::string_view(key_of(entry));
                             }) -
            sorted.begin());
    if (query.limit && sorted.size() - range.begin > *query.limit) {
        range.end = range.begin + *query.limit;
        range.next_cursor = page_cursor(key_of(sorted[range.end - 1]));
    }
    return range;
}

// Sorts `entries` by their string field `key`, keeps the page, and sets
// `next_cursor` in `out`.
void page_json(Json::Array& entries, std::string_view key, const PageQuery&, Json::Object& out);

} // namespace macha
