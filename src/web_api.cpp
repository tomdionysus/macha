// SPDX-License-Identifier: GPL-3.0-or-later
#include "web_api.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>

namespace macha {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// What a built web client is made of. Anything not named here is served as
// bytes: a client asset with an unknown extension is still the client's, and
// application/octet-stream is the honest answer for it.
struct AssetType {
    std::string_view extension;
    std::string_view mime;
};

constexpr std::array<AssetType, 26> asset_types{{
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".map", "application/json; charset=utf-8"},
    {".webmanifest", "application/manifest+json; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".xml", "application/xml; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".avif", "image/avif"},
    {".ico", "image/x-icon"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    {".wasm", "application/wasm"},
    {".mp4", "video/mp4"},
    {".webm", "video/webm"},
    {".mp3", "audio/mpeg"},
}};

std::string asset_mime(const fs::path& path) {
    const auto extension = lower(path.extension().string());
    for (const auto& type : asset_types)
        if (type.extension == extension) return std::string(type.mime);
    return "application/octet-stream";
}

// A file on disk, read as the response is written. The client's assets are
// small, but a bundle is not something to hold twice in memory per request,
// and this keeps a large asset from deciding how much a node allocates.
class FileBody final : public HttpBodySource {
    int fd_{-1};
    uint64_t size_{};

  public:
    FileBody(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~FileBody() override {
        if (fd_ >= 0) ::close(fd_);
    }
    FileBody(const FileBody&) = delete;
    FileBody& operator=(const FileBody&) = delete;

    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_ || fd_ < 0) return 0;
        const auto wanted = static_cast<size_t>(
            std::min<uint64_t>(destination.size(), size_ - offset));
        size_t filled = 0;
        while (filled < wanted) {
            const auto n = ::pread(fd_, destination.data() + filled, wanted - filled,
                                   static_cast<off_t>(offset + filled));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            filled += static_cast<size_t>(n);
        }
        return filled;
    }
};

// One path segment at a time, so the checks are on what the segments say
// rather than on what the joined string looks like. A segment that is empty,
// `.`, `..`, or begins with a dot is refused: the first three are how a
// request climbs out of the root, and the last is how it reads the build
// tooling's leftovers (.env, .git) that have no business being served.
std::optional<fs::path> relative_request_path(std::string_view path) {
    fs::path out;
    size_t begin = 0;
    while (begin < path.size()) {
        while (begin < path.size() && path[begin] == '/') ++begin;
        if (begin >= path.size()) break;
        auto end = path.find('/', begin);
        if (end == std::string_view::npos) end = path.size();
        const auto segment = http_url_decode(path.substr(begin, end - begin));
        if (segment.empty() || segment == "." || segment == ".." || segment.front() == '.')
            return std::nullopt;
        if (segment.find('/') != std::string::npos || segment.find('\\') != std::string::npos ||
            segment.find('\0') != std::string::npos)
            return std::nullopt;
        out /= segment;
        begin = end;
    }
    return out;
}

bool within(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end() || *c != *r) return false;
    }
    return true;
}

std::string entity_tag(const fs::path& path, uint64_t size) {
    std::error_code ec;
    const auto written = fs::last_write_time(path, ec);
    const auto stamp = ec ? 0LL
                          : static_cast<long long>(written.time_since_epoch().count());
    return "\"" + std::to_string(size) + "-" + std::to_string(stamp) + "\"";
}

HttpResponse serve(const fs::path& path, bool is_index, const HttpRequest& request) {
    std::error_code ec;
    const auto size = static_cast<uint64_t>(fs::file_size(path, ec));
    if (ec) return http_error(404, "not_found", "no such file");
    const auto tag = entity_tag(path, size);

    HttpResponse response;
    response.content_type = asset_mime(path);
    response.headers["ETag"] = tag;
    // The index document names the current asset bundle, so it must be
    // revalidated on every load or a deploy is invisible until the browser
    // decides otherwise. The assets it names are content-addressed by the
    // client's own build and can be held.
    response.headers["Cache-Control"] = is_index ? "no-cache" : "public, max-age=3600";

    if (auto it = request.headers.find("if-none-match");
        it != request.headers.end() && it->second == tag) {
        response.status = 304;
        return response;
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return http_error(404, "not_found", "no such file");
    response.status = 200;
    response.stream = std::make_shared<FileBody>(fd, size);
    return response;
}

} // namespace

WebApi::WebApi(WebConfig config) : config_(std::move(config)) {}

bool WebApi::enabled() const noexcept {
    return config_.enabled && !config_.root.empty();
}

bool WebApi::api_path(std::string_view path) noexcept {
    return path == "/api" || path.starts_with("/api/");
}

HttpResponse WebApi::handle(const HttpRequest& request) const {
    if (!enabled())
        return http_error(404, "not_found", "no web client is configured on this node");
    if (api_path(request.path))
        return http_error(404, "not_found", "endpoint not found");
    if (request.method != "GET" && request.method != "HEAD")
        return http_error(405, "method", "GET or HEAD required");

    std::error_code ec;
    const auto root = fs::canonical(config_.root, ec);
    if (ec)
        return http_error(503, "web_client_unavailable",
                          "the configured web root does not exist on this node");
    const auto index = root / config_.index;

    // A path naming a file under the root is that file. Everything else is
    // the client's own route, and the client is the index document -- which
    // is the whole point: /library/artist/x is a route it will resolve for
    // itself once it has loaded, and 404 would never let it load.
    if (const auto relative = relative_request_path(request.path); relative && !relative->empty()) {
        const auto candidate = fs::weakly_canonical(root / *relative, ec);
        if (!ec && within(root, candidate) && fs::is_regular_file(candidate, ec))
            return serve(candidate, candidate == index, request);
    }

    if (!fs::is_regular_file(index, ec))
        return http_error(503, "web_client_unavailable",
                          "the configured web root has no index document");
    return serve(index, true, request);
}

} // namespace macha
