// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue_api.hpp"

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

std::map<std::string, std::string, std::less<>> parse_query(std::string_view query) {
    std::map<std::string, std::string, std::less<>> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        auto part = query.substr(pos, amp == std::string_view::npos ? query.size() - pos : amp - pos);
        auto eq = part.find('=');
        out[url_decode(part.substr(0, eq))] =
            eq == std::string_view::npos ? "" : url_decode(part.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

std::optional<std::string> read_token(const std::optional<std::filesystem::path>& path) {
    if (!path) return {};
    std::ifstream file(*path);
    if (!file) throw std::runtime_error("cannot open catalogue API token file");
    std::string token;
    std::getline(file, token);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
    if (token.empty()) throw std::runtime_error("catalogue API token file is empty");
    return token;
}

} // namespace

HttpResponse CatalogueApi::handle(const HttpRequest& request) {
    try {
        if (request.method == "GET" && request.path == "/api/v1/catalogue/status") {
            auto status = catalogue_.status();
            std::string out = "{\"enabled\":true,\"ready\":" + std::string(status.ready ? "true" : "false");
            out += ",\"metadata_generation\":" + std::to_string(status.metadata_generation);
            out += ",\"root\":" + (status.root ? json_escape(to_string(*status.root)) : "null");
            out += ",\"items\":" + std::to_string(status.items);
            out += ",\"artwork_objects\":" + std::to_string(status.artwork_objects);
            out += ",\"local_artwork_objects\":" + std::to_string(status.local_artwork_objects);
            out += ",\"last_sync_unix_ms\":" + std::to_string(status.last_sync_unix_ms);
            out += ",\"error\":" + (status.error.empty() ? "null" : json_escape(status.error)) + "}";
            return json(200, std::move(out));
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
            auto bytes = catalogue_.artwork(id);
            if (!bytes) return error(404, "not_found", "artwork not found");
            std::string mime = "application/octet-stream";
            auto snapshot = catalogue_.snapshot();
            for (const auto& [_, item] : snapshot.items) {
                auto it = std::find_if(item.artwork.begin(), item.artwork.end(), [&](const auto& art) { return art.id == id; });
                if (it != item.artwork.end()) { mime = it->mime_type; break; }
            }
            return {200, mime, {}, std::move(*bytes)};
        }

        return error(404, "not_found", "endpoint not found");
    } catch (const CatalogueConflict& e) {
        return error(409, "conflict", e.what());
    } catch (const std::exception& e) {
        return error(503, "catalogue_unavailable", e.what());
    }
}

HttpServer::HttpServer(CatalogueApiConfig config,
                       std::function<HttpResponse(const HttpRequest&)> handler)
    : config_(std::move(config)), handler_(std::move(handler)),
      bearer_token_(read_token(config_.token_file)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void HttpServer::run(std::stop_token stop) {
    try {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* results = nullptr;
        auto port = std::to_string(config_.port);
        const char* host = config_.listen.empty() ? nullptr : config_.listen.c_str();
        if (getaddrinfo(host, port.c_str(), &hints, &results))
            throw std::runtime_error("catalogue API address resolution failed");
        for (auto* result = results; result; result = result->ai_next) {
            listen_fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (listen_fd_ < 0) continue;
            int yes = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (bind(listen_fd_, result->ai_addr, result->ai_addrlen) == 0 && listen(listen_fd_, 32) == 0)
                break;
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        freeaddrinfo(results);
        if (listen_fd_ < 0) throw std::runtime_error("cannot bind catalogue API");
        Log::info("catalogue API listening on " + config_.listen + ":" + std::to_string(config_.port));
        while (!stop.stop_requested()) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (stop.stop_requested() || !running_) break;
                continue;
            }
            handle_client(fd);
            ::close(fd);
        }
    } catch (const std::exception& e) {
        if (running_) Log::error("catalogue API: " + std::string(e.what()));
    }
}

void HttpServer::handle_client(int fd) {
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif

    std::string input;
    std::array<char, 8192> buffer{};
    auto header_end = std::string::npos;
    while ((header_end = input.find("\r\n\r\n")) == std::string::npos) {
        auto n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) return;
        input.append(buffer.data(), static_cast<size_t>(n));
        if (input.size() > 64 * 1024) return;
    }

    std::istringstream headers(input.substr(0, header_end));
    std::string request_line;
    std::getline(headers, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
    std::istringstream request_stream(request_line);
    HttpRequest request;
    std::string target, version;
    request_stream >> request.method >> target >> version;
    if (request.method.empty() || target.empty()) return;
    auto question = target.find('?');
    request.path = url_decode(target.substr(0, question));
    if (question != std::string::npos) request.query = parse_query(target.substr(question + 1));

    std::string line;
    size_t content_length = 0;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        request.headers[key] = value;
        if (key == "content-length") {
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), content_length);
            if (ec != std::errc{} || end != value.data() + value.size()) return;
        }
    }

    HttpResponse response;
    if (content_length > config_.max_request_bytes) {
        response = error(413, "too_large", "request body too large");
    } else {
        auto body_start = header_end + 4;
        request.body.insert(request.body.end(), input.begin() + static_cast<std::ptrdiff_t>(body_start), input.end());
        while (request.body.size() < content_length) {
            auto n = recv(fd, buffer.data(), std::min(buffer.size(), content_length - request.body.size()), 0);
            if (n <= 0) return;
            request.body.insert(request.body.end(), buffer.begin(), buffer.begin() + n);
        }
        if (request.body.size() > content_length) request.body.resize(content_length);

        if (bearer_token_) {
            auto authorization = request.headers.find("authorization");
            const std::string expected = "Bearer " + *bearer_token_;
            if (authorization == request.headers.end() || authorization->second != expected)
                response = error(401, "unauthorized", "bearer token required");
            else
                response = handler_(request);
        } else {
            response = handler_(request);
        }
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << reason(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    for (const auto& [key, value] : response.headers) out << key << ": " << value << "\r\n";
    out << "\r\n";
    auto head = out.str();
    (void)send(fd, head.data(), head.size(), send_flags);
    size_t sent = 0;
    while (sent < response.body.size()) {
        auto n = send(fd, response.body.data() + sent, response.body.size() - sent, send_flags);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

} // namespace macha
