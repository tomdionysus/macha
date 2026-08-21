// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue_api.hpp"
#include "macha_version.hpp"

#include "log.hpp"
#include "json.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace macha {
namespace {
std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out{"\""};
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xf]);
                out.push_back(hex[c & 0xf]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

Bytes body(std::string value) {
    return Bytes(value.begin(), value.end());
}

HttpResponse json(int status, std::string value) {
    return {status, "application/json; charset=utf-8", {}, body(std::move(value))};
}

HttpResponse error(int status, std::string_view code, std::string_view message) {
    return json(status, "{\"error\":" + json_escape(code) + ",\"message\":" +
                            json_escape(message) + "}");
}

std::string optional_number(const std::optional<int32_t>& value) {
    return value ? std::to_string(*value) : "null";
}

std::string item_json(const CatalogueItem& item) {
    std::string out = "{";
    out += "\"id\":" + json_escape(item.id);
    out += ",\"kind\":" + json_escape(catalogue_kind_name(item.kind));
    out += ",\"title\":" + json_escape(item.title);
    out += ",\"sort_title\":" + json_escape(item.sort_title);
    out += ",\"synopsis\":" + json_escape(item.synopsis);
    out += ",\"parent_id\":" + (item.parent_id ? json_escape(*item.parent_id) : "null");
    out += ",\"year\":" + optional_number(item.year);
    out += ",\"season_number\":" + optional_number(item.season_number);
    out += ",\"episode_number\":" + optional_number(item.episode_number);
    out += ",\"disc_number\":" + optional_number(item.disc_number);
    out += ",\"track_number\":" + optional_number(item.track_number);
    out += ",\"aliases\":[";
    for (size_t i = 0; i < item.aliases.size(); ++i) {
        if (i) out += ',';
        out += json_escape(item.aliases[i]);
    }
    out += "],\"external_ids\":{";
    bool first = true;
    for (const auto& [provider, id] : item.external_ids) {
        if (!first) out += ',';
        first = false;
        out += json_escape(provider) + ":" + json_escape(id);
    }
    out += "},\"media_ids\":[";
    for (size_t i = 0; i < item.media_ids.size(); ++i) {
        if (i) out += ',';
        out += json_escape(item.media_ids[i]);
    }
    out += "],\"artwork\":[";
    for (size_t i = 0; i < item.artwork.size(); ++i) {
        if (i) out += ',';
        const auto& art = item.artwork[i];
        out += "{\"role\":" + json_escape(art.role) + ",\"id\":" +
               json_escape(to_string(art.id)) + ",\"mime_type\":" +
               json_escape(art.mime_type) + "}";
    }
    out += "],\"revision\":" + std::to_string(item.revision);
    out += ",\"updated_ns\":" + std::to_string(item.updated_ns) + "}";
    return out;
}

std::string items_json(const std::vector<CatalogueItem>& items) {
    std::string out = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        out += item_json(items[i]);
    }
    out += "]}";
    return out;
}

std::optional<int32_t> json_i32(const Json& root, std::string_view key) {
    const auto* value = root.find(key);
    if (!value || value->isNull()) return {};
    auto number = value->asInt64();
    if (number < INT32_MIN || number > INT32_MAX)
        throw std::runtime_error("catalogue integer out of range");
    return static_cast<int32_t>(number);
}

CatalogueItem parse_item(std::string id, std::span<const uint8_t> bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto root = Json::parse(text);
    if (!root.isObject())
        throw std::runtime_error("catalogue item body must be a JSON object");
    CatalogueItem item;
    item.id = std::move(id);
    const auto* kind_value = root.find("kind");
    const auto* title_value = root.find("title");
    if (!kind_value || !title_value)
        throw std::runtime_error("catalogue item requires kind and title");
    auto kind = parse_catalogue_kind(kind_value->asString());
    if (!kind)
        throw std::runtime_error("unknown catalogue kind");
    item.kind = *kind;
    item.title = title_value->asString();
    if (const auto* value = root.find("sort_title")) item.sort_title = value->asString();
    if (const auto* value = root.find("synopsis")) item.synopsis = value->asString();
    if (const auto* value = root.find("parent_id"); value && !value->isNull())
        item.parent_id = value->asString();
    item.year = json_i32(root, "year");
    item.season_number = json_i32(root, "season_number");
    item.episode_number = json_i32(root, "episode_number");
    item.disc_number = json_i32(root, "disc_number");
    item.track_number = json_i32(root, "track_number");
    if (const auto* aliases = root.find("aliases")) {
        if (!aliases->isArray()) throw std::runtime_error("aliases must be an array");
        for (const auto& alias : aliases->asArray()) item.aliases.push_back(alias.asString());
    }
    if (const auto* external = root.find("external_ids")) {
        if (!external->isObject()) throw std::runtime_error("external_ids must be an object");
        for (const auto& [provider, value] : external->asObject())
            item.external_ids[provider] = value.asString();
    }
    if (const auto* media = root.find("media_ids")) {
        if (!media->isArray()) throw std::runtime_error("media_ids must be an array");
        for (const auto& value : media->asArray()) item.media_ids.push_back(value.asString());
    }
    if (const auto* artwork = root.find("artwork")) {
        if (!artwork->isArray()) throw std::runtime_error("artwork must be an array");
        for (const auto& value : artwork->asArray()) {
            const auto* id_value = value.find("id");
            const auto* role = value.find("role");
            const auto* mime = value.find("mime_type");
            if (!id_value || !role || !mime)
                throw std::runtime_error("artwork requires role, id and mime_type");
            auto decoded = unhex(id_value->asString());
            if (!decoded || decoded->size() != 32)
                throw std::runtime_error("bad artwork object id");
            CatalogueArtwork art;
            art.role = role->asString();
            std::copy(decoded->begin(), decoded->end(), art.id.bytes.begin());
            art.mime_type = mime->asString();
            item.artwork.push_back(std::move(art));
        }
    }
    return item;
}

std::optional<uint64_t> expected_revision(const HttpRequest& request) {
    auto it = request.headers.find("if-match");
    if (it == request.headers.end())
        return {};
    auto value = it->second;
    value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
    if (value.starts_with("rev-")) value.erase(0, 4);
    uint64_t revision{};
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), revision);
    if (ec != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("invalid If-Match revision");
    return revision;
}

std::string url_decode(std::string_view value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = nibble(value[i + 1]), lo = nibble(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

} // namespace

HttpResponse CatalogueApi::handle(const HttpRequest& request) {
    try {
        if (request.method == "GET" && request.path == "/api/v1/catalogue/status") {
            auto status = catalogue_.status();
            std::string out = "{\"server_version\":" + json_escape(kServerVersion) + ",\"enabled\":true,\"ready\":" + std::string(status.ready ? "true" : "false");
            out += ",\"metadata_generation\":" + std::to_string(status.metadata_generation);
            out += ",\"known_metadata_generation\":" + std::to_string(status.known_metadata_generation);
            out += ",\"root\":" + (status.root ? json_escape(to_string(*status.root)) : "null");
            out += ",\"items\":" + std::to_string(status.items);
            out += ",\"artwork_objects\":" + std::to_string(status.artwork_objects);
            out += ",\"local_artwork_objects\":" + std::to_string(status.local_artwork_objects);
            out += ",\"last_sync_unix_ms\":" + std::to_string(status.last_sync_unix_ms);
            out += ",\"error\":" + (status.error.empty() ? "null" : json_escape(status.error)) + "}";
            return json(200, std::move(out));
        }

        if (request.method == "GET" && request.path == "/api/v1/catalogue/hints") {
            Json::Array values;
            for (const auto& hint : hints_.list()) {
                Json::Object item;
                item["id"] = hint.id;
                item["path"] = hint.path;
                item["priority"] = static_cast<int64_t>(hint.priority);
                item["state"] = catalogue_hint_state_name(hint.state);
                item["attempts"] = static_cast<uint64_t>(hint.attempts);
                item["ready_after_unix_ms"] = hint.ready_after_unix_ms;
                item["provider"] = hint.provider.empty() ? Json(nullptr) : Json(hint.provider);
                item["media_id"] = hint.media_id.empty() ? Json(nullptr) : Json(hint.media_id);
                item["result"] = hint.result.empty() ? Json(nullptr) : Json(hint.result);
                item["error"] = hint.error.empty() ? Json(nullptr) : Json(hint.error);
                Json::Array catalogue_ids;
                for (const auto& id : hint.catalogue_item_ids) catalogue_ids.emplace_back(id);
                item["catalogue_item_ids"] = std::move(catalogue_ids);
                Json::Array origins;
                for (const auto& origin : hint.origins) {
                    Json::Object value;
                    value["source"] = origin.source;
                    value["source_ref"] = origin.source_ref;
                    value["priority"] = static_cast<int64_t>(origin.priority);
                    origins.emplace_back(std::move(value));
                }
                item["origins"] = std::move(origins);
                item["created_unix_ms"] = hint.created_unix_ms;
                item["updated_unix_ms"] = hint.updated_unix_ms;
                values.emplace_back(std::move(item));
            }
            Json::Object out;
            out["hints"] = std::move(values);
            return json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/catalogue/items") {
            std::optional<CatalogueKind> kind;
            if (auto it = request.query.find("type"); it != request.query.end() && !it->second.empty()) {
                kind = parse_catalogue_kind(it->second);
                if (!kind) return error(400, "bad_type", "unknown catalogue type");
            }
            std::optional<std::string_view> parent;
            if (auto it = request.query.find("parent"); it != request.query.end()) parent = it->second;
            return json(200, items_json(catalogue_.list(kind, parent)));
        }

        if (request.method == "GET" && request.path == "/api/v1/catalogue/search") {
            auto q = request.query.find("q");
            if (q == request.query.end()) return error(400, "missing_query", "q is required");
            size_t limit = 50;
            if (auto it = request.query.find("limit"); it != request.query.end() && !it->second.empty()) {
                auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), limit);
                if (ec != std::errc{} || end != it->second.data() + it->second.size() || limit > 1000)
                    return error(400, "bad_limit", "limit must be 0..1000");
            }
            return json(200, items_json(catalogue_.search(q->second, limit)));
        }

        constexpr std::string_view item_prefix = "/api/v1/catalogue/items/";
        if (request.path.starts_with(item_prefix)) {
            auto rest = std::string_view(request.path).substr(item_prefix.size());
            auto metadata_suffix = rest.rfind("/metadata");
            if (metadata_suffix != std::string_view::npos && metadata_suffix + 9 == rest.size() &&
                request.method == "DELETE") {
                auto id = url_decode(rest.substr(0, metadata_suffix));
                if (!catalogue_.clear_metadata(id, expected_revision(request)))
                    return error(404, "not_found", "catalogue item not found");
                if (request_rescan_) request_rescan_();
                return {204, "application/json; charset=utf-8", {}, {}};
            }

            auto artwork_suffix = rest.rfind("/artwork");
            if (artwork_suffix != std::string_view::npos && artwork_suffix + 8 == rest.size() &&
                request.method == "POST") {
                auto id = url_decode(rest.substr(0, artwork_suffix));
                auto role = request.query.find("role");
                auto mime = request.query.find("mime");
                if (role == request.query.end() || role->second.empty() || mime == request.query.end() || mime->second.empty())
                    return error(400, "bad_artwork", "role and mime are required");
                auto art = catalogue_.put_artwork(id, role->second, mime->second, request.body,
                                                  expected_revision(request));
                auto updated = catalogue_.get(id);
                auto response = json(201, "{\"role\":" + json_escape(art.role) +
                                             ",\"id\":" + json_escape(to_string(art.id)) +
                                             ",\"mime_type\":" + json_escape(art.mime_type) + "}");
                if (updated) response.headers["ETag"] = "\"rev-" + std::to_string(updated->revision) + "\"";
                return response;
            }

            auto id = url_decode(rest);
            if (request.method == "GET") {
                auto item = catalogue_.get(id);
                if (!item) return error(404, "not_found", "catalogue item not found");
                auto response = json(200, item_json(*item));
                response.headers["ETag"] = "\"rev-" + std::to_string(item->revision) + "\"";
                return response;
            }
            if (request.method == "PUT") {
                auto existing = catalogue_.get(id);
                auto item = parse_item(id, request.body);
                auto saved = catalogue_.upsert(std::move(item), expected_revision(request));
                auto response = json(existing ? 200 : 201, item_json(saved));
                response.headers["ETag"] = "\"rev-" + std::to_string(saved.revision) + "\"";
                return response;
            }
            if (request.method == "DELETE") {
                if (!catalogue_.erase(id, expected_revision(request)))
                    return error(404, "not_found", "catalogue item not found");
                return {204, "application/json; charset=utf-8", {}, {}};
            }
        }

        constexpr std::string_view artwork_prefix = "/api/v1/catalogue/artwork/";
        if (request.method == "GET" && request.path.starts_with(artwork_prefix)) {
            auto text = url_decode(std::string_view(request.path).substr(artwork_prefix.size()));
            auto decoded = unhex(text);
            if (!decoded || decoded->size() != 32) return error(400, "bad_id", "bad artwork object id");
            ObjectId id;
            std::copy(decoded->begin(), decoded->end(), id.bytes.begin());
            auto artwork = catalogue_.artwork(id);
            if (!artwork) return error(404, "not_found", "artwork not found");
            return {200, std::move(artwork->mime_type), {}, std::move(artwork->bytes)};
        }

        return error(404, "not_found", "endpoint not found");
    } catch (const CatalogueConflict& e) {
        return error(409, "conflict", e.what());
    } catch (const std::exception& e) {
        return error(503, "catalogue_unavailable", e.what());
    }
}


} // namespace macha
