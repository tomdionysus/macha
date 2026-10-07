// SPDX-License-Identifier: GPL-3.0-or-later
#include "observation.hpp"
#include "json.hpp"
#include "api/manage_api.hpp"
#include "api/status_api.hpp"
#include "supervised.hpp"
#include "test_backend_support.hpp"
#include "api/users_api.hpp"

#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/wait.h>
#endif

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

Json body_json(const HttpResponse& response) {
    return Json::parse(
        std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
}

HttpRequest request_for(std::string method, std::string path, std::string_view body = {},
                        std::map<std::string, std::string, std::less<>> query = {}) {
    HttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.body.assign(body.begin(), body.end());
    for (auto& [name, value] : query)
        request.query[name] = value;
    return request;
}

std::string error_code(const Json& body) {
    return body.find("error")->find("code")->asString();
}

std::string error_message(const Json& body) {
    return body.find("error")->find("message")->asString();
}

// A single node with what ManageApi and CatalogueScanner work on, built
// directly: no Service, so no maintenance pass, scanner or HTTP server runs
// beside the test.
class CatalogueBench {
    TestNode node_;
    std::optional<CatalogueManager> catalogue_;
    std::optional<CatalogueHintQueue> hints_;

  public:
    explicit CatalogueBench(std::string_view name, size_t metadata_write_copies = 1)
        : node_(name) {
        auto& config = node_.config();
        config.replication = 1;
        config.metadata_write_copies = metadata_write_copies;
        config.hydration.enabled = false;
        config.catalogue.scanner.enabled = false;
        config.maintenance.interval = std::chrono::hours(1);
        node_.start();
        auto& node = node_.node();
        catalogue_.emplace(node, node.local_state(), node.metadata_server(), node_.store(),
                           node_.metadata(), node.ledger());
        hints_.emplace(node.config().state_path);
    }
    const std::filesystem::path& path() const { return node_.path(); }
    BareNode& node() { return node_.node(); }
    MetadataManager& metadata() { return node_.metadata(); }
    DistributedStore& store() { return node_.store(); }
    FileSystem& fs() { return node_.filesystem(); }
    NodeResources& resources() { return node_.resources(); }
    const Config& config() { return node_.config(); }
    CatalogueManager& catalogue() { return *catalogue_; }
    CatalogueHintQueue& hints() { return *hints_; }

    std::unique_ptr<CatalogueScanner> scanner(CatalogueScannerConfig config = {},
                                              std::unique_ptr<HttpClient> http = {}) {
        return std::make_unique<CatalogueScanner>(node(), node().metadata_server(), fs(),
                                                  catalogue(), hints(), std::move(config),
                                                  std::move(http));
    }
    // A file the scanner found and could not match, as the Unmatched list sees it.
    std::pair<std::string, std::string> unmatched(const std::string& path, uint8_t seed) {
        write_file(fs(), path, pattern(4096, seed));
        const auto media_id = file_media_id(fs().getattr(path));
        const auto hint_id =
            hints().submit(path, "scanner", media_id, CatalogueHintPriority::periodic_scan);
        REQUIRE(hints().claim_next().has_value());
        hints().mark_no_match(hint_id, "scanner", media_id, "no_provider_match");
        return {hint_id, media_id};
    }
};

CatalogueScannerConfig provider_scanner_config(const std::filesystem::path& token) {
    CatalogueScannerConfig config;
    config.movies.roots = {"/Movies"};
    config.movies.tmdb.token_file = token;
    config.tv.roots = {"/TV"};
    config.tv.tmdb.token_file = token;
    config.music.roots = {"/Music"};
    return config;
}

std::filesystem::path write_token(const std::filesystem::path& dir) {
    const auto token = dir / "tmdb.token";
    std::ofstream(token) << "test-token\n";
    return token;
}

MACHA_TEST("invariants", test_convergence_demand_coalesces_burst_and_keeps_same_generation_event) {
    ConvergenceDemand demand;
    CHECK(!demand.pending());

    demand.request(10);
    auto first = demand.begin();
    REQUIRE(first.has_value());
    CHECK(first->generation == 10);

    // Notices arriving during a run advance the high-water mark without
    // scheduling a run per intermediate generation.
    for (uint64_t generation = 11; generation <= 210; ++generation)
        demand.request(generation);
    demand.request(210); // same-generation sibling/topology change

    auto during = demand.diagnostics();
    CHECK(during.events_received == 202);
    CHECK(during.runs_scheduled == 1);
    CHECK(during.latest_generation == 210);
    CHECK(demand.complete(*first));

    auto second = demand.begin();
    REQUIRE(second.has_value());
    CHECK(second->generation == 210);
    CHECK(!demand.complete(*second));

    auto settled = demand.diagnostics();
    CHECK(settled.runs_scheduled == 2);
    CHECK(settled.runs_completed == 2);
    CHECK(settled.completed_epoch == settled.requested_epoch);
    CHECK(!settled.scheduled);

    // A sibling notice at an already processed generation still schedules a
    // run: generation alone is not the identity.
    demand.request(210);
    auto sibling = demand.begin();
    REQUIRE(sibling.has_value());
    CHECK(sibling->epoch > second->epoch);
    CHECK(!demand.complete(*sibling));
    CHECK(demand.diagnostics().runs_scheduled == 3);
}

// ManageApi over unmatched files: list, rename (the hint follows the file),
// identify by hand under existing parents, match to a provider reference
// (refusals leave the file unmatched), list files bound twice as conflicts,
// and a scan keeps on a manual item only the files still in the namespace.
MACHA_TEST("invariants", test_unmatched_files_are_identified_by_hand_or_by_reference) {
    CatalogueBench bench("manage-unmatched");
    auto& fs = bench.fs();
    auto& catalogue = bench.catalogue();
    auto& hints = bench.hints();
    for (const auto* root : {"/Movies", "/TV", "/Music", "/Other"})
        fs.mkdir(root, 0755, getuid(), getgid());

    {
        // List, rename, identify by hand, browse, delete.
        auto scanner = bench.scanner();
        ManageApi manage(bench.node(), bench.metadata(), fs, catalogue, hints, *scanner);
        const auto [hint_id, media_id] = bench.unmatched("/Movies/unknown.mkv", 91);

        auto listed = manage.handle(request_for("GET", "/api/v1/manage/unmatched"));
        REQUIRE(listed.status == 200);
        CHECK(body_json(listed).find("count")->asUInt64() == 1);

        REQUIRE(manage.handle(request_for(
                                  "POST", "/api/v1/manage/filesystem/rename",
                                  R"({"path":"/Movies/unknown.mkv","destination":"/Movies/renamed.mkv"})"))
                    .status == 200);
        CHECK(file_media_id(fs.getattr("/Movies/renamed.mkv")) == media_id);
        auto moved = hints.list();
        REQUIRE(moved.size() == 1);
        CHECK(moved.front().path == "/Movies/renamed.mkv");
        CHECK(moved.front().media_id == media_id);

        auto created = manage.handle(
            request_for("POST", "/api/v1/manage/unmatched/" + moved.front().id + "/manual",
                        R"({"kind":"movie","title":"Manually Identified","year":2026})"));
        REQUIRE(created.status == 201);
        const auto leaf_id = body_json(created).find("leaf_item_id")->asString();
        auto item = catalogue.get(leaf_id);
        REQUIRE(item.has_value());
        CHECK(item->title == "Manually Identified");
        CHECK(item->media_ids == std::vector<std::string>{media_id});
        CHECK(!hints.get(moved.front().id).has_value());

        auto browsed = manage.handle(
            request_for("GET", "/api/v1/manage/filesystem", {}, {{"path", "/Movies"}}));
        REQUIRE(browsed.status == 200);
        const auto browsed_json = body_json(browsed);
        const auto& entries = browsed_json.find("entries")->asArray();
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().find("path")->asString() == "/Movies/renamed.mkv");
        CHECK(entries.front().find("media_id")->asString() == media_id);
        REQUIRE(entries.front().find("catalogue_item_ids")->asArray().size() == 1);
        CHECK(entries.front().find("catalogue_item_ids")->asArray().front().asString() == leaf_id);

        // Files-tab deletion also clears any durable match state for the path.
        const auto stale = hints.submit("/Movies/renamed.mkv", "scanner", media_id,
                                        CatalogueHintPriority::periodic_scan);
        REQUIRE(hints.claim_next().has_value());
        hints.mark_no_match(stale, "movies", media_id, "synthetic stale exception");
        REQUIRE(manage.handle(request_for("DELETE", "/api/v1/manage/filesystem", {},
                                          {{"path", "/Movies/renamed.mkv"}}))
                    .status == 204);
        CHECK(hints.list().empty());
    }

    {
        // By hand under existing parents, named by id.
        auto parent = [](CatalogueKind kind, std::string id, std::string title,
                         std::optional<std::string> parent_id = {}) {
            CatalogueItem item;
            item.kind = kind;
            item.id = std::move(id);
            item.title = std::move(title);
            if (parent_id)
                item.parent_id = *parent_id;
            return item;
        };
        auto season_two =
            parent(CatalogueKind::season, "tmdb:tv:1:season:2", "Season 2", "tmdb:tv:1");
        season_two.season_number = 2;
        catalogue.upsert_many({parent(CatalogueKind::show, "tmdb:tv:1", "Scanner Show"),
                               season_two,
                               parent(CatalogueKind::artist, "musicbrainz:artist:a",
                                      "Scanner Artist")});
        auto scanner = bench.scanner();
        ManageApi manage(bench.node(), bench.metadata(), fs, catalogue, hints, *scanner);
        int file = 0;
        auto unmatched = [&] {
            ++file;
            return bench.unmatched("/TV/hand-" + std::to_string(file) + ".mkv",
                                   static_cast<uint8_t>(100 + file))
                .first;
        };
        auto post = [&](const std::string& hint_id, std::string_view body) {
            auto response = manage.handle(
                request_for("POST", "/api/v1/manage/unmatched/" + hint_id + "/manual", body));
            return std::pair{response.status, body_json(response)};
        };
        auto leaf = [&](const Json& body) {
            return catalogue.get(body.find("leaf_item_id")->asString());
        };

        // season_id: the episode joins the scanner's season, locked.
        auto [status, body] = post(unmatched(), R"({"kind":"episode","season_id":"tmdb:tv:1:season:2","season_number":7,"episode_number":3})");
        REQUIRE(status == 201);
        auto episode = leaf(body);
        REQUIRE(episode.has_value());
        CHECK(episode->parent_id == "tmdb:tv:1:season:2");
        CHECK(episode->season_number == 2);
        CHECK(episode->external_ids.at("macha_metadata_locked") == "1");
        CHECK(catalogue.get("tmdb:tv:1:season:2")->title == "Season 2");

        // series_id plus an existing season number reuses that season.
        std::tie(status, body) = post(unmatched(), R"({"kind":"episode","series_id":"tmdb:tv:1","season_number":2,"episode_number":4,"lock":false})");
        REQUIRE(status == 201);
        episode = leaf(body);
        CHECK(episode->parent_id == "tmdb:tv:1:season:2");
        CHECK(!episode->external_ids.contains("macha_metadata_locked"));
        CHECK(catalogue.list(CatalogueKind::season, std::string_view("tmdb:tv:1")).size() == 1);

        // A new season number creates one season under the existing show.
        std::tie(status, body) = post(unmatched(), R"({"kind":"episode","series_id":"tmdb:tv:1","season_number":5,"episode_number":1})");
        REQUIRE(status == 201);
        const auto new_season = catalogue.get(*leaf(body)->parent_id);
        REQUIRE(new_season.has_value());
        CHECK(new_season->parent_id == "tmdb:tv:1");
        CHECK(new_season->season_number == 5);
        CHECK(catalogue.list(CatalogueKind::show).size() == 1);

        // artist_id plus an album title: one album, reused by the second track.
        std::tie(status, body) = post(unmatched(), R"({"kind":"track","artist_id":"musicbrainz:artist:a","album":"Hand Album","title":"One","track_number":1})");
        REQUIRE(status == 201);
        const auto album_id = leaf(body)->parent_id;
        CHECK(catalogue.get(*album_id)->parent_id == "musicbrainz:artist:a");
        std::tie(status, body) = post(unmatched(), R"({"kind":"track","artist_id":"musicbrainz:artist:a","album":"Hand Album","title":"Two","track_number":2})");
        REQUIRE(status == 201);
        CHECK(leaf(body)->parent_id == album_id);
        CHECK(catalogue.list(CatalogueKind::artist).size() == 1);

        // A missing parent is 404 and a wrong kind is 400, both naming the parent.
        const auto refused = unmatched();
        std::tie(status, body) = post(refused, R"({"kind":"episode","season_id":"tmdb:tv:missing","episode_number":1})");
        CHECK(status == 404);
        CHECK(error_code(body) == "parent_not_found");
        CHECK(body.find("error")->find("parent_id")->asString() == "tmdb:tv:missing");
        std::tie(status, body) = post(refused, R"({"kind":"track","album_id":"musicbrainz:artist:a","title":"Three"})");
        CHECK(status == 400);
        CHECK(error_code(body) == "bad_parent_kind");
        CHECK(body.find("error")->find("expected_kind")->asString() == "album");
        CHECK(body.find("error")->find("parent_kind")->asString() == "artist");
        CHECK(hints.get(refused).has_value());
    }

    {
        // By provider reference, as a scan would match it.
        const std::string release = "0f9a7b22-3c3e-4f5e-9d1a-2b8e6f7c5d41";
        auto http = std::make_unique<FakeHttpClient>();
        http->add("/movie/335984", 200, "application/json",
                  R"({"id":335984,"title":"Blade Runner 2049","release_date":"2017-10-04","poster_path":"/p.jpg"})");
        http->add("/movie/77", 500, "text/plain", "down");
        http->add("/tv/1399/season/1", 200, "application/json",
                  R"({"id":3624,"name":"Season 1","episodes":[{"id":63057,"episode_number":2,"name":"The Kingsroad"}]})");
        http->add("/tv/1399", 200, "application/json",
                  R"({"id":1399,"name":"Game of Thrones","first_air_date":"2011-04-17"})");
        // Before "/release/", which the cover lookup's URL also contains.
        http->add("coverartarchive.org/release/" + release, 200, "application/json",
                  R"({"images":[{"front":true,"image":"https://covers.example/full.jpg",
                                 "thumbnails":{"500":"https://covers.example/front-500.jpg"}}]})");
        http->add("covers.example", 200, "image/jpeg", "cover-bytes");
        http->add("/release/" + release, 200, "application/json",
                  R"({"id":")" + release + R"(","title":"Hand Album","date":"1999",
                      "artist-credit":[{"name":"Band","artist":{"id":"a1","name":"Band"}}],
                      "media":[{"position":1,"tracks":[{"position":1,"title":"One","recording":{"id":"r1","title":"One"}},
                                                       {"position":2,"title":"Two","recording":{"id":"r2","title":"Two"}}]}]})");
        http->add("image.tmdb.org", 200, "image/jpeg", "poster-bytes");
        auto* http_ptr = http.get();
        auto scanner =
            bench.scanner(provider_scanner_config(write_token(bench.path())), std::move(http));
        ManageApi manage(bench.node(), bench.metadata(), fs, catalogue, hints, *scanner);
        auto match = [&](const std::string& hint_id, const std::string& body) {
            auto response = manage.handle(
                request_for("POST", "/api/v1/manage/unmatched/" + hint_id + "/match", body));
            return std::pair{response.status, body_json(response)};
        };

        // A movie by TMDB id: the record, its poster and the binding.
        auto [movie_hint, movie_media] = bench.unmatched("/Movies/ref-1.mkv", 1);
        auto [status, body] = match(movie_hint, R"({"ref":"tmdb:movie:335984"})");
        REQUIRE(status == 200);
        CHECK(body.find("status")->asString() == "matched");
        CHECK(body.find("leaf_item_id")->asString() == "tmdb:movie:335984");
        auto movie = catalogue.get("tmdb:movie:335984");
        REQUIRE(movie.has_value());
        CHECK(movie->title == "Blade Runner 2049");
        CHECK(movie->media_ids == std::vector<std::string>{movie_media});
        REQUIRE(movie->artwork.size() == 1);
        CHECK(movie->artwork.front().role == "poster");
        CHECK(!hints.get(movie_hint).has_value());

        // A show by id with season and episode numbers builds show, season, episode.
        auto [episode_hint, episode_media] = bench.unmatched("/TV/ref-2.mkv", 2);
        std::tie(status, body) = match(episode_hint, R"({"ref":"tmdb:tv:1399","season_number":1,"episode_number":2})");
        REQUIRE(status == 200);
        auto episode = catalogue.get(body.find("leaf_item_id")->asString());
        REQUIRE(episode.has_value());
        CHECK(episode->title == "The Kingsroad");
        CHECK(episode->media_ids == std::vector<std::string>{episode_media});
        CHECK(catalogue.get(*episode->parent_id)->parent_id == "tmdb:tv:1399");

        // A release by MusicBrainz id with a track number.
        auto [track_hint, track_media] = bench.unmatched("/Music/ref-3.flac", 3);
        std::tie(status, body) = match(track_hint, R"({"ref":"musicbrainz:release:)" + release + R"(","track_number":2})");
        REQUIRE(status == 200);
        auto track = catalogue.get(body.find("leaf_item_id")->asString());
        REQUIRE(track.has_value());
        CHECK(track->title == "Two");
        CHECK(track->media_ids == std::vector<std::string>{track_media});
        const auto album = catalogue.get(*track->parent_id);
        REQUIRE(album.has_value());
        REQUIRE(album->artwork.size() == 1);
        CHECK(album->artwork.front().role == "cover");
        CHECK(http_ptr->requests_containing("covers.example") == 1);

        // Another track of the release: the cover this node already holds is
        // neither fetched nor stored again.
        auto [other_hint, other_media] = bench.unmatched("/Music/ref-3b.flac", 13);
        std::tie(status, body) = match(other_hint, R"({"ref":"musicbrainz:release:)" + release + R"(","track_number":1})");
        REQUIRE(status == 200);
        CHECK(catalogue.get(body.find("leaf_item_id")->asString())->title == "One");
        CHECK(http_ptr->requests_containing("covers.example") == 1);
        CHECK(catalogue.get(album->id)->artwork == album->artwork);
        // Both tracks were looked up through the provider that had the release.
        CHECK(http_ptr->requests_containing("ws/2/release/" + release) == 1);
        CHECK(http_ptr->requests_containing("coverartarchive.org/release/" + release) == 1);

        // Refusals leave the file unmatched.
        const auto refused = bench.unmatched("/Movies/ref-4.mkv", 4).first;
        struct Refusal {
            std::string body;
            int status;
            const char* code;
        };
        for (const auto& refusal : std::vector<Refusal>{
                 {R"({"ref":"imdb:tt0083658"})", 400, "bad_ref"},
                 {R"({"ref":"tmdb:tv:1399","season_number":1})", 400, "not_playable_ref"},
                 {R"({"ref":"tmdb:movie:404404"})", 404, "provider_not_found"},
                 {R"({"ref":"tmdb:tv:1399","season_number":1,"episode_number":9})", 404,
                  "provider_not_found"},
                 {R"({"ref":"tmdb:movie:77"})", 503, "provider_unavailable"}}) {
            std::tie(status, body) = match(refused, refusal.body);
            CHECK(status == refusal.status);
            CHECK(error_code(body) == refusal.code);
        }
        CHECK(hints.get(refused).has_value());

        // With no TMDB token the movie provider has no source: refused before
        // any request leaves the node.
        CatalogueScannerConfig unconfigured;
        unconfigured.movies.roots = {"/Movies"};
        auto silent = std::make_unique<FakeHttpClient>();
        auto* silent_ptr = silent.get();
        auto bare = bench.scanner(unconfigured, std::move(silent));
        ManageApi unconfigured_manage(bench.node(), bench.metadata(), fs, catalogue, hints, *bare);
        auto response = unconfigured_manage.handle(
            request_for("POST", "/api/v1/manage/unmatched/" + refused + "/match",
                        R"({"ref":"tmdb:movie:335984"})"));
        CHECK(response.status == 400);
        CHECK(error_code(body_json(response)) == "provider_not_configured");
        CHECK(silent_ptr->requests() == 0);
    }

    {
        // A file bound to two items is a conflict; one season's multi-episode
        // file is not.
        auto item = [](CatalogueKind kind, std::string id, std::vector<std::string> media,
                       std::optional<std::string> parent = {}) {
            CatalogueItem out;
            out.kind = kind;
            out.id = std::move(id);
            out.title = out.id;
            out.media_ids = std::move(media);
            if (parent)
                out.parent_id = *parent;
            return out;
        };
        catalogue.upsert_many({
            item(CatalogueKind::movie, "tmdb:movie:1", {"macha:twice"}),
            item(CatalogueKind::movie, "manual:movie:1", {"macha:twice"}),
            item(CatalogueKind::episode, "tmdb:episode:1", {"macha:double"}, "tmdb:season:9:1"),
            item(CatalogueKind::episode, "tmdb:episode:2", {"macha:double"}, "tmdb:season:9:1"),
            item(CatalogueKind::movie, "tmdb:movie:2", {"macha:once"}),
        });
        auto scanner = bench.scanner();
        ManageApi manage(bench.node(), bench.metadata(), fs, catalogue, hints, *scanner);
        auto response = manage.handle(request_for("GET", "/api/v1/manage/unmatched"));
        REQUIRE(response.status == 200);
        const auto json = body_json(response);
        const auto& conflicts = json.find("conflicts")->asArray();
        REQUIRE(conflicts.size() == 1);
        CHECK(conflicts[0].find("media_id")->asString() == "macha:twice");
        CHECK(conflicts[0].find("item_ids")->asArray().size() == 2);
    }

    {
        // A scan keeps on a manual item every file still in the namespace,
        // inside its roots or not, and drops only the deleted one.
        fs.mkdir("/Prune", 0755, getuid(), getgid());
        auto file = [&](const std::string& path, uint8_t seed) {
            write_file(fs, path, pattern(4096, seed));
            return file_media_id(fs.getattr(path));
        };
        const auto in_root = file("/Prune/a.mkv", 201);
        const auto outside_roots = file("/Other/b.mkv", 202);
        const auto deleted = file("/Prune/c.mkv", 203);
        CatalogueItem manual;
        manual.kind = CatalogueKind::movie;
        manual.id = "manual:movie:kept";
        manual.title = "Kept";
        manual.media_ids = {in_root, outside_roots, deleted};
        CatalogueItem only_deleted = manual;
        only_deleted.id = "manual:movie:emptied";
        only_deleted.media_ids = {deleted};
        catalogue.upsert_many({manual, only_deleted});
        fs.unlink("/Prune/c.mkv");

        CatalogueScannerConfig config;
        config.enabled = true;
        config.movies.roots = {"/Prune"};
        config.tv.enabled = false;
        config.music.enabled = false;
        (void)bench.scanner(config, std::make_unique<FakeHttpClient>())->scan_once();

        auto kept = catalogue.get("manual:movie:kept");
        REQUIRE(kept.has_value());
        auto expected = std::vector<std::string>{in_root, outside_roots};
        std::sort(expected.begin(), expected.end());
        auto actual = kept->media_ids;
        std::sort(actual.begin(), actual.end());
        CHECK(actual == expected);
        auto emptied = catalogue.get("manual:movie:emptied");
        REQUIRE(emptied.has_value());
        CHECK(emptied->media_ids.empty());
    }
}
Config config_for(const std::filesystem::path& path, const std::filesystem::path& key,
                  uint16_t port, std::vector<Endpoint> bootstrap = {}) {
    return macha::test_support::config_for(path, key, port, std::move(bootstrap),
                                           macha::test_support::ConfigProfile::isolated);
}

#if defined(__linux__)
std::atomic_bool track_fsync{false};
std::atomic_uint64_t fsync_calls{0};
std::atomic_uint64_t syncfs_calls{0};
#endif


// ManageApi over provider records: search says which results are already
// catalogued, artwork options are listed per role, and a choice fetches the
// full image, replaces the role's artwork and locks the item.
MACHA_TEST("invariants", test_a_titles_files_are_unmatched_and_deleted_one_call_each) {
    CatalogueBench bench("title-files");
    auto& fs = bench.fs();
    auto& catalogue = bench.catalogue();
    auto& hints = bench.hints();
    for (const auto* dir : {"/Music", "/Music/A", "/TV", "/TV/S"})
        fs.mkdir(dir, 0755, getuid(), getgid());
    write_file(fs, "/Music/A/1.flac", pattern(4096, 1));
    write_file(fs, "/Music/A/2.flac", pattern(4096, 2));
    write_file(fs, "/Music/A/copy-of-2.flac", pattern(4096, 2));
    write_file(fs, "/TV/S/e1.mkv", pattern(4096, 3));
    const auto one = file_media_id(fs.getattr("/Music/A/1.flac"));
    const auto two = file_media_id(fs.getattr("/Music/A/2.flac"));
    const auto episode_media = file_media_id(fs.getattr("/TV/S/e1.mkv"));
    REQUIRE(file_media_id(fs.getattr("/Music/A/copy-of-2.flac")) == two);

    // A hand-made artist and album, and a scanner-made show: removal does not
    // depend on who made an item.
    const auto item = [](CatalogueKind kind, std::string id, std::optional<std::string> parent,
                         std::vector<std::string> media = {}, bool scanner = false) {
        CatalogueItem out;
        out.kind = kind;
        out.id = std::move(id);
        out.title = out.id;
        out.parent_id = std::move(parent);
        out.media_ids = std::move(media);
        if (scanner) out.external_ids["macha_scanner"] = "1";
        return out;
    };
    catalogue.upsert_many({item(CatalogueKind::artist, "artist", {}),
                           item(CatalogueKind::album, "album", "artist"),
                           item(CatalogueKind::track, "track-1", "album", {one}),
                           item(CatalogueKind::track, "track-2", "album", {two}),
                           item(CatalogueKind::show, "show", {}, {}, true),
                           item(CatalogueKind::season, "season", "show", {}, true),
                           item(CatalogueKind::episode, "episode", "season", {episode_media}, true),
                           item(CatalogueKind::show, "other-show", {})});

    auto scanner = bench.scanner();
    ManageApi manage(bench.node(), bench.metadata(), fs, catalogue, hints, *scanner);
    REQUIRE(ManageApi::title_file_route(request_for("DELETE", "/api/v1/files/Music/A/1.flac")));
    REQUIRE(ManageApi::title_file_route(
        request_for("DELETE", "/api/v1/catalogue/items/track-1/media/" + one)));
    CHECK(!ManageApi::title_file_route(request_for("GET", "/api/v1/files/Music/A/1.flac")));
    CHECK(!ManageApi::title_file_route(request_for("DELETE", "/api/v1/catalogue/items/track-1")));
    const auto call = [&](const std::string& path,
                          std::map<std::string, std::string, std::less<>> query = {}) {
        auto response = manage.handle(request_for("DELETE", path, {}, std::move(query)));
        return std::pair{response.status, body_json(response)};
    };
    const auto ids = [](const Json& body) {
        std::vector<std::string> out;
        for (const auto& id : body.find("removed_item_ids")->asArray()) out.push_back(id.asString());
        return out;
    };
    const auto unmatched_paths = [&] {
        std::set<std::string> out;
        const auto listed = body_json(manage.handle(request_for("GET", "/api/v1/manage/unmatched")));
        for (const auto& entry : listed.find("items")->asArray())
            out.insert(entry.find("path")->asString());
        return out;
    };

    // A directory lists in pages, by name.
    {
        std::vector<std::string> names;
        std::map<std::string, std::string, std::less<>> query{{"path", "/Music/A"}, {"limit", "1"}};
        for (int pages = 0;; ++pages) {
            REQUIRE(pages < 5);
            const auto body = body_json(manage.handle(
                request_for("GET", "/api/v1/manage/filesystem", {}, query)));
            REQUIRE(body.find("entries")->asArray().size() == 1);
            names.push_back(body.find("entries")->asArray().front().find("name")->asString());
            if (body.find("next_cursor")->isNull()) break;
            query["cursor"] = body.find("next_cursor")->asString();
        }
        CHECK((names == std::vector<std::string>{"1.flac", "2.flac", "copy-of-2.flac"}));
    }

    // Unmatch: the file goes to the unmatched list as it is, and nothing is
    // queued for a provider. The track goes; the album keeps its other track.
    auto [status, body] = call("/api/v1/catalogue/items/track-1/media/" + one);
    REQUIRE(status == 200);
    CHECK(body.find("status")->asString() == "unmatched");
    CHECK(body.find("item") == nullptr);
    CHECK(ids(body) == std::vector<std::string>{"track-1"});
    CHECK(!catalogue.get("track-1").has_value());
    CHECK(catalogue.get("album").has_value());
    CHECK(unmatched_paths() == std::set<std::string>{"/Music/A/1.flac"});
    CHECK(hints.summary().queued == 0);
    CHECK(fs.getattr("/Music/A/1.flac").size == 4096);
    // A scan that finds the same file does not reopen it.
    hints.submit("/Music/A/1.flac", "scanner", one, CatalogueHintPriority::periodic_scan);
    CHECK(hints.summary().queued == 0);
    CHECK(unmatched_paths() == std::set<std::string>{"/Music/A/1.flac"});

    std::tie(status, body) = call("/api/v1/catalogue/items/track-2/media/" + one);
    CHECK(status == 404);
    CHECK(error_code(body) == "media_not_bound");
    std::tie(status, body) = call("/api/v1/catalogue/items/missing/media/" + one);
    CHECK(status == 404);
    CHECK(error_code(body) == "not_found");

    // One path: the content is still held by its copy, so the track stays.
    std::tie(status, body) = call("/api/v1/files/Music/A/2.flac");
    REQUIRE(status == 200);
    CHECK(body.find("path")->asString() == "/Music/A/2.flac");
    CHECK(ids(body).empty());
    CHECK(catalogue.get("track-2")->media_ids == std::vector<std::string>{two});

    // Every path of the content: the last track goes, and the album and
    // artist left with no children go with it.
    std::tie(status, body) = call("/api/v1/files", {{"hash", two}});
    REQUIRE(status == 200);
    CHECK(body.find("paths")->asArray().size() == 1);
    CHECK((ids(body) == std::vector<std::string>{"album", "artist", "track-2"}));
    CHECK(fs.media_paths(two).empty());
    std::tie(status, body) = call("/api/v1/files", {{"hash", two}});
    CHECK(status == 404);

    // The last episode's file: episode, season and show go; another show stays.
    std::tie(status, body) = call("/api/v1/files/TV/S/e1.mkv");
    REQUIRE(status == 200);
    CHECK((ids(body) == std::vector<std::string>{"episode", "season", "show"}));
    CHECK(catalogue.get("other-show").has_value());

    std::tie(status, body) = call("/api/v1/files/TV/S");
    CHECK(status == 409);
    CHECK(error_code(body) == "not_a_file");
    std::tie(status, body) = call("/api/v1/files/TV/S/missing.mkv");
    CHECK(status == 404);
    std::tie(status, body) = call("/api/v1/files");
    CHECK(status == 400);
    CHECK(error_code(body) == "missing_hash");

    // Clear Metadata: the item and everything beneath it go, and the file
    // goes to the unmatched list as it is. No provider is asked, which would
    // only match it badly again.
    write_file(fs, "/Music/A/clear.flac", pattern(4096, 9));
    const auto clear_media = file_media_id(fs.getattr("/Music/A/clear.flac"));
    catalogue.upsert_many({item(CatalogueKind::album, "album-2", {}),
                           item(CatalogueKind::track, "track-9", "album-2", {clear_media}, true)});
    std::tie(status, body) = [&] {
        auto response = manage.handle(request_for("DELETE", "/api/v1/catalogue/items/album-2/metadata"));
        return std::pair{response.status, Json(Json::Object{})};
    }();
    CHECK(status == 204);
    CHECK(!catalogue.get("album-2").has_value());
    CHECK(!catalogue.get("track-9").has_value());
    CHECK(unmatched_paths().contains("/Music/A/clear.flac"));
    CHECK(hints.summary().queued == 0);
    std::tie(status, body) = call("/api/v1/catalogue/items/missing/metadata");
    CHECK(status == 404);
}

MACHA_TEST("invariants", test_provider_search_and_artwork_choice) {
    CatalogueBench bench("manage-providers");
    auto& catalogue = bench.catalogue();
    const std::string release = "0f9a7b22-3c3e-4f5e-9d1a-2b8e6f7c5d41";
    auto item = [](CatalogueKind kind, std::string id, std::optional<std::string> parent = {}) {
        CatalogueItem out;
        out.kind = kind;
        out.id = std::move(id);
        out.title = out.id;
        if (parent)
            out.parent_id = *parent;
        return out;
    };
    auto movie = item(CatalogueKind::movie, "tmdb:movie:335984");
    movie.title = "Blade Runner 2049";
    auto season = item(CatalogueKind::season, "tmdb:season:1399:1", "tmdb:tv:1399");
    season.season_number = 1;
    auto episode = item(CatalogueKind::episode, "tmdb:episode:63057", season.id);
    episode.season_number = 1;
    episode.episode_number = 2;
    auto album = item(CatalogueKind::album, "musicbrainz:album:g1");
    album.external_ids["musicbrainz_release"] = release;
    catalogue.upsert_many({movie, item(CatalogueKind::show, "tmdb:tv:1399"), season, episode,
                           album, item(CatalogueKind::movie, "manual:movie:x")});

    auto http = std::make_unique<FakeHttpClient>();
    auto* http_ptr = http.get();
    http->add("/search/movie", 200, "application/json",
              R"({"results":[{"id":335984,"title":"Blade Runner 2049","release_date":"2017-10-04","overview":"K."},
                             {"id":78,"title":"Blade Runner","release_date":"1982-06-25"},
                             {"id":79,"title":"Blade Runner Black Out"}]})");
    http->add("/search/tv", 500, "text/plain", "down");
    http->add("musicbrainz.org/ws/2/release?", 200, "application/json",
              R"({"releases":[{"id":")" + release + R"(","title":"Hand Album","date":"1999-02-01",
                               "artist-credit":[{"name":"Band","artist":{"id":"a1","name":"Band"}}],
                               "release-group":{"id":"g1"}}]})");
    http->add("/movie/335984/images", 200, "application/json",
              R"({"posters":[{"file_path":"/a.jpg","width":1000,"height":1500,"iso_639_1":"en"},
                             {"file_path":"/b.jpg","width":2000,"height":3000,"iso_639_1":null}],
                  "backdrops":[{"file_path":"/c.jpg","width":1920,"height":1080}]})");
    http->add("/tv/1399/season/1/episode/2/images", 200, "application/json",
              R"({"stills":[{"file_path":"/s.jpg","width":1280,"height":720}]})");
    http->add("/movie/404404/images", 404, "application/json", "{}");
    http->add("image.tmdb.org", 200, "image/jpeg", "tmdb-image");
    http->add("-500.jpg", 200, "image/jpeg", "cover-image");
    http->add("coverartarchive.org/release/" + release, 200, "application/json",
              R"({"images":[{"id":36041390393,"front":true,"types":["Front"],
                             "image":"https://coverartarchive.org/release/)" + release + R"(/36041390393.jpg",
                             "thumbnails":{"250":"https://coverartarchive.org/release/)" + release + R"(/36041390393-250.jpg",
                                           "500":"https://coverartarchive.org/release/)" + release + R"(/36041390393-500.jpg"}},
                            {"id":2,"front":false,"types":["Back"],"image":"https://coverartarchive.org/x/2.jpg"}]})");
    auto scanner = bench.scanner(provider_scanner_config(write_token(bench.path())), std::move(http));
    ManageApi manage(bench.node(), bench.metadata(), bench.fs(), catalogue, bench.hints(),
                     *scanner);
    const auto get = [&](const char* path, std::map<std::string, std::string, std::less<>> query) {
        auto response = manage.handle(request_for("GET", path, {}, std::move(query)));
        return std::pair{response.status, body_json(response)};
    };
    const auto choose = [&](std::string_view body) {
        auto response =
            manage.handle(request_for("POST", "/api/v1/manage/providers/artwork/choose", body));
        return std::pair{response.status, body_json(response)};
    };
    constexpr const char* search = "/api/v1/manage/providers/search";
    constexpr const char* artwork = "/api/v1/manage/providers/artwork";

    auto [status, body] =
        get(search, {{"q", "Blade Runner"}, {"kind", "movie"}, {"year", "2017"}, {"limit", "2"}});
    REQUIRE(status == 200);
    CHECK(body.find("status")->asString() == "ok");
    {
        const auto& results = body.find("results")->asArray();
        REQUIRE(results.size() == 2);
        CHECK(results[0].find("ref")->asString() == "tmdb:movie:335984");
        CHECK(results[0].find("provider")->asString() == "tmdb");
        CHECK(results[0].find("year")->asInt64() == 2017);
        CHECK(results[0].find("overview")->asString() == "K.");
        CHECK(results[0].find("catalogue_item_id")->asString() == "tmdb:movie:335984");
        CHECK(results[1].find("catalogue_item_id") == nullptr);
        CHECK(http_ptr->requests_containing("primary_release_year=2017") == 1);
    }
    std::tie(status, body) = get(search, {{"q", "Hand Album"}, {"kind", "album"}, {"artist", "Band"}});
    REQUIRE(status == 200);
    {
        const auto& albums = body.find("results")->asArray();
        REQUIRE(albums.size() == 1);
        CHECK(albums[0].find("ref")->asString() == "musicbrainz:release:" + release);
        CHECK(albums[0].find("artist")->asString() == "Band");
        CHECK(albums[0].find("year")->asInt64() == 1999);
    }

    std::tie(status, body) = get(artwork, {{"ref", "tmdb:movie:335984"}, {"role", "poster"}});
    REQUIRE(status == 200);
    {
        const auto& posters = body.find("options")->asArray();
        REQUIRE(posters.size() == 2);
        CHECK(posters[0].find("option_id")->asString() == "/a.jpg");
        CHECK(posters[0].find("width")->asInt64() == 1000);
        CHECK(posters[0].find("language")->asString() == "en");
        CHECK(posters[1].find("language")->isNull());
        CHECK(posters[0].find("preview_url")->asString() == "https://image.tmdb.org/t/p/w185/a.jpg");
        CHECK(posters[0].find("url") == nullptr);
    }
    std::tie(status, body) = get(artwork, {{"ref", "musicbrainz:release:" + release}, {"role", "cover"}});
    REQUIRE(status == 200);
    REQUIRE(body.find("options")->asArray().size() == 1);
    CHECK(body.find("options")->asArray()[0].find("option_id")->asString() == "36041390393");

    // Choosing fetches the full image, replaces the role and locks the item.
    std::tie(status, body) = choose(R"({"item_id":"tmdb:movie:335984","role":"poster","option_id":"/b.jpg"})");
    REQUIRE(status == 200);
    CHECK(body.find("status")->asString() == "chosen");
    auto chosen = catalogue.get("tmdb:movie:335984");
    REQUIRE(chosen->artwork.size() == 1);
    CHECK(chosen->artwork.front().role == "poster");
    CHECK(chosen->external_ids.at("macha_metadata_locked") == "1");
    CHECK(http_ptr->requests_containing("/b.jpg") == 1);
    // An episode's reference is its show's, with its own numbers.
    std::tie(status, body) = choose(R"({"item_id":"tmdb:episode:63057","role":"still","option_id":"/s.jpg","lock":false})");
    REQUIRE(status == 200);
    auto chosen_episode = catalogue.get("tmdb:episode:63057");
    REQUIRE(chosen_episode->artwork.size() == 1);
    CHECK(!chosen_episode->external_ids.contains("macha_metadata_locked"));
    // An album's reference is its release.
    std::tie(status, body) = choose(R"({"item_id":"musicbrainz:album:g1","role":"cover","option_id":"36041390393"})");
    REQUIRE(status == 200);
    CHECK(catalogue.get("musicbrainz:album:g1")->artwork.size() == 1);
    CHECK(http_ptr->requests_containing("36041390393-500.jpg") == 1);
    // A manual item needs a reference given with the choice.
    std::tie(status, body) = choose(R"({"item_id":"manual:movie:x","role":"poster","option_id":"/a.jpg","ref":"tmdb:movie:335984"})");
    CHECK(status == 200);

    struct Refusal {
        const char* method;
        std::map<std::string, std::string, std::less<>> query;
        std::string body;
        int status;
        const char* code;
    };
    const std::vector<Refusal> refusals{
        {search, {{"q", "Thrones"}, {"kind", "show"}}, {}, 503, "provider_unavailable"},
        {search, {{"q", "x"}, {"kind", "episode"}}, {}, 400, "bad_kind"},
        {search, {{"kind", "movie"}}, {}, 400, "bad_query"},
        {search, {{"q", "x"}, {"kind", "movie"}, {"limit", "0"}}, {}, 400, "bad_limit"},
        {artwork, {{"ref", "tmdb:movie:335984"}, {"role", "still"}}, {}, 400, "bad_role"},
        {artwork, {{"ref", "tmdb:movie:404404"}, {"role", "poster"}}, {}, 404, "provider_not_found"},
        {nullptr, {}, R"({"item_id":"tmdb:movie:335984","role":"poster","option_id":"/elsewhere.jpg"})",
         404, "option_not_found"},
        {nullptr, {}, R"({"item_id":"manual:movie:x","role":"poster","option_id":"/a.jpg"})", 400,
         "no_provider_ref"},
    };
    for (const auto& refusal : refusals) {
        std::tie(status, body) = refusal.method ? get(refusal.method, refusal.query)
                                                : choose(refusal.body);
        CHECK(status == refusal.status);
        CHECK(error_code(body) == refusal.code);
    }
}

// ManageApi over a MusicBrainz release's tracks: the release's own order, the
// track's title, a null length where MusicBrainz gives none, and the provider
// routes' codes for each refusal.
MACHA_TEST("invariants", test_provider_release_tracks) {
    CatalogueBench bench("manage-release-tracks");
    const std::string release = "0f9a7b22-3c3e-4f5e-9d1a-2b8e6f7c5d41";
    const std::string other = "11111111-2222-4333-8444-555555555555";
    const auto tracks_path = [](std::string_view id) {
        return "/api/v1/manage/providers/musicbrainz/releases/" + std::string(id) + "/tracks";
    };
    const auto get = [&](ManageApi& manage, const std::string& path) {
        auto response = manage.handle(request_for("GET", path));
        return std::pair{response.status, body_json(response)};
    };

    {
        auto http = std::make_unique<FakeHttpClient>();
        auto* http_ptr = http.get();
        http->add("musicbrainz.org/ws/2/release/" + release, 200, "application/json",
                  R"JSON({"id":")JSON" + release + R"JSON(","title":"Two Discs","media":[
                      {"position":1,"tracks":[
                          {"position":1,"title":"Opening","length":215000,
                           "recording":{"id":"aaaaaaaa-0000-4000-8000-000000000001",
                                        "title":"Opening (album version)","length":216000}},
                          {"position":2,"title":"Untimed","length":null,
                           "recording":{"id":"aaaaaaaa-0000-4000-8000-000000000002",
                                        "title":"Untimed","length":null}}]},
                      {"position":2,"tracks":[
                          {"position":1,"title":"Second Disc","length":90500,
                           "recording":{"id":"aaaaaaaa-0000-4000-8000-000000000003",
                                        "title":"Second Disc"}}]}]})JSON");
        auto scanner = bench.scanner(provider_scanner_config(write_token(bench.path())),
                                     std::move(http));
        ManageApi manage(bench.node(), bench.metadata(), bench.fs(), bench.catalogue(),
                         bench.hints(), *scanner);

        auto [status, body] = get(manage, tracks_path(release));
        REQUIRE(status == 200);
        CHECK(body.find("status")->asString() == "ok");
        CHECK(body.asObject().size() == 2);
        const auto& tracks = body.find("tracks")->asArray();
        REQUIRE(tracks.size() == 3);
        struct Expected {
            int64_t disc;
            int64_t track;
            const char* title;
            std::optional<int64_t> length_ms;
            const char* recording_id;
        };
        const std::vector<Expected> expected{
            {1, 1, "Opening", 215000, "aaaaaaaa-0000-4000-8000-000000000001"},
            {1, 2, "Untimed", std::nullopt, "aaaaaaaa-0000-4000-8000-000000000002"},
            {2, 1, "Second Disc", 90500, "aaaaaaaa-0000-4000-8000-000000000003"},
        };
        for (size_t i = 0; i < expected.size(); ++i) {
            std::cerr << "row: " << expected[i].title << "\n";
            CHECK(tracks[i].asObject().size() == 5);
            CHECK(tracks[i].find("disc_number")->asInt64() == expected[i].disc);
            CHECK(tracks[i].find("track_number")->asInt64() == expected[i].track);
            CHECK(tracks[i].find("title")->asString() == expected[i].title);
            if (expected[i].length_ms)
                CHECK(tracks[i].find("length_ms")->asInt64() == *expected[i].length_ms);
            else
                CHECK(tracks[i].find("length_ms")->isNull());
            CHECK(tracks[i].find("recording_id")->asString() == expected[i].recording_id);
        }
        CHECK(http_ptr->requests() == 1);

        // A malformed id is refused before any request to MusicBrainz.
        for (const char* malformed : {"not-an-mbid", "0f9a7b22-3c3e-4f5e-9d1a-2b8e6f7c5d4",
                                      "0f9a7b22_3c3e_4f5e_9d1a_2b8e6f7c5d41"}) {
            std::tie(status, body) = get(manage, tracks_path(malformed));
            CHECK(status == 400);
            CHECK(error_code(body) == "bad_ref");
        }
        CHECK(http_ptr->requests() == 1);

        // Only MusicBrainz releases are a resource here, and only to read.
        std::tie(status, body) =
            get(manage, "/api/v1/manage/providers/tmdb/releases/" + release + "/tracks");
        CHECK(status == 404);
        CHECK(error_code(body) == "not_found");
        CHECK(manage.handle(request_for("POST", tracks_path(release), "{}")).status == 404);

        // Reading it makes the node call MusicBrainz: a manager's request.
        HttpRequest request;
        request.method = "GET";
        request.path = tracks_path(release);
        CHECK(Service::required_role(request) == role_manager);
    }
    {
        // MusicBrainz has no such release.
        auto http = std::make_unique<FakeHttpClient>();
        auto scanner = bench.scanner(provider_scanner_config(write_token(bench.path())),
                                     std::move(http));
        ManageApi manage(bench.node(), bench.metadata(), bench.fs(), bench.catalogue(),
                         bench.hints(), *scanner);
        auto [status, body] = get(manage, tracks_path(release));
        CHECK(status == 404);
        CHECK(error_code(body) == "provider_not_found");
    }
    {
        // One rate-limit answer is waited out: the editor's request tries
        // again after the Retry-After MusicBrainz gave, and succeeds.
        auto http = std::make_unique<FakeHttpClient>();
        auto* http_ptr = http.get();
        http->add_once("musicbrainz.org/ws/2/release/", 503, "slow down", std::chrono::seconds(1));
        http->add("musicbrainz.org/ws/2/release/" + release, 200, "application/json",
                  R"JSON({"id":")JSON" + release + R"JSON(","title":"Once","media":[
                      {"position":1,"tracks":[{"position":1,"title":"Only",
                       "recording":{"id":"aaaaaaaa-0000-4000-8000-000000000009","title":"Only"}}]}]})JSON");
        auto scanner = bench.scanner(provider_scanner_config(write_token(bench.path())),
                                     std::move(http));
        ManageApi manage(bench.node(), bench.metadata(), bench.fs(), bench.catalogue(),
                         bench.hints(), *scanner);
        const auto started = std::chrono::steady_clock::now();
        auto [status, body] = get(manage, tracks_path(release));
        CHECK(status == 200);
        CHECK(http_ptr->requests() == 2);
        CHECK(std::chrono::steady_clock::now() - started >= 1s);
    }
    {
        // MusicBrainz down: the editor waits out the first backoff (2 s) and
        // tries once more, then is refused with how long to wait; the next
        // request waits out a backoff it can (4 s) and tries once; past that
        // (8 s) a request is refused without asking. The client is told the
        // provider is unavailable and when to try again, never why.
        auto http = std::make_unique<FakeHttpClient>();
        auto* http_ptr = http.get();
        http->add("musicbrainz.org/ws/2/release/", 503, "text/plain", "down");
        auto scanner = bench.scanner(provider_scanner_config(write_token(bench.path())),
                                     std::move(http));
        ManageApi manage(bench.node(), bench.metadata(), bench.fs(), bench.catalogue(),
                         bench.hints(), *scanner);
        const auto refused = [&](const std::string& id) {
            const auto response = manage.handle(request_for("GET", tracks_path(id)));
            const auto body = body_json(response);
            CHECK(response.status == 503);
            CHECK(error_code(body) == "provider_unavailable");
            CHECK(error_message(body) == "Provider unavailable");
            REQUIRE(body.find("error")->find("retry_after_ms") != nullptr);
            CHECK(response.headers.contains("Retry-After"));
            return body.find("error")->find("retry_after_ms")->asUInt64();
        };
        CHECK(refused(release) == 4000);
        CHECK(http_ptr->requests() == 2);
        CHECK(refused(other) == 8000);
        CHECK(http_ptr->requests() == 3);
        const auto left = refused(release);
        CHECK(left > 5000);
        CHECK(left <= 8000);
        CHECK(http_ptr->requests() == 3);
    }
    {
        auto config = provider_scanner_config(write_token(bench.path()));
        config.music.musicbrainz.enabled = false;
        auto http = std::make_unique<FakeHttpClient>();
        auto* http_ptr = http.get();
        auto scanner = bench.scanner(std::move(config), std::move(http));
        ManageApi manage(bench.node(), bench.metadata(), bench.fs(), bench.catalogue(),
                         bench.hints(), *scanner);
        auto [status, body] = get(manage, tracks_path(release));
        CHECK(status == 400);
        CHECK(error_code(body) == "provider_not_configured");
        CHECK(http_ptr->requests() == 0);
    }
}

// Resetting an identity association: the stale NodeId leaves membership at
// once and pre-reset gossip cannot bring it back, while a different NodeId
// may own the endpoint; a reset by IP covers every port. The reset is
// admitted on the local tombstone alone: it does not wait for writable
// metadata or a metadata mutation in progress, and the audit record follows.
MACHA_TEST("invariants", test_identity_association_reset) {
    const auto members_without = [](BareNode& node, const NodeId& id) {
        const auto all = node.membership().all();
        return std::none_of(all.begin(), all.end(),
                            [&](const NodeInfo& member) { return member.id == id; });
    };
    const auto stale_peer = [](std::string host, uint16_t port) {
        NodeInfo stale;
        stale.id = random_node_id();
        stale.host = std::move(host);
        stale.port = port;
        stale.failure_domain = "test";
        stale.seen_unix_ms = unix_ms();
        return stale;
    };

    {
        CatalogueBench bench("identity-reset");
        auto& node = bench.node();
        auto& metadata = bench.metadata();
        // A lone node's replica set, loaded and validated as its maintenance
        // pass would.
        (void)metadata.snapshot();
        metadata.note_replica_validation(true);
        REQUIRE(metadata.cluster_status().availability == MetadataAvailability::writable);
        auto scanner = bench.scanner();
        ManageApi manage(node, metadata, bench.fs(), bench.catalogue(), bench.hints(), *scanner);

        auto root = manage.handle(request_for("GET", "/api/v1/manage"));
        REQUIRE(root.status == 200);
        const auto root_json = body_json(root);
        CHECK(root_json.find("api")->asString() == "manage");
        CHECK(root_json.find("actions")->find("identity_association_reset") != nullptr);
        CHECK(root_json.find("actions")->find("node_identity_association_reset") != nullptr);

        auto stale = stale_peer("10.44.1.50", 57401);
        node.membership().observe(stale, true);
        PersistedNodeStatus durable;
        durable.observed_unix_ms = unix_ms();
        durable.host = stale.host;
        durable.port = stale.port;
        durable.failure_domain = stale.failure_domain;
        metadata.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            snapshot.node_status[stale.id] = durable;
            delta.upsert_node_status[stale.id] = durable;
        });

        // Hold a metadata mutation open: admission must not wait for it.
        TestGate mutation_gate;
        std::jthread blocker([&] {
            metadata.mutate_delta(
                [&](MetadataSnapshot&, MetadataDelta&) { mutation_gate.enter_and_wait(); });
        });
        REQUIRE(mutation_gate.wait_for_entries(1));
        const auto began = Clock::now();
        auto reset = manage.handle(request_for(
            "POST", "/api/v1/manage/nodes/" + to_string(stale.id) + "/identity-association/reset",
            R"({"host":"10.44.1.50","port":57401,"reason":"test endpoint reassignment"})"));
        CHECK(Clock::now() - began < 500ms);
        REQUIRE(reset.status == 202);
        const auto reset_json = body_json(reset);
        const auto& reset_value = *reset_json.find("reset");
        CHECK(reset_value.find("stale_node_id")->asString() == to_string(stale.id));
        CHECK(reset_value.find("epoch")->asUInt64() == 1);
        CHECK(reset_value.find("scope")->asString() == "[10.44.1.50]:57401");
        CHECK(reset_json.find("audit_state")->asString() == "queued");
        CHECK(!reset_json.find("metadata_persisted")->asBool());
        CHECK(members_without(node, stale.id));
        mutation_gate.open();
        blocker.join();

        // Re-gossiping the pre-reset association cannot resurrect it; a
        // different authenticated NodeId may own the same endpoint at once.
        node.membership().observe(stale, false);
        CHECK(members_without(node, stale.id));
        auto replacement = stale;
        replacement.id = random_node_id();
        node.membership().observe(replacement, true);
        CHECK(!members_without(node, replacement.id));

        const auto key = identity_reset_key(stale.host, stale.port);
        REQUIRE(wait_until([&] { return metadata.snapshot().identity_resets.contains(key); }));
        const auto audited = metadata.snapshot();
        CHECK(audited.node_status.contains(stale.id));
        CHECK(audited.identity_resets.at(key).stale_node_id == stale.id);
        CHECK(audited.identity_resets.at(key).reason == "test endpoint reassignment");

        // By IP alone: no NodeId, every port on the host.
        auto second_port = replacement;
        second_port.id = random_node_id();
        second_port.port = 57402;
        node.membership().observe(second_port, true);
        auto unrelated = replacement;
        unrelated.id = random_node_id();
        unrelated.host = "10.44.1.51";
        node.membership().observe(unrelated, true);
        auto reset_ip = manage.handle(request_for("POST", "/api/v1/manage/identity-associations/reset",
                                                  R"({"host":"10.44.1.50","reason":"clear by ip"})"));
        REQUIRE(reset_ip.status == 202);
        const auto reset_ip_json = body_json(reset_ip);
        const auto& reset_ip_value = *reset_ip_json.find("reset");
        CHECK(reset_ip_value.find("scope")->asString() == "[10.44.1.50]:*");
        CHECK(reset_ip_value.find("port")->isNull());
        CHECK(reset_ip_value.find("stale_node_id")->isNull());
        const auto reset_ip_at = reset_ip_value.find("reset_at_unix_ms")->asUInt64();
        const auto after_ip = node.membership().all();
        CHECK(std::none_of(after_ip.begin(), after_ip.end(),
                           [&](const NodeInfo& member) { return member.host == "10.44.1.50"; }));
        CHECK(!members_without(node, unrelated.id));

        // Pre-reset gossip cannot recreate an association for that IP, but
        // post-reset authentication can establish a replacement identity.
        replacement.seen_unix_ms = reset_ip_at ? reset_ip_at - 1 : 0;
        node.membership().observe(replacement, false);
        CHECK(members_without(node, replacement.id));
        auto fresh = replacement;
        fresh.id = random_node_id();
        fresh.seen_unix_ms = reset_ip_at + 1;
        node.membership().observe(fresh, true);
        CHECK(!members_without(node, fresh.id));

        const auto ip_key = identity_reset_key("10.44.1.50", 0);
        REQUIRE(wait_until([&] { return metadata.snapshot().identity_resets.contains(ip_key); }));
        const auto ip_audited = metadata.snapshot();
        CHECK(ip_audited.identity_resets.at(ip_key).stale_node_id == NodeId{});
        CHECK(ip_audited.identity_resets.at(ip_key).reason == "clear by ip");
    }

    {
        // Metadata needs two writers and has one: the reset still succeeds on
        // the durable local tombstone, breaking the cycle in which an
        // unreachable stale identity keeps metadata unavailable.
        CatalogueBench bench("identity-reset-unavailable", 2);
        auto& node = bench.node();
        auto scanner = bench.scanner();
        ManageApi manage(node, bench.metadata(), bench.fs(), bench.catalogue(), bench.hints(),
                         *scanner);
        const auto stale = stale_peer("10.44.1.50", 7437);
        node.membership().observe(stale, true);
        REQUIRE(node.membership().all().size() == 2);

        auto response = manage.handle(request_for(
            "POST", "/api/v1/manage/nodes/" + to_string(stale.id) + "/identity-association/reset",
            R"({"reason":"recover unavailable metadata"})"));
        REQUIRE(response.status == 202);
        const auto value = body_json(response);
        CHECK(!value.find("metadata_persisted")->asBool());
        CHECK(value.find("metadata_generation")->isNull());
        CHECK(value.find("persistence_error")->isNull());
        CHECK(value.find("audit_state")->asString() == "queued");
        CHECK(value.find("reset")->find("stale_node_id")->asString() == to_string(stale.id));
        CHECK(members_without(node, stale.id));
        const auto resets = node.identity_resets();
        REQUIRE(resets.size() == 1);
        CHECK(resets.front().host == stale.host);
        CHECK(resets.front().port == stale.port);
    }
}

// Kept integrated: the order is Service::start's, observed through its own
// HTTP server and role gate.
//
// Health identifies Macha without a token in every state; Status answers
// before the control plane is up and while storage recovers, when ordinary
// routes say the node is recovering; Status needs view_status, which every
// capability implies and which grants no media.
MACHA_TEST("invariants", test_service_answers_health_and_status_through_startup) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("startup-order");
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();
    const auto port = config.catalogue.api.port;

    TestGate control_gate;
    TestGate recovery_gate;
    Service service(config, cluster.keys(), test_durability_window, [&](std::string_view stage) {
        if (stage == "control-plane")
            control_gate.enter_and_wait();
        if (stage == "data-storage" || stage == "control-storage")
            recovery_gate.enter_and_wait();
    });
    struct ReleaseGates {
        TestGate& control;
        TestGate& recovery;
        ~ReleaseGates() {
            control.open();
            recovery.open();
        }
    } release{control_gate, recovery_gate};
    std::jthread starter([&] { service.start(); });
    REQUIRE(control_gate.wait_for_entries(1));

    const auto status_json = [&] {
        const auto response = raw_http_get(port, "/api/v1/status", bearer_header(service));
        CHECK(response.find("HTTP/1.1 200") != std::string::npos);
        const auto body_at = response.find("\r\n\r\n");
        REQUIRE(body_at != std::string::npos);
        return Json::parse(response.substr(body_at + 4));
    };
    const auto available = [](const Json& diagnostics, const char* section) {
        return diagnostics.find("diagnostics")->find(section)->find("available")->asBool();
    };

    // Starting: health is 503 and still identifies Macha; Status, both
    // halves, already answers.
    {
        const auto health = raw_http_get(port, "/api/v1/health");
        CHECK(health.find("HTTP/1.1 503") != std::string::npos);
        CHECK(health.find("Content-Type: application/json") != std::string::npos);
        CHECK(health.find("\"service\":\"macha\"") != std::string::npos);
        CHECK(health.find("\"status\":\"starting\"") != std::string::npos);
        const auto startup = *status_json().find("startup");
        CHECK(startup.find("phase")->asString() == "starting");
        CHECK(startup.find("api")->asString() == "ready");
        CHECK(startup.find("control_plane")->asString() == "starting");
        const auto diagnostics = status_diagnostics_response(port, service);
        CHECK(!available(diagnostics, "data_store"));
        CHECK(available(diagnostics, "convergence"));
        CHECK(!available(diagnostics, "filesystem"));
    }

    // Control plane up, storage recovering: Status says so, ordinary routes
    // refuse with a reason.
    control_gate.open();
    starter.join();
    REQUIRE(recovery_gate.wait_for_entries(2));
    {
        const auto startup = *status_json().find("startup");
        CHECK(startup.find("phase")->asString() == "recovering");
        CHECK(startup.find("api")->asString() == "ready");
        CHECK(startup.find("control_plane")->asString() == "ready");
        CHECK(startup.find("data_storage")->asString() == "recovering");
        CHECK(startup.find("control_storage")->asString() == "recovering");
        CHECK(!available(status_diagnostics_response(port, service), "data_store"));
        CHECK(!service.ready());
        const auto ordinary = raw_http_get(port, "/api/v1/catalogue/status", bearer_header(service));
        CHECK(ordinary.find("HTTP/1.1 503") != std::string::npos);
        CHECK(ordinary.find("service_recovering") != std::string::npos);
    }

    recovery_gate.open();
    REQUIRE(wait_until([&] { return service.ready(); }, 10s));
    CHECK(status_json().find("startup")->find("phase")->asString() == "ready");

    // Once a maintenance pass has run, Status says what it did with repair.
    {
        const auto repair = [&] {
            return *status_diagnostics_response(port, service).find("diagnostics")->find("repair");
        };
        REQUIRE(wait_until([&] { return repair().find("pace")->asString() != "unknown"; }, 10s));
        const auto now = repair();
        const auto pace = now.find("pace")->asString();
        CHECK((pace == "running" || pace == "paced" || pace == "settling" ||
               pace == "awaiting_credit"));
        CHECK((pace == "paced") == !now.find("paced_by")->asArray().empty());
    }

    // Serving: 200 and the same marker, still without a token; the cluster's
    // shape needs one. The probe works cross-origin too.
    const auto health = raw_http_get(port, "/api/v1/health");
    CHECK(health.find("HTTP/1.1 200") != std::string::npos);
    CHECK(health.find("\"service\":\"macha\"") != std::string::npos);
    CHECK(health.find("\"status\":\"ok\"") != std::string::npos);
    CHECK(health.find("\"version\":\"") != std::string::npos);
    CHECK(health.find("node") == std::string::npos);
    CHECK(health.find("capacity") == std::string::npos);
    CHECK(health.find("Access-Control-Allow-Origin: *") != std::string::npos);

    const auto token_for = [&](std::vector<std::string> roles) {
        auto minted = service.accounts().sessions().create(expand_roles(roles));
        REQUIRE(minted.has_value());
        return std::map<std::string, std::string>{
            {"Authorization", "Bearer " + minted->bearer_token}};
    };
    const auto refused = raw_http_get(port, "/api/v1/status", token_for({}));
    CHECK(refused.find("HTTP/1.1 403") != std::string::npos);
    CHECK(refused.find("view_status") != std::string::npos);
    CHECK(raw_http_get(port, "/api/v1/status", token_for({std::string(role_view_status)}))
              .find("HTTP/1.1 200") != std::string::npos);
    CHECK(raw_http_get(port, "/api/v1/catalogue/items", token_for({std::string(role_view_status)}))
              .find("HTTP/1.1 403") != std::string::npos);
    CHECK(raw_http_get(port, "/api/v1/status", token_for({std::string(role_importer)}))
              .find("HTTP/1.1 200") != std::string::npos);

    service.stop();
}

// Kept integrated: a peer reaching a node whose local state is still
// recovering, over real RPC and telemetry gossip.
//
// The recovering node's control plane answers ping and membership at once,
// and the peer's Status shows it online and recovering, with no fabricated
// zero capacity, as a cluster condition.
MACHA_TEST("invariants", test_a_recovering_node_answers_peers_and_is_shown_recovering) {
    TestCluster cluster(ConfigProfile::isolated);
    auto recovering_config = cluster.node_config("recovering-node");
    auto peer_config = cluster.node_config("recovering-peer");

    TestGate recovery_gate;
    BareNode recovering(recovering_config, cluster.keys(), [&](std::string_view stage) {
        if (stage == "data-storage" || stage == "control-storage")
            recovery_gate.enter_and_wait();
    });
    struct ReleaseGate {
        TestGate& gate;
        ~ReleaseGate() { gate.open(); }
    } release{recovery_gate};

    recovering.start();
    REQUIRE(recovery_gate.wait_for_entries(2));
    CHECK(recovering.readiness().control_plane_online);
    CHECK(!recovering.readiness().local_state_ready);

    BareNode peer(peer_config, cluster.keys());
    peer.start();
    REQUIRE(peer.wait_local_state_ready(10s));

    const Endpoint recovering_endpoint{"127.0.0.1", recovering_config.port};
    CHECK(peer.call(recovering_endpoint, MessageType::ping).message.type == MessageType::ok);
    CHECK(peer.call(recovering_endpoint, MessageType::members).message.type ==
          MessageType::members_reply);
    REQUIRE(wait_until([&] {
        const auto all = recovering.membership().all();
        return std::any_of(all.begin(), all.end(),
                           [&](const NodeInfo& node) { return node.id == peer.node_id(); });
    }));

    // Fresh telemetry on the peer reporting the non-ready phase.
    REQUIRE(wait_until([&] {
        const auto views = peer.telemetry().views(std::chrono::milliseconds(60000));
        return std::any_of(views.begin(), views.end(), [&](const TelemetryView& view) {
            return view.telemetry.node_id == recovering.node_id() &&
                   view.telemetry.phase != NodePhase::ready;
        });
    }, 10s));

    ClusterStatusService status(peer, peer.accounts(), peer.resources.activity,
                                peer.resources.data, peer.resources.memory);
    auto response = status.handle(request_for("GET", "/api/v1/status"));
    REQUIRE(response.status == 200);
    const auto root = body_json(response);
    bool found = false;
    for (const auto& value : root.find("nodes")->asArray()) {
        if (value.find("id")->asString() != to_string(recovering.node_id()))
            continue;
        found = true;
        CHECK(value.find("state")->asString() == "online");
        CHECK(value.find("phase")->asString() == "recovering");
        // A not-yet-ready sample's zero capacity/usage is not a measurement.
        CHECK(!value.find("storage")->find("available")->asBool());
    }
    CHECK(found);
    const auto* cluster_json = root.find("cluster");
    REQUIRE(cluster_json != nullptr);
    bool saw_condition = false;
    for (const auto& condition : cluster_json->find("conditions")->asArray())
        if (condition.asString() == "one or more online nodes are still recovering")
            saw_condition = true;
    CHECK(saw_condition);
    CHECK(cluster_json->find("health")->asString() != "healthy");

    recovery_gate.open();
    REQUIRE(recovering.wait_local_state_ready(10s));
    peer.stop();
    recovering.stop();
}

// Status is polled; diagnostics walk the subsystems under their own locks, so
// they live on a separate route that a poll never touches.
MACHA_FAST_TEST("invariants", test_status_is_light_and_diagnostics_have_their_own_route) {
    TestCluster cluster;
    BareNode node(cluster.node_config("status-split"), cluster.keys());
    ClusterStatusService status(node, node.accounts(), node.resources.activity, node.resources.data, node.resources.memory);

    const auto body_of = [](const HttpResponse& response) {
        return Json::parse(
            std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
    };
    const auto get = [&](std::string path) {
        HttpRequest request;
        request.method = "GET";
        request.path = std::move(path);
        return status.handle(request);
    };

    auto light = get("/api/v1/status");
    REQUIRE(light.status == 200);
    auto root = body_of(light);
    for (const auto* key : {"cluster", "nodes", "startup", "subsystems", "threads", "connectivity"})
        CHECK(root.find(key) != nullptr);
    // A supervised thread's fault is reported on the polled route.
    std::thread([] {
        run_supervised_once("test-status-thread-fault", [] { throw std::runtime_error("boom"); });
    }).join();
    {
        const auto again = body_of(get("/api/v1/status"));
        const Json* listed = nullptr;
        for (const auto& entry : again.find("threads")->asArray())
            if (entry.find("name")->asString() == "test-status-thread-fault") listed = &entry;
        REQUIRE(listed != nullptr);
        CHECK(listed->find("faults")->asUInt64() == 1);
        CHECK(listed->find("running")->asUInt64() == 0);
        CHECK(listed->find("last_fault_code")->asString() == "exception");
        CHECK(listed->find("last_fault")->asString() == "boom");
        CHECK(!listed->find("last_fault_unix_ms")->isNull());
    }
    // node_id names the answering node and joins to an entry in nodes[], so a
    // node reached by two addresses is not counted twice. api_endpoint cannot
    // do this: it may differ from the address the client used.
    REQUIRE(root.find("node_id") != nullptr);
    const auto answering = root.find("node_id")->asString();
    CHECK(!answering.empty());
    {
        const auto* listed = root.find("nodes");
        REQUIRE(listed != nullptr);
        bool found = false;
        for (const auto& entry : listed->asArray())
            if (entry.find("id") && entry.find("id")->asString() == answering) found = true;
        CHECK(found);
    }
    // The polled route carries no diagnostics tree; it names the route that does.
    CHECK(root.find("diagnostics") == nullptr);
    REQUIRE(root.find("diagnostics_endpoint") != nullptr);
    CHECK(root.find("diagnostics_endpoint")->asString() == "/api/v1/status/diagnostics");
    auto heavy = get("/api/v1/status/diagnostics");
    REQUIRE(heavy.status == 200);
    auto heavy_root = body_of(heavy);
    const auto* diagnostics = heavy_root.find("diagnostics");
    REQUIRE(diagnostics != nullptr);
    constexpr std::array<const char*, 9> sections{"metadata",       "rpc_server", "rpc_transport",
                                                  "data_resources", "retained_memory",
                                                  "data_store",     "filesystem", "convergence",
                                                  "auth"};
    for (const auto* section : sections)
        CHECK(diagnostics->find(section) != nullptr);
    CHECK(heavy_root.find("generated_at_unix_ms") != nullptr);

    // No diagnostics section is reachable through the polled route. Matched by
    // a field each section alone owns: the light view legitimately contains
    // "metadata_generation", which a bare "metadata" would match.
    const std::string light_text(reinterpret_cast<const char*>(light.body.data()),
                                 light.body.size());
    for (const auto* owned : {"retained_memory", "rpc_server", "rpc_transport", "data_resources",
                              "data_store", "convergence", "peer_latency_ms",
                              "materialization_cache_hits", "user_table_hash",
                              "data_publication_quanta"})
        CHECK(light_text.find(owned) == std::string::npos);

    // The per-node prefix still routes alongside the diagnostics route.
    CHECK(get("/api/v1/status/nodes").status == 200);
    CHECK(get("/api/v1/status/nodes/not-a-node-id").status == 400);
}


// Steady time that stands still until the test moves it.
class SteppedTime {
    std::atomic<Clock::rep> now_{Clock::now().time_since_epoch().count()};

  public:
    Clock::time_point now() const { return Clock::time_point(Clock::duration(now_.load())); }
    std::function<Clock::time_point()> source() const {
        return [this] { return now(); };
    }
    void advance(Clock::duration by) { now_.fetch_add(by.count()); }
};

const Json* node_entry(const Json& root, const NodeId& id) {
    for (const auto& value : root.find("nodes")->asArray())
        if (value.find("id") && value.find("id")->asString() == to_string(id))
            return &value;
    return nullptr;
}

// What Status says of each node from what this node knows of it: a retired
// identity is audit history, not a failed member; membership alone makes a
// peer visible and online with its resource figures unknown, never zero;
// fresh telemetry supplies measured figures, the advertised API endpoint,
// the playback budgets and the fresher metadata generation; telemetry past
// the freshness floor is stale, its resource figures withheld and its
// runtime figures kept with their age.
MACHA_TEST("invariants", test_status_renders_each_node_from_what_it_knows) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("status-render");
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.hydration.enabled = false;
    config.catalogue.scanner.enabled = false;
    // The peer stays membership-online past the telemetry freshness floor.
    config.dead_after = 60s;
    SteppedTime clock;
    BareNode node(config, cluster.keys(), {}, clock.source());
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));
    MetadataManager metadata(node, node.local_state(), node.metadata_server());
    REQUIRE(metadata.snapshot().metadata_voters.empty());

    ClusterStatusService status(node, node.accounts(), node.resources.activity,
                                node.resources.data, node.resources.memory);
    StatusSources sources;
    sources.local = &node.local_state();
    sources.metadata = &metadata;
    const auto get = [&](std::string path) {
        auto response = status.handle(request_for("GET", std::move(path)), sources);
        REQUIRE(response.status == 200);
        return body_json(response);
    };

    {
        // A retired identity stays queryable for audit but is not counted.
        const auto stale_id = random_node_id();
        PersistedNodeStatus stale;
        stale.observed_unix_ms = unix_ms() - 1000;
        stale.host = "10.34.1.50";
        stale.port = 7437;
        stale.failure_domain = "remote";
        stale.storage_capacity = 8ULL * 1024 * 1024 * 1024;
        stale.storage_used = 1024;
        metadata.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            snapshot.node_status[stale_id] = stale;
            delta.upsert_node_status[stale_id] = stale;
        });
        IdentityAssociationReset reset;
        reset.host = stale.host;
        reset.port = stale.port;
        reset.stale_node_id = stale_id;
        reset.epoch = 1;
        reset.reset_unix_ms = unix_ms();
        reset.reset_by = node.node_id();
        reset.reason = "replaced node identity";
        REQUIRE(node.apply_identity_reset(reset));
        metadata.note_replica_validation(true);

        const auto root = get("/api/v1/status");
        CHECK(root.find("cluster")->find("nodes_known")->asUInt64() == 1);
        CHECK(root.find("cluster")->find("nodes_online")->asUInt64() == 1);
        CHECK(root.find("cluster")->find("health")->asString() == "healthy");
        CHECK(root.find("nodes")->asArray().size() == 1);
        CHECK(node_entry(root, node.node_id()) != nullptr);
        const auto detail = get("/api/v1/status/nodes/" + to_string(stale_id));
        CHECK(detail.find("state")->asString() == "retired");
        CHECK(detail.find("identity_association_reset")->find("epoch")->asUInt64() == 1);
    }

    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "10.44.1.200";
    peer.port = 7437;
    peer.failure_domain = "test-lab";
    peer.capacity = 4ULL * 1024 * 1024 * 1024;
    peer.used = 1024ULL * 1024 * 1024;
    // Unknown to membership; telemetry knows it.
    peer.metadata_generation = 0;
    peer.seen_unix_ms = unix_ms();
    node.membership().observe(peer, true);
    // A pending replica-set validation does not demote write capability while
    // the durability floor is reachable.
    metadata.note_replica_validation(false, "test metadata reconciliation pending");

    {
        // Membership without telemetry.
        const auto root = get("/api/v1/status");
        const auto* cluster_json = root.find("cluster");
        CHECK(cluster_json->find("nodes_known")->asUInt64() == 2);
        CHECK(cluster_json->find("nodes_online")->asUInt64() == 2);
        CHECK(cluster_json->find("metadata_availability")->asString() == "writable");
        CHECK(cluster_json->find("metadata_read_available")->asBool());
        CHECK(cluster_json->find("metadata_write_available")->asBool());
        CHECK(!cluster_json->find("metadata_replica_set_validated")->asBool());
        CHECK(!cluster_json->find("storage_online")->find("available")->asBool());
        CHECK(cluster_json->find("storage_online")->find("used_bytes")->isNull());
        CHECK(cluster_json->find("storage_online")->find("free_bytes")->isNull());

        const auto* connectivity = root.find("connectivity");
        REQUIRE(connectivity != nullptr);
        CHECK(!connectivity->find("upnp")->find("enabled")->asBool());
        CHECK(!connectivity->find("upnp")->find("mapping_active")->asBool());
        const auto self = node.membership().self();
        CHECK(connectivity->find("advertised")->find("host")->asString() == self.host);
        CHECK(connectivity->find("advertised")->find("port")->asUInt64() == self.port);
        CHECK(connectivity->find("advertised")->find("source")->asString() == "configured");

        // A node serving no playback omits its budgets rather than reporting
        // a zero a client might act on.
        const auto* self_entry = node_entry(root, node.node_id());
        REQUIRE(self_entry != nullptr);
        REQUIRE(self_entry->find("playback") != nullptr);
        CHECK(self_entry->find("playback")->find("startup_timeout_ms") == nullptr);
        CHECK(self_entry->find("playback")->find("segment_timeout_ms") == nullptr);

        const auto* entry = node_entry(root, peer.id);
        REQUIRE(entry != nullptr);
        CHECK(entry->find("state")->asString() == "online");
        CHECK(entry->find("telemetry_freshness")->asString() == "unavailable");
        CHECK(entry->find("host")->asString() == peer.host);
        CHECK(entry->find("port")->asUInt64() == peer.port);
        // Without telemetry the advertised API address is unknown: omitted.
        CHECK(entry->find("api_endpoint") == nullptr);
        const auto* storage = entry->find("storage");
        CHECK(!storage->find("available")->asBool());
        CHECK(storage->find("capacity_bytes")->asUInt64() == peer.capacity);
        CHECK(storage->find("used_bytes")->isNull());
        CHECK(storage->find("free_bytes")->isNull());
        CHECK(entry->find("storage_backends_online")->isNull());
        const auto* cache = entry->find("cache");
        CHECK(!cache->find("available")->asBool());
        CHECK(cache->find("capacity_bytes")->isNull());
        CHECK(cache->find("used_bytes")->isNull());
        CHECK(cache->find("free_bytes")->isNull());

        const auto diagnostics_root = get("/api/v1/status/diagnostics");
        const auto* diagnostics = diagnostics_root.find("diagnostics");
        REQUIRE(diagnostics != nullptr);
        const auto* metadata_diagnostics = diagnostics->find("metadata");
        CHECK(metadata_diagnostics->find("available")->asBool());
        REQUIRE(metadata_diagnostics->find("accepted_head_persistence_writes") != nullptr);
        REQUIRE(metadata_diagnostics->find("accepted_head_persistence_bytes") != nullptr);
        CHECK(metadata_diagnostics->find("accepted_head_persistence_failures")->asUInt64() == 0);
        const auto* rpc_diagnostics = diagnostics->find("rpc_server");
        CHECK(rpc_diagnostics->find("metadata_pending_jobs")->asUInt64() == 0);
        REQUIRE(rpc_diagnostics->find("frame_timings") != nullptr);
        REQUIRE(rpc_diagnostics->find("message_timings") != nullptr);
        REQUIRE(diagnostics->find("rpc_transport")->find("canonical_connections") != nullptr);
        const auto* data_resources = diagnostics->find("data_resources");
        CHECK(data_resources->find("capacity_bytes")->asUInt64() == config.data_inflight_bytes);
        CHECK(data_resources->find("viewer_reserve_bytes")->asUInt64() ==
              config.data_viewer_reserve_bytes);
        CHECK(data_resources->find("used_bytes")->asUInt64() == 0);
        REQUIRE(data_resources->find("viewer_admissions") != nullptr);
        REQUIRE(data_resources->find("loader_waits") != nullptr);
        REQUIRE(data_resources->find("speculative_waits") != nullptr);
        CHECK(!diagnostics->find("convergence")->find("available")->asBool());
        CHECK(!diagnostics->find("filesystem")->find("available")->asBool());
    }

    NodeTelemetry telemetry;
    telemetry.node_id = peer.id;
    telemetry.boot_id = random_node_id();
    telemetry.sequence = 1;
    telemetry.observed_unix_ms = unix_ms();
    telemetry.host = peer.host;
    telemetry.failure_domain = peer.failure_domain;
    telemetry.port = peer.port;
    telemetry.storage_capacity = peer.capacity;
    // A measured zero differs from a missing observation.
    telemetry.storage_used = 0;
    telemetry.cache_capacity = 1024;
    telemetry.cache_used = 0;
    telemetry.metadata_generation = 25723;
    telemetry.storage_backends_online = 1;
    telemetry.uptime_ms = 60000;
    telemetry.rss_bytes = 123456;
    telemetry.api_endpoint = "http://10.44.1.51:7438";
    telemetry.node_name = "Corvus Test Peer";
    telemetry.playback_startup_timeout_ms = 9000;
    telemetry.playback_segment_timeout_ms = 3000;
    telemetry.traffic = {TrafficClass{2, 5000, 100, 2500, 50}};
    telemetry.traffic_window_ms = 10000;
    node.telemetry().observe(telemetry, true);

    {
        // Fresh telemetry.
        const auto root = get("/api/v1/status");
        const auto* entry = node_entry(root, peer.id);
        REQUIRE(entry != nullptr);
        CHECK(entry->find("telemetry_freshness")->asString() == "live");
        // The fresher telemetry generation wins over the membership record's.
        CHECK(entry->find("metadata_generation")->asUInt64() == 25723);
        // The advertised API address, not the RPC host/port.
        CHECK(entry->find("api_endpoint")->asString() == "http://10.44.1.51:7438");
        // The operator's display name travels beside host, never in place of it.
        CHECK(entry->find("node_name")->asString() == "Corvus Test Peer");
        CHECK(entry->find("host")->asString() == peer.host);
        CHECK(entry->find("playback")->find("startup_timeout_ms")->asInt64() == 9000);
        CHECK(entry->find("playback")->find("segment_timeout_ms")->asInt64() == 3000);
        const auto* traffic = entry->find("traffic");
        REQUIRE(traffic != nullptr);
        CHECK(traffic->find("as_of_unix_ms")->asUInt64() == telemetry.observed_unix_ms);
        CHECK(traffic->find("window_ms")->asUInt64() == 10000);
        const auto& classes = traffic->find("classes")->asArray();
        REQUIRE(classes.size() == 1);
        CHECK(classes.front().find("class")->asString() == "foreground");
        CHECK(classes.front().find("in_bytes")->asUInt64() == 5000);
        CHECK(classes.front().find("out_bytes")->asUInt64() == 100);
        CHECK(classes.front().find("in_bytes_per_s")->asUInt64() == 2500);
        CHECK(classes.front().find("out_bytes_per_s")->asUInt64() == 50);
        const auto* storage = entry->find("storage");
        CHECK(storage->find("available")->asBool());
        CHECK(storage->find("capacity_bytes")->asUInt64() == peer.capacity);
        CHECK(storage->find("used_bytes")->asUInt64() == 0);
        CHECK(storage->find("free_bytes")->asUInt64() == peer.capacity);
        CHECK(entry->find("storage_backends_online")->asUInt64() == 1);
        const auto* cache = entry->find("cache");
        CHECK(cache->find("available")->asBool());
        CHECK(cache->find("capacity_bytes")->asUInt64() == 1024);
        CHECK(cache->find("used_bytes")->asUInt64() == 0);
        CHECK(cache->find("free_bytes")->asUInt64() == 1024);
        CHECK(entry->find("runtime")->find("rss_bytes")->asUInt64() == telemetry.rss_bytes);
    }

    {
        // Past the fixed 5 s freshness floor.
        clock.advance(5200ms);
        const auto root = get("/api/v1/status");
        const auto* entry = node_entry(root, peer.id);
        REQUIRE(entry != nullptr);
        CHECK(entry->find("state")->asString() == "online");
        CHECK(entry->find("telemetry_freshness")->asString() == "stale");
        // Stale resource figures are withheld: consumers sum them across nodes.
        CHECK(!entry->find("storage")->find("available")->asBool());
        CHECK(entry->find("storage")->find("used_bytes")->isNull());
        CHECK(!root.find("cluster")->find("storage_online")->find("available")->asBool());
        // Runtime figures survive: nothing aggregates them and the entry
        // states their age.
        CHECK(entry->find("runtime")->find("rss_bytes")->asUInt64() == telemetry.rss_bytes);
        CHECK(entry->find("runtime")->find("uptime_ms")->asUInt64() == telemetry.uptime_ms);
        CHECK(entry->find("live_age_ms")->asUInt64() >= 5000);
    }

    metadata.note_replica_validation(true);
    const auto validated = get("/api/v1/status");
    CHECK(validated.find("cluster")->find("metadata_availability")->asString() == "writable");
    CHECK(validated.find("cluster")->find("metadata_write_available")->asBool());
    node.stop();
}

// What a node publishes about itself from its configuration: the client API
// endpoint it advertises, and the playback budgets it enforces.
MACHA_FAST_TEST("invariants", test_a_node_advertises_its_api_endpoint_and_playback_budgets) {
    struct ApiRow {
        bool enabled;
        const char* listen;
        uint16_t port;
        const char* advertised;
        const char* host;
        const char* expected;
    };
    const std::vector<ApiRow> endpoints{
        // A wildcard bind is undialable: the advertised RPC host is used.
        {true, "0.0.0.0", 19991, "", "10.44.1.60", "http://10.44.1.60:19991"},
        {true, "0.0.0.0", 19991, "", "fd00::1", "http://[fd00::1]:19991"},
        // An override wins and carries its own scheme (TLS offload).
        {true, "127.0.0.1", 19992, "https://media-node-2.example.net:443", "10.44.1.60",
         "https://media-node-2.example.net:443"},
        {false, "0.0.0.0", 19991, "", "10.44.1.60", ""},
    };
    for (const auto& row : endpoints) {
        CatalogueApiConfig api;
        api.enabled = row.enabled;
        api.listen = row.listen;
        api.port = row.port;
        api.advertised_endpoint = row.advertised;
        CHECK(advertised_api_endpoint(api, row.host) == row.expected);
    }

    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.startup_timeout = 9000ms;
    streaming.segment_timeout = 3000ms;
    const auto budgets = enforced_playback_budgets(streaming);
    CHECK(budgets.startup_timeout_ms == 9000);
    CHECK(budgets.segment_timeout_ms == 3000);
    CHECK(budgets.max_sessions == streaming.max_sessions);
    streaming.enabled = false;
    const auto none = enforced_playback_budgets(streaming);
    CHECK(none.startup_timeout_ms == 0);
    CHECK(none.segment_timeout_ms == 0);
    CHECK(none.max_sessions == 0);
}

// Kept integrated: telemetry gossip between real nodes, and departure
// detected on dead_after.
//
// Status shows a connected peer's collected telemetry, with no fan-out from
// the client, and marks the peer offline once it stops.
MACHA_TEST("invariants", test_status_follows_a_peer_through_telemetry_and_departure) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto first_port = free_port();
    const auto second_port = free_port();
    BareNode first(cluster.node_config("status-first", first_port, {{"127.0.0.1", second_port}}),
                   cluster.keys());
    BareNode second(cluster.node_config("status-second", second_port, {{"127.0.0.1", first_port}}),
                    cluster.keys());
    first.start();
    second.start();
    REQUIRE(first.wait_local_state_ready(10s));
    REQUIRE(second.wait_local_state_ready(10s));
    const auto second_id = second.node_id();
    REQUIRE(wait_until([&] {
        return first.membership().active().size() == 2 && second.membership().active().size() == 2;
    }));
    REQUIRE(wait_until([&] {
        const auto values = first.telemetry().all();
        return std::any_of(values.begin(), values.end(), [&](const NodeTelemetry& value) {
            return value.node_id == second_id && value.storage_capacity > 0 &&
                   value.storage_backends_online > 0;
        });
    }, 10s));

    ClusterStatusService status(first, first.accounts(), first.resources.activity,
                                first.resources.data, first.resources.memory);
    const auto root = [&] {
        auto response = status.handle(request_for("GET", "/api/v1/status"));
        REQUIRE(response.status == 200);
        return body_json(response);
    };
    {
        const auto current = root();
        const auto* entry = node_entry(current, second_id);
        REQUIRE(entry != nullptr);
        CHECK(entry->find("state")->asString() == "online");
        CHECK(entry->find("telemetry_freshness")->asString() == "live");
        CHECK(entry->find("storage")->find("available")->asBool());
        CHECK(!entry->find("storage")->find("used_bytes")->isNull());
        CHECK(entry->find("cache")->find("available")->asBool());
        CHECK(!entry->find("storage_backends_online")->isNull());
        CHECK(current.find("cluster")->find("storage_online")->find("available")->asBool());
    }

    second.stop();
    REQUIRE(wait_until([&] {
        const auto current = root();
        const auto* entry = node_entry(current, second_id);
        return entry && entry->find("state")->asString() == "offline";
    }));
    first.stop();
}

MACHA_TEST("invariants", test_metadata_availability_logs_only_transitions) {
    TestNode fixture("metadata-availability-log");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_write_copies = 1;
    auto& node = fixture.start();
    (void)node;
    auto& metadata = fixture.metadata();
    REQUIRE(metadata.snapshot().metadata_voters.empty());

    auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::all);
    Log::set_logger(capture);

    metadata.note_replica_validation(false, "metadata reconciliation pending");
    CHECK(metadata.cluster_status().write_available);
    CHECK(!metadata.cluster_status().stable);
    metadata.note_replica_validation(false, "metadata reconciliation pending");
    metadata.note_replica_validation(true);
    CHECK(metadata.cluster_status().write_available);
    CHECK(metadata.cluster_status().stable);
    metadata.note_replica_validation(false, "metadata reconciliation pending");
    CHECK(metadata.cluster_status().write_available);
    CHECK(!metadata.cluster_status().stable);

    const auto records = capture->records();
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));

    size_t availability_logs = 0;
    bool saw_initial_writable = false;
    for (const auto& [level, message] : records) {
        if (message.find("metadata availability changed") == std::string::npos)
            continue;
        ++availability_logs;
        if (level == LogLevel::info &&
            message.find("state=writable previous=unavailable") != std::string::npos &&
            message.find("reconciliation pending") != std::string::npos)
            saw_initial_writable = true;
    }
    CHECK(availability_logs == 1);
    CHECK(saw_initial_writable);
}


// An open FUSE inode keeps its identity when another node replaces or
// unlinks its path: it reads the original, and a dirty one never publishes
// through the name another inode now holds.
MACHA_TEST("invariants", test_open_fuse_inodes_keep_their_identity) {
    CatalogueBench bench("fuse-identity");
    auto& fs = bench.fs();
    const auto& config = bench.config();
    const auto original = pattern(4096, 3);
    const auto replacement = pattern(4096, 4);
    const auto dirty = pattern(2048, 23);
    write_file(fs, "/replace.bin", original);
    write_file(fs, "/unlink.bin", original);
    write_file(fs, "/victim.bin", original);
    write_file(fs, "/incoming.bin", replacement);

    FuseFrontend frontend(fs, bench.resources().memory, config.fuse,
                          std::make_unique<ViewerWeightedAdmission>(fs, config.fuse), fs);
    const auto replaced = frontend.open("/replace.bin", true, false, false, false);
    const auto unlinked = frontend.open("/unlink.bin", true, false, false, false);
    const auto victim = frontend.open("/victim.bin", true, true, false, false);
    REQUIRE(frontend.write(victim.inode, 0, dirty) == dirty.size());

    // Stand in for another node publishing a new namespace generation.
    fs.rename("/replace.bin", "/old-replace.bin");
    write_file(fs, "/replace.bin", replacement);
    fs.unlink("/unlink.bin");
    fs.rename("/incoming.bin", "/victim.bin");

    // Force adoption of the already-decoded newer namespace.
    REQUIRE(frontend.inode_for_path("/replace.bin").has_value());
    CHECK(fuse_read(frontend, replaced.inode, original.size()) == original);
    // Unlink removes the pathname at once; the open inode lives on.
    CHECK(!frontend.inode_for_path("/unlink.bin").has_value());
    CHECK(fuse_read(frontend, unlinked.inode, original.size()) == original);

    const auto current = frontend.inode_for_path("/victim.bin");
    REQUIRE(current.has_value());
    CHECK(*current != victim.inode);
    CHECK(fuse_read(frontend, victim.inode, dirty.size()) == dirty);
    frontend.release(victim.inode, true);
    REQUIRE(frontend.wait_for_idle(5s));
    auto reader = fs.open_read("/victim.bin");
    Bytes actual(replacement.size());
    size_t offset = 0;
    while (offset < actual.size()) {
        const auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
        REQUIRE(n > 0);
        offset += n;
    }
    CHECK(actual == replacement);
    frontend.stop();
}

// The catalogue never loses what the namespace still holds: a failed commit
// rolls back without erasing a live object; a scan's prune is fenced to the
// namespace generation it enumerated; artwork staged in a batch is durable
// only at the batch's barrier; and GC liveness fails closed when the current
// catalogue root cannot be fetched.
MACHA_TEST("invariants", test_catalogue_keeps_what_the_namespace_holds) {
    CatalogueBench bench("catalogue-fences");
    auto& node = bench.node();
    auto& fs = bench.fs();
    auto& catalogue = bench.catalogue();
    REQUIRE(wait_until([&] { return node.local_store().online_backends() == 1; }));

    {
        // Rollback never erases a hash namespace metadata still reaches;
        // orphan collection belongs to GC.
        const auto live_bytes = pattern(8192, 5);
        write_file(fs, "/live.bin", live_bytes);
        const auto live_entry = fs.getattr("/live.bin");
        REQUIRE(live_entry.extents.size() == 1);
        const auto live_id = live_entry.extents.front().id;
        REQUIRE(node.local_store().get(live_id).has_value());
        CatalogueItem item;
        item.id = "test:movie:failed-stage";
        item.kind = CatalogueKind::movie;
        item.title = "Failed stage must not delete live data";
        item.artwork.push_back({"poster", live_id, "application/octet-stream"});
        const auto missing_id = object_id(pattern(7777, 6));
        REQUIRE(missing_id != live_id);
        item.artwork.push_back({"backdrop", missing_id, "application/octet-stream"});
        bool failed = false;
        try {
            (void)catalogue.upsert(std::move(item));
        } catch (...) {
            failed = true;
        }
        REQUIRE(failed);
        CHECK(node.local_store().get(live_id) == std::optional<Bytes>{live_bytes});
    }

    {
        // A scan enumerates one immutable snapshot. A move after it leaves the
        // media id live, so a no-op reconciliation succeeds; a replacement
        // after it would be pruned by the stale active set, so the old
        // signature fences that commit.
        FsEntry dir;
        dir.type = EntryType::directory;
        dir.mode = 0755;
        dir.uid = getuid();
        dir.gid = getgid();
        dir.ctime_ns = dir.mtime_ns = wall_time_ns();
        FsEntry file;
        file.type = EntryType::file;
        file.mode = 0644;
        file.uid = getuid();
        file.gid = getgid();
        file.size = 1234;
        file.ctime_ns = file.mtime_ns = wall_time_ns();
        file.extents.push_back({0, file.size, object_id(pattern(1234, 7)), false});
        bench.metadata().mutate([&](MetadataSnapshot& snapshot) {
            snapshot.entries["/Movies"] = dir;
            snapshot.entries["/Movies/A"] = dir;
            snapshot.entries["/Movies/B"] = dir;
            snapshot.entries["/Movies/A/live.mkv"] = file;
        });
        CatalogueItem item;
        item.id = "test:movie:mixed-generation";
        item.kind = CatalogueKind::movie;
        item.title = "Still live";
        item.external_ids["macha_scanner"] = "1";
        item.media_ids = {file_media_id(file)};
        (void)catalogue.upsert(item);

        const auto scanned = fs.local_snapshot_view();
        const auto scanned_signature = metadata_namespace_signature(*scanned.snapshot);
        auto scan_nodes = fs.namespace_nodes();
        const auto discovered = catalogue_snapshot_files("/Movies", *scanned.snapshot, &scan_nodes);
        REQUIRE(discovered.size() == 1);
        CHECK(discovered.front().first == "/Movies/A/live.mkv");
        std::set<std::string> active;
        for (const auto& [_, entry] : discovered)
            active.insert(file_media_id(entry));

        fs.rename("/Movies/A/live.mkv", "/Movies/B/live.mkv");
        const auto moved = fs.local_snapshot_view();
        REQUIRE(moved.snapshot->entries.contains("/Movies/B/live.mkv"));
        CHECK(metadata_namespace_signature(*moved.snapshot) != scanned_signature);
        catalogue.reconcile_scanner({}, active, true, scanned_signature);
        CHECK(catalogue.get(item.id)->media_ids == std::vector<std::string>{file_media_id(file)});

        fs.unlink("/Movies/B/live.mkv");
        write_file(fs, "/Movies/B/live.mkv", pattern(1234, 19));
        const auto replacement_media_id = file_media_id(fs.getattr("/Movies/B/live.mkv"));
        REQUIRE(replacement_media_id != file_media_id(file));
        auto current = catalogue.get(item.id);
        REQUIRE(current.has_value());
        current->media_ids = {replacement_media_id};
        (void)catalogue.upsert(*current, current->revision);
        bool conflicted = false;
        try {
            catalogue.reconcile_scanner({}, active, true, scanned_signature);
        } catch (const CatalogueConflict&) {
            conflicted = true;
        }
        CHECK(conflicted);
        CHECK(catalogue.get(item.id)->media_ids ==
              std::vector<std::string>{replacement_media_id});
    }

    {
        // A scan of an older namespace generation cannot prune a binding
        // made since: the file was replaced after the scan was taken.
        write_file(fs, "/media.bin", pattern(4096, 31));
        const auto old_entry = fs.getattr("/media.bin");
        const auto scanned = fs.local_snapshot_view();
        const auto scanned_signature = metadata_namespace_signature(*scanned.snapshot);
        const std::set<std::string> stale_active{file_media_id(old_entry)};
        fs.unlink("/media.bin");
        write_file(fs, "/media.bin", pattern(4096, 32));
        const auto new_media_id = file_media_id(fs.getattr("/media.bin"));
        REQUIRE(new_media_id != file_media_id(old_entry));
        CatalogueItem item;
        item.id = "test:movie:namespace-fence";
        item.kind = CatalogueKind::movie;
        item.title = "Namespace fence";
        item.external_ids["macha_scanner"] = "1";
        item.media_ids = {new_media_id};
        (void)catalogue.upsert(item);
        bool conflicted = false;
        try {
            catalogue.reconcile_scanner({}, stale_active, true, scanned_signature);
        } catch (const CatalogueConflict&) {
            conflicted = true;
        }
        CHECK(conflicted);
        CHECK(catalogue.get(item.id)->media_ids == std::vector<std::string>{new_media_id});
    }

    {
        // Two artworks in one durability domain: the later generation covers
        // the earlier, so the batch keeps a one-entry frontier, durable only
        // at its barrier.
        DistributedStore::DurabilityBatch batch;
        const auto a = catalogue.stage_artwork_deferred("poster", "image/jpeg",
                                                        pattern(64 * 1024, 201), batch);
        const auto b = catalogue.stage_artwork_deferred("backdrop", "image/jpeg",
                                                        pattern(64 * 1024, 202), batch);
        REQUIRE(batch.requirements.size() == 1);
        const auto token = [&] {
            const auto& replica = batch.requirements.front().replicas.front();
            return StoragePool::DurabilityToken{replica.domain, replica.generation,
                                                replica.backend_instance};
        };
        REQUIRE(batch.requirements.front().replicas.size() == 1);
        REQUIRE(batch.requirements.front().replicas.front().id == node.node_id());
        CHECK(!node.local_store().durability_covered(token()));
        REQUIRE(catalogue.artwork_durability_barrier(batch));
        CHECK(node.local_store().durability_covered(token()));
        CHECK(node.local_store().has(a.id));
        CHECK(node.local_store().has(b.id));
    }

    {
        // A catalogue root that cannot be fetched is still live.
        catalogue.repair_once();
        const auto missing_root = object_id(pattern(32123, 11));
        REQUIRE(!node.local_store().has(missing_root));
        bench.metadata().mutate(
            [&](MetadataSnapshot& snapshot) { snapshot.catalogue_root = missing_root; });
        const auto maintenance = maintenance_inventory(catalogue);
        CHECK(maintenance.control_live.contains(missing_root));
        CHECK(!maintenance.complete);
    }
}

// HttpServer's connection lifecycle: an incomplete request occupies a worker
// only for the client I/O timeout; an idle keep-alive connection occupies
// none; a connection is reused until the client asks to close, the request
// budget is spent, or it idles past its allowance.
MACHA_TEST("invariants", test_http_connections_are_bounded_and_reused) {
    const auto serve = [](CatalogueApiConfig& config) {
        config.enabled = true;
        config.listen = "127.0.0.1";
        config.port = free_port();
        config.max_connections = 4;
        auto server = std::make_unique<HttpServer>(
            config, [](const HttpRequest&) { return http_json(200, "{\"ok\":true}"); });
        server->start();
        REQUIRE(wait_until([&] { return server->bound_port() == config.port; }, 2s));
        return server;
    };
    const auto connect = [](uint16_t port) {
        const int fd = connect_idle(port);
        timeval timeout{2, 0};
        REQUIRE(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        return fd;
    };
    const auto closed_by_server = [](int fd) {
        char probe;
        return ::recv(fd, &probe, 1, 0) == 0;
    };
    const std::string request = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";

    {
        // One worker.
        CatalogueApiConfig config;
        config.workers = 1;
        config.client_io_timeout = 100ms;
        config.keep_alive_idle_timeout = 5s;
        auto server = serve(config);

        // An incomplete request ahead of a complete one in the sole worker:
        // the complete one is served once the slow client's I/O times out.
        const int slow = connect_idle(config.port);
        const std::string partial = "GET /slow HTTP/1.1\r\nHost: localhost\r\n";
        REQUIRE(::send(slow, partial.data(), partial.size(), 0) ==
                static_cast<ssize_t>(partial.size()));
        const int fast = connect(config.port);
        CHECK(raw_http_exchange(fast, request).status == 200);
        ::close(slow);
        ::close(fast);

        // Sequential requests reuse one never-reconnected socket, and an idle
        // keep-alive connection holds no worker: a second connection is served
        // at once, and the first still answers after it.
        const int first = connect(config.port);
        auto reply = raw_http_exchange(first, request);
        CHECK(reply.status == 200);
        CHECK(reply.headers["connection"] == "keep-alive");
        const int second = connect(config.port);
        const auto started = Clock::now();
        CHECK(raw_http_exchange(second, request).status == 200);
        CHECK(Clock::now() - started < 1s);
        reply = raw_http_exchange(first, request);
        CHECK(reply.status == 200);
        CHECK(reply.headers["connection"] == "keep-alive");

        // The client asks to close: the server answers, then closes.
        reply = raw_http_exchange(
            second, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        CHECK(reply.status == 200);
        CHECK(reply.headers["connection"] == "close");
        CHECK(closed_by_server(second));
        ::close(first);
        ::close(second);
        server->stop();
    }

    {
        // A request budget and a short idle allowance.
        CatalogueApiConfig config;
        config.workers = 2;
        config.keep_alive_max_requests = 2;
        config.keep_alive_idle_timeout = 100ms;
        auto server = serve(config);

        // The second of two allowed requests closes, though the client would go on.
        const int budgeted = connect(config.port);
        CHECK(raw_http_exchange(budgeted, request).headers["connection"] == "keep-alive");
        CHECK(raw_http_exchange(budgeted, request).headers["connection"] == "close");
        CHECK(closed_by_server(budgeted));
        ::close(budgeted);

        // A silent keep-alive connection is closed after the idle allowance.
        const int idle = connect(config.port);
        CHECK(raw_http_exchange(idle, request).headers["connection"] == "keep-alive");
        const auto idle_since = Clock::now();
        CHECK(closed_by_server(idle));
        CHECK(Clock::now() - idle_since >= 50ms);
        ::close(idle);
        server->stop();
    }
}

MACHA_TEST("invariants", test_replica_repair_does_not_count_corrupt_remote_as_healthy) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.write_copies = c2.write_copies = 2;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.storage_packing = c2.storage_packing = StoragePackingConfig{0, 0};

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    DistributedStore distributed(n1, n1.local_state(), n1.resources.activity, n1.resources.data, n1.resources.memory, n1.resources.events);
    const auto bytes = pattern(128 * 1024, 9);
    const auto id = object_id(bytes);
    REQUIRE(distributed.put(id, bytes));
    REQUIRE(wait_until([&] { return n2.local_store().has(id); }));
    corrupt_object(c2.storage_backends.front().path, id);

    std::vector<ObjectId> live{id};
    for (int i = 0; i < 4; ++i)
        distributed.repair_once(8ULL * 1024 * 1024, live);

    bool repaired = false;
    try {
        auto data = n2.local_store().get(id);
        repaired = data && *data == bytes;
    } catch (...) {
        repaired = false;
    }
    CHECK(repaired);

    n2.stop();
    n1.stop();
}

MACHA_TEST("invariants", test_rebalance_reads_nothing_with_one_backend_online) {
    // With one backend there is nowhere to move an object: rebalance says it
    // is complete without reading the store.
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto a = t.path() / "only";
    std::filesystem::create_directories(a);
    const std::vector<StorageBackendConfig> backends{{a, 64ULL * 1024 * 1024}};
    StoragePool pool(t.path() / "pool-state", random_node_id(), backends, keys.storage);
    REQUIRE(wait_until([&] { return pool.online_backends() == 1; }));
    for (uint8_t i = 0; i < 8; ++i) {
        const auto bytes = pattern(4096, i);
        REQUIRE(pool.put(object_id(bytes), bytes));
    }
    const auto step = pool.rebalance_step(8ULL * 1024 * 1024, 64);
    CHECK(step.complete);
    CHECK(step.objects == 0);
    CHECK(step.bytes == 0);
}

MACHA_TEST("invariants", test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto a = t.path() / "a";
    const auto b = t.path() / "b";
    std::filesystem::create_directories(a);
    std::filesystem::create_directories(b);
    const auto bytes = pattern(96 * 1024, 10);
    const auto id = object_id(bytes);

    const auto pool_state = t.path() / "pool-state";
    const auto pool_node = random_node_id();
    const std::vector<StorageBackendConfig> backends{{a, 64ULL * 1024 * 1024},
                                                     {b, 64ULL * 1024 * 1024}};

    // Initialise backend identity while the stores are empty, then seed objects
    // through LocalStore, so reopening the pool sees validly formatted media.
    {
        StoragePool initialise(pool_state, pool_node, backends, keys.storage);
        REQUIRE(wait_until([&] { return initialise.online_backends() == 2; }));
    }
    {
        LocalStore sa(a, 64ULL * 1024 * 1024, keys.storage);
        LocalStore sb(b, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return sa.scan_complete() && sb.scan_complete(); }));
        REQUIRE(sa.put(id, bytes));
        REQUIRE(sb.put(id, bytes));
    }

    StoragePool pool(pool_state, pool_node, backends, keys.storage);
    REQUIRE(wait_until([&] { return pool.online_backends() == 2; }));

    // pool.put() reaffirms/touches only the deterministic preferred backend.
    const auto a_before = std::filesystem::last_write_time(object_path(a, id));
    std::this_thread::sleep_for(20ms);
    REQUIRE(pool.put(id, bytes));
    const auto a_after = std::filesystem::last_write_time(object_path(a, id));
    const auto preferred = a_after != a_before ? a : b;
    const auto secondary = preferred == a ? b : a;
    REQUIRE(std::filesystem::exists(object_path(secondary, id)));

    corrupt_object(preferred, id);
    for (int i = 0; i < 8; ++i) {
        auto result = pool.rebalance_step(8ULL * 1024 * 1024, 8);
        if (result.complete)
            break;
    }

    // Rebalance must not remove the last valid copy until the repaired
    // preferred replica has passed strong validation.
    CHECK(std::filesystem::exists(object_path(secondary, id)) || pool.valid(id));
    bool readable = false;
    try {
        auto data = pool.get(id);
        readable = data && *data == bytes;
    } catch (...) {
        readable = false;
    }
    CHECK(readable);
}

MACHA_TEST("invariants", test_rpc_pre_auth_admission_is_bounded) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto port = free_port();
    NodeInfo server_info{random_node_id(), "127.0.0.1", "test", port};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage&) {
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    const auto thread_count = [] {
        size_t count = 0;
        for (const auto& ignored : std::filesystem::directory_iterator("/proc/self/task")) {
            (void)ignored;
            ++count;
        }
        return count;
    };
    const auto before = thread_count();
    std::vector<int> sockets;
    for (int i = 0; i < 32; ++i)
        sockets.push_back(connect_idle(port));
    std::this_thread::sleep_for(100ms);
    const auto after = thread_count();

    // Unauthenticated sockets cost bounded resources, not a thread each.
    CHECK(after <= before + 8);

    for (auto fd : sockets)
        close(fd);
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] pre-auth admission check requires /proc/self/task; skipped\n";
#endif
}

MACHA_TEST("invariants", test_a_durability_barrier_is_timed_per_filesystem) {
    // Each barrier's time is recorded under the directory its domain was made
    // for, so control's commits and a DATA disk's are told apart.
    TempDir t;
    const auto root = t.path() / "barrier-timed-objects";
    std::filesystem::create_directories(root);
    auto& series = observations().histogram("durability.barrier_us.barrier-timed-objects");
    const auto before = series.snapshot().count;
    auto domain = std::make_shared<DurabilityDomain>(1, root, 1ms);
    const auto file = root / "object";
    std::ofstream(file) << "bytes";
    domain->await_durable(domain->complete_mutation(file, root), DurabilityUrgency::immediate);
    CHECK(series.snapshot().count > before);
}

MACHA_TEST("invariants", test_authoritative_deferred_generation_batches_stable_storage_barriers) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 20ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                     domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto strict_a = pattern(256 * 1024, 31);
    const auto strict_b = pattern(256 * 1024, 32);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(store.put(object_id(strict_a), strict_a));
    REQUIRE(store.put(object_id(strict_b), strict_b));
    track_fsync = false;

    // Strict puts share publication's write/rename/generation path: one fsync
    // marks accounting DIRTY, then durability comes from domain syncfs, with
    // no per-object or directory fsyncs.
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 1);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 2);

    const auto deferred_a = pattern(256 * 1024, 33);
    const auto deferred_b = pattern(256 * 1024, 34);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    const auto ga = store.put_deferred(object_id(deferred_a), deferred_a);
    const auto gb = store.put_deferred(object_id(deferred_b), deferred_b);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    REQUIRE(*gb > *ga);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);

    store.durability_barrier(*gb);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(store.get(object_id(deferred_a)) == std::optional<Bytes>{deferred_a});
    CHECK(store.get(object_id(deferred_b)) == std::optional<Bytes>{deferred_b});
#else
    std::cout << "[ARCH-REGRESSION] syncfs generation check is Linux-only; skipped\n";
#endif
}


MACHA_TEST("invariants", test_durability_batch_retains_exact_nondominated_frontier) {
    DistributedStore::DurabilityBatch batch;
    const auto first = random_node_id();
    const auto second = random_node_id();
    const auto first_epoch = random_node_id();
    const auto second_epoch = random_node_id();
    const auto object_a = object_id(pattern(32, 0x31));
    const auto object_b = object_id(pattern(32, 0x41));
    const auto object_c = object_id(pattern(32, 0x51));

    auto requirement = [&](const ObjectId& id, uint64_t first_generation,
                           uint64_t second_generation) {
        DistributedStore::DurabilityRequirement out;
        out.id = id;
        out.required = 1;
        out.replicas = {{first, first_epoch, 1, first_generation, 11},
                        {second, second_epoch, 2, second_generation, 22}};
        return out;
    };

    batch.add(requirement(object_a, 10, 1));
    batch.add(requirement(object_b, 1, 10));
    // Neither alternative dominates, so both are kept to preserve the
    // per-object quorum formula.
    REQUIRE(batch.requirements.size() == 2);

    batch.add(requirement(object_c, 11, 11));
    REQUIRE(batch.requirements.size() == 1);
    CHECK(batch.requirements.front().id == object_c);

    // A later insertion already covered by the frontier retains no metadata.
    batch.add(requirement(object_a, 9, 9));
    CHECK(batch.requirements.size() == 1);
}

MACHA_TEST("invariants", test_accounting_dirty_marker_is_process_session_scoped) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 10ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                     domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    const auto a = pattern(128 * 1024, 44);
    const auto b = pattern(128 * 1024, 45);
    const auto c = pattern(128 * 1024, 46);
    const auto ga = store.put_deferred(object_id(a), a);
    const auto gb = store.put_deferred(object_id(b), b);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    store.durability_barrier(*gb);
    const auto gc = store.put_deferred(object_id(c), c);
    REQUIRE(gc.has_value());
    store.durability_barrier(*gc);
    track_fsync = false;

    // Accounting is marked DIRTY once before the first mutation, not toggled
    // per generation; the clean checkpoint happens at teardown, outside this
    // measured scope.
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 1);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 2);
#else
    std::cout << "[ARCH-REGRESSION] accounting session check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_clean_accounting_checkpoint_is_trusted_with_packs) {
    // A clean checkpoint is trusted with packs: "dirty" is persisted before a
    // session's first mutation, so a torn pack tail only sits behind a dirty
    // checkpoint.
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    // Objects at or below pack_threshold are packed.
    const LocalStoreOptions packed{64ULL * 1024 * 1024, 0, 256 * 1024, 1024 * 1024};
    const auto a = pattern(48 * 1024, 150);
    const auto b = pattern(48 * 1024, 151);
    uint64_t clean_used = 0;
    {
        LocalStore store(root, packed, keys.storage, LocalStoreMode::authoritative, {});
        REQUIRE(wait_until([&] { return store.scan_complete(); }, 5s));
        const auto generation = store.put_deferred(object_id(a), a);
        REQUIRE(generation.has_value());
        store.durability_barrier(*generation);
        clean_used = store.used();
        REQUIRE(clean_used > 0);
    }
    bool have_packs = false;
    for (const auto& entry : std::filesystem::directory_iterator(root / "packs"))
        have_packs = have_packs || entry.is_regular_file();
    REQUIRE(have_packs);
    {
        // Clean shutdown above: no scan, accounting restored immediately.
        LocalStore store(root, packed, keys.storage, LocalStoreMode::authoritative, {});
        CHECK(store.scan_complete());
        CHECK(store.used() == clean_used);
        REQUIRE(store.get(object_id(a)).has_value());
    }

    // Unclean exit after a mutation: the checkpoint is dirty. The store must
    // still admit a put before the walk completes, on the estimate.
    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        try {
            LocalStore store(root, packed, keys.storage, LocalStoreMode::authoritative, {});
            if (!store.scan_complete())
                _exit(20);
            const auto generation = store.put_deferred(object_id(b), b);
            if (!generation.has_value())
                _exit(21);
            store.durability_barrier(*generation);
            _exit(0);
        } catch (...) {
            _exit(22);
        }
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    {
        LocalStore recovered(root, packed, keys.storage, LocalStoreMode::authoritative, {});
        CHECK(recovered.used() >= clean_used); // the dirty checkpoint's estimate
        const auto c = pattern(48 * 1024, 152);
        // Admitted whether or not the walk has finished yet.
        REQUIRE(recovered.put_deferred(object_id(c), c).has_value());
        REQUIRE(wait_until([&] { return recovered.scan_complete(); }, 5s));
        REQUIRE(recovered.get(object_id(b)).has_value());
        REQUIRE(recovered.get(object_id(c)).has_value());
        CHECK(recovered.used() > clean_used);
    }
}

MACHA_TEST("invariants", test_unclean_accounting_recovery_establishes_durable_baseline) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    const auto a = pattern(128 * 1024, 140);
    const auto b = pattern(128 * 1024, 141);
    uint64_t clean_used = 0;
    {
        LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        REQUIRE(store.put(object_id(a), a));
        clean_used = store.used();
    }

    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        try {
            LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
            if (!store.scan_complete())
                _exit(20);
            const auto generation = store.put_deferred(object_id(b), b);
            if (!generation.has_value())
                _exit(21);
            // Skip LocalStore teardown: an unclean process exit, OS still up.
            _exit(0);
        } catch (...) {
            _exit(22);
        }
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    {
        auto domain = std::make_shared<DurabilityDomain>(1, root, 10ms);
        LocalStore recovered(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                             domain);
        REQUIRE(wait_until([&] { return recovered.scan_complete(); }, 5s));
        CHECK(recovered.used() > clean_used);
        REQUIRE(recovered.get(object_id(b)).has_value());
    }
    track_fsync = false;

    // DIRTY accounting forces a tree reconciliation and one physical baseline
    // before generation-zero existing objects can be used as durability proof.
    CHECK(syncfs_calls.load(std::memory_order_relaxed) >= 1);
#else
    std::cout << "[ARCH-REGRESSION] unclean accounting baseline check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_restart_durable_reaffirmation_requires_no_new_barrier) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    const auto bytes = pattern(128 * 1024, 142);
    const auto id = object_id(bytes);
    {
        LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        REQUIRE(store.put(id, bytes));
    }

    auto domain = std::make_shared<DurabilityDomain>(1, root, 100ms);
    LocalStore reopened(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                        domain);
    REQUIRE(reopened.scan_complete());

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    const auto generation = reopened.put_deferred(id, bytes);
    REQUIRE(generation.has_value());
    CHECK(*generation == 0);
    reopened.durability_barrier(*generation);
    track_fsync = false;
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] restart reaffirmation check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_authoritative_delete_is_lazy_durability) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    const auto bytes = pattern(128 * 1024, 143);
    const auto id = object_id(bytes);
    {
        LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        REQUIRE(store.put(id, bytes));
    }

    auto domain = std::make_shared<DurabilityDomain>(1, root, 100ms);
    LocalStore reopened(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                        domain);
    REQUIRE(reopened.scan_complete());
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(reopened.remove(id));
    track_fsync = false;

    // The first mutation dirties derived accounting once. The unlink itself is
    // intentionally not a synchronous durability event: losing it in a crash
    // leaks unreachable garbage, never published media.
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 1);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] lazy deletion check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_local_store_barrier_remembers_complete_physical_cut) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 10ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                     domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto a = pattern(256 * 1024, 47);
    const auto b = pattern(256 * 1024, 48);
    const auto ga = store.put_deferred(object_id(a), a);
    const auto gb = store.put_deferred(object_id(b), b);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    REQUIRE(*gb > *ga);

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    store.durability_barrier(*ga);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(store.durable_generation() >= *gb);

    const auto c = pattern(256 * 1024, 49);
    const auto gc = store.put_deferred(object_id(c), c);
    REQUIRE(gc.has_value());
    REQUIRE(*gc > *gb);

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    store.durability_barrier(*gb);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);

    syncfs_calls = 0;
    track_fsync = true;
    store.durability_barrier(*gc);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
#else
    std::cout << "[ARCH-REGRESSION] local generation cut check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_durability_domain_group_commits_independent_publications) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 120ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                     domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto a = pattern(256 * 1024, 50);
    const auto ga = store.put_deferred(object_id(a), a);
    REQUIRE(ga.has_value());

    syncfs_calls = 0;
    track_fsync = true;
    auto first = std::async(std::launch::async, [&] { store.durability_barrier(*ga); });
    std::this_thread::sleep_for(30ms);

    const auto b = pattern(256 * 1024, 51);
    const auto gb = store.put_deferred(object_id(b), b);
    REQUIRE(gb.has_value());
    auto second = std::async(std::launch::async, [&] { store.durability_barrier(*gb); });
    std::this_thread::sleep_for(30ms);

    const auto c = pattern(256 * 1024, 52);
    const auto gc = store.put_deferred(object_id(c), c);
    REQUIRE(gc.has_value());
    auto third = std::async(std::launch::async, [&] { store.durability_barrier(*gc); });

    first.get();
    second.get();
    third.get();
    track_fsync = false;

    // Three independent publication completions entered the same physical
    // domain inside one scheduling window. Exactly one filesystem barrier must
    // satisfy all three tickets.
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(domain->durable_generation() >= *gc);
#else
    std::cout << "[ARCH-REGRESSION] durability group-commit check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_storage_pool_tokens_name_physical_domain_and_backend_incarnation) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto disk_a = t.path() / "disk-a";
    const auto disk_b = t.path() / "disk-b";
    std::filesystem::create_directories(disk_a);
    std::filesystem::create_directories(disk_b);
    StoragePool pool(t.path() / "state", random_node_id(),
                     {{disk_a, 64ULL * 1024 * 1024}, {disk_b, 64ULL * 1024 * 1024}}, keys.storage,
                     100ms);
    REQUIRE(wait_until([&] { return pool.online_backends() == 2; }));

    std::optional<StoragePool::DurabilityToken> first;
    std::optional<StoragePool::DurabilityToken> second;
    for (uint8_t seed = 60; seed < 120 && !second; ++seed) {
        const auto bytes = pattern(128 * 1024, seed);
        const auto token = pool.put_deferred(object_id(bytes), bytes);
        REQUIRE(token.has_value());
        if (!first)
            first = token;
        else if (token->backend_instance != first->backend_instance)
            second = token;
    }
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->domain == second->domain); // both configured paths are on TempDir's filesystem
    CHECK(first->backend_instance != second->backend_instance);

    syncfs_calls = 0;
    track_fsync = true;
    auto a = std::async(std::launch::async, [&] { pool.durability_barrier(*first); });
    auto b = std::async(std::launch::async, [&] { pool.durability_barrier(*second); });
    a.get();
    b.get();
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(pool.durability_covered(*first));
    CHECK(pool.durability_covered(*second));
#else
    std::cout << "[ARCH-REGRESSION] storage-domain token check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_immediate_reaffirmation_flushes_provisional_generation) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 50ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage, LocalStoreMode::authoritative,
                     domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto bytes = pattern(256 * 1024, 35);
    const auto id = object_id(bytes);
    REQUIRE(store.put_deferred(id, bytes).has_value());

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(store.put(id, bytes));
    track_fsync = false;

    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] provisional reaffirmation check is Linux-only; skipped\n";
#endif
}

// A burst of commits against peers that take their claims slowly: what is
// owed is each commit's claim ids, bounded in bytes, not its snapshot. Past
// the bound the oldest claims go; the newest head is still offered.
MACHA_TEST("invariants", test_claims_owed_to_slow_peers_stay_within_their_bound) {
    TestNode fixture("owed-claims-bound");
    // 1.6 MB of claim ids a commit: twenty commits are past the 32 MB bound.
    constexpr size_t ids_per_commit = 50000;
    std::atomic_uint64_t made{};
    fixture.set_publication_claims([&](const MetadataPublicationContext&) {
        MetadataPublicationClaims claims;
        claims.data.reserve(ids_per_commit);
        const auto base = made.fetch_add(1) * ids_per_commit;
        for (size_t i = 0; i < ids_per_commit; ++i) {
            ObjectId id;
            const auto value = base + i;
            std::memcpy(id.bytes.data(), &value, sizeof(value));
            claims.data.push_back(id);
        }
        return claims;
    });
    TestGate peers;
    fixture.set_peer_retention([&](const NodeId&, uint64_t, const MetadataPublicationClaims&) {
        peers.enter_and_wait();
    });
    struct Opener {
        TestGate& gate;
        ~Opener() { gate.open(); }
    } open_on_exit{peers};
    fixture.start();
    auto& fs = fixture.filesystem();
    auto& metadata = fixture.metadata();
    fs.mkdir("/first", 0755, getuid(), getgid());
    REQUIRE(peers.wait_for_entries(1, 10s));
    for (int i = 0; i < 40; ++i)
        fs.mkdir("/burst-" + std::to_string(i), 0755, getuid(), getgid());

    const auto backlog = metadata.replication_backlog();
    const uint64_t bound = 32ULL * 1024 * 1024;
    CHECK(backlog.claim_bytes <= bound);
    CHECK(backlog.claim_bytes + ids_per_commit * sizeof(ObjectId) > bound);
    CHECK(backlog.dropped > 0);
    CHECK(backlog.commits < 40);

    peers.open();
    REQUIRE(metadata.wait_replicated(20s));
    const auto drained = metadata.replication_backlog();
    CHECK(drained.commits == 0);
    CHECK(drained.claim_bytes == 0);
}

MACHA_TEST("invariants", test_publication_generation_barrier_precedes_metadata_commit) {
#if defined(__linux__)
    TestNode fixture("publication-generation");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.extent_size = 1024 * 1024;
    fixture.start();
    auto& fs = fixture.filesystem();
    fs.create_file("/generation.bin", 0644, getuid(), getgid());

    const auto bytes = pattern(config.extent_size * 8, 37);
    auto writer =
        fs.open_write("/generation.bin", true, false, WriteDurability::publication_generation);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(writer->write(0, bytes) == bytes.size());
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
    writer->commit();
    track_fsync = false;

    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    auto reader = fs.open_read("/generation.bin");
    Bytes actual(bytes.size());
    size_t done = 0;
    while (done < actual.size()) {
        const auto count = reader->read(done, {actual.data() + done, actual.size() - done});
        REQUIRE(count > 0);
        done += count;
    }
    CHECK(actual == bytes);
#else
    std::cout << "[ARCH-REGRESSION] publication generation syncfs check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_persistent_cache_is_explicitly_ephemeral) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    PersistentBlockCache cache({t.path() / "cache", 2, true}, keys.storage);

    const auto a = pattern(64 * 1024, 41);
    const auto b = pattern(64 * 1024, 42);
    const auto c = pattern(64 * 1024, 43);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(cache.put(object_id(a), a));
    REQUIRE(cache.put(object_id(b), b));
    REQUIRE(cache.put(object_id(c), c));
    track_fsync = false;

    CHECK(cache.blocks() == 2);

    // The cache reports hits, misses and evictions, not only occupancy.
    {
        const auto before = cache.stats();
        CHECK(before.entries == 2);
        // Three puts into a two-block cache: one eviction.
        CHECK(before.evictions == 1);

        CHECK(!cache.get(object_id(pattern(1024, 99))));
        CHECK(cache.stats().misses == before.misses + 1);
        CHECK(cache.stats().hits == before.hits);

        // `c` was the last put, so it survived the eviction that took `a`.
        REQUIRE(cache.get(object_id(c)));
        CHECK(cache.stats().hits == before.hits + 1);
        CHECK(cache.stats().misses == before.misses + 1);
    }
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] cache fsync interception check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_control_plane_hint_admission_is_storage_durable) {
#if defined(__linux__)
    TempDir t;
    CatalogueHintQueue hints(t.path() / "state");
    fsync_calls = 0;
    track_fsync = true;
    (void)hints.submit("/Movies/Durable.mkv", "manual", "manual:1",
                       CatalogueHintPriority::manual_rescan);
    track_fsync = false;
    CHECK(fsync_calls.load() >= 2);
#else
    std::cout << "[ARCH-REGRESSION] fsync interception check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_deferred_object_barrier_rejects_stale_process_epoch) {
    TestCluster cluster(ConfigProfile::isolated);
    auto server_config = cluster.node_config("durability-epoch-server");
    auto client_config = cluster.node_config("durability-epoch-client");
    server_config.replication = client_config.replication = 1;
    server_config.metadata_write_copies = client_config.metadata_write_copies = 1;
    BareNode server(server_config, cluster.keys());
    BareNode client(client_config, cluster.keys());
    server.start();
    client.start();
    REQUIRE(server.wait_local_state_ready(10s));
    REQUIRE(client.wait_local_state_ready(10s));

    const auto bytes = pattern(64 * 1024, 46);
    const auto id = object_id(bytes);
    Writer request;
    request.fixed(id.bytes);
    request.bytes(bytes);
    const Endpoint endpoint{"127.0.0.1", server_config.port};
    auto placed = client.call(endpoint, MessageType::put_object_deferred, request.data(),
                              FrameType::read_ahead);
    REQUIRE(placed.message.type == MessageType::ok);
    Reader placed_reply(placed.message.payload);
    NodeId acknowledged_epoch{placed_reply.fixed<16>()};
    const auto domain = placed_reply.u64();
    const auto generation = placed_reply.u64();
    const auto backend_instance = placed_reply.u64();
    placed_reply.finish();
    CHECK(acknowledged_epoch == server.durability_epoch());

    auto barrier_payload = [&](const NodeId& epoch) {
        Writer writer;
        writer.fixed(epoch.bytes);
        writer.u64(domain);
        writer.u64(generation);
        writer.u64(backend_instance);
        return writer.take();
    };

    auto wrong_epoch = random_node_id();
    while (wrong_epoch == acknowledged_epoch)
        wrong_epoch = random_node_id();
    auto stale = barrier_payload(wrong_epoch);
    auto rejected =
        client.call(endpoint, MessageType::object_durability_barrier, stale, FrameType::read_ahead);
    CHECK(rejected.message.type == MessageType::error);

    auto current = barrier_payload(acknowledged_epoch);
    auto durable = client.call(endpoint, MessageType::object_durability_barrier, current,
                               FrameType::read_ahead);
    CHECK(durable.message.type == MessageType::ok);
    client.stop();
    server.stop();
}

MACHA_TEST("invariants", test_rpc_durability_barrier_group_commits_independent_publications) {
#if defined(__linux__)
    TestCluster cluster(ConfigProfile::isolated);
    auto server_config = cluster.node_config("durability-group-rpc-server");
    auto client_config = cluster.node_config("durability-group-rpc-client");
    server_config.replication = client_config.replication = 1;
    server_config.metadata_write_copies = client_config.metadata_write_copies = 1;
    // A window the three barriers below, 75 ms apart, all fall inside.
    BareNode server(server_config, cluster.keys(), {}, {}, 500ms);
    BareNode client(client_config, cluster.keys());
    server.start();
    client.start();
    REQUIRE(server.wait_local_state_ready(10s));
    REQUIRE(client.wait_local_state_ready(10s));
    const Endpoint endpoint{"127.0.0.1", server_config.port};

    struct RemoteToken {
        NodeId epoch{};
        uint64_t domain{};
        uint64_t generation{};
        uint64_t backend_instance{};
    };
    auto defer = [&](uint8_t seed) {
        const auto bytes = pattern(64 * 1024, seed);
        Writer request;
        request.fixed(object_id(bytes).bytes);
        request.bytes(bytes);
        auto reply = client.call(endpoint, MessageType::put_object_deferred, request.data(),
                                 FrameType::read_ahead);
        REQUIRE(reply.message.type == MessageType::ok);
        Reader reader(reply.message.payload);
        RemoteToken token;
        token.epoch.bytes = reader.fixed<16>();
        token.domain = reader.u64();
        token.generation = reader.u64();
        token.backend_instance = reader.u64();
        reader.finish();
        return token;
    };
    auto barrier = [&](const RemoteToken& token) {
        Writer request;
        request.fixed(token.epoch.bytes);
        request.u64(token.domain);
        request.u64(token.generation);
        request.u64(token.backend_instance);
        return client.call(endpoint, MessageType::object_durability_barrier, request.data(),
                           FrameType::read_ahead);
    };

    const auto a = defer(53);
    syncfs_calls = 0;
    track_fsync = true;
    auto first = std::async(std::launch::async, [&] { return barrier(a); });
    std::this_thread::sleep_for(75ms);
    const auto b = defer(54);
    auto second = std::async(std::launch::async, [&] { return barrier(b); });
    std::this_thread::sleep_for(75ms);
    const auto c = defer(55);
    auto third = std::async(std::launch::async, [&] { return barrier(c); });

    REQUIRE(first.get().message.type == MessageType::ok);
    REQUIRE(second.get().message.type == MessageType::ok);
    REQUIRE(third.get().message.type == MessageType::ok);
    track_fsync = false;

    CHECK(a.epoch == b.epoch);
    CHECK(b.epoch == c.epoch);
    CHECK(a.domain == b.domain);
    CHECK(b.domain == c.domain);
    CHECK(a.backend_instance == b.backend_instance);
    CHECK(b.backend_instance == c.backend_instance);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    client.stop();
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] RPC group-commit check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_rpc_durability_barrier_reuses_already_covered_generation) {
#if defined(__linux__)
    TestCluster cluster(ConfigProfile::isolated);
    auto server_config = cluster.node_config("durability-generation-rpc-server");
    auto client_config = cluster.node_config("durability-generation-rpc-client");
    server_config.replication = client_config.replication = 1;
    server_config.metadata_write_copies = client_config.metadata_write_copies = 1;
    BareNode server(server_config, cluster.keys(), {}, {}, 500ms);
    BareNode client(client_config, cluster.keys());
    server.start();
    client.start();
    REQUIRE(server.wait_local_state_ready(10s));
    REQUIRE(client.wait_local_state_ready(10s));
    const Endpoint endpoint{"127.0.0.1", server_config.port};

    struct RemoteToken {
        NodeId epoch{};
        uint64_t domain{};
        uint64_t generation{};
        uint64_t backend_instance{};
    };
    auto defer = [&](uint8_t seed) {
        const auto bytes = pattern(64 * 1024, seed);
        Writer request;
        request.fixed(object_id(bytes).bytes);
        request.bytes(bytes);
        auto reply = client.call(endpoint, MessageType::put_object_deferred, request.data(),
                                 FrameType::read_ahead);
        REQUIRE(reply.message.type == MessageType::ok);
        Reader reader(reply.message.payload);
        RemoteToken token;
        token.epoch.bytes = reader.fixed<16>();
        token.domain = reader.u64();
        token.generation = reader.u64();
        token.backend_instance = reader.u64();
        reader.finish();
        return token;
    };
    auto barrier = [&](const RemoteToken& token) {
        Writer request;
        request.fixed(token.epoch.bytes);
        request.u64(token.domain);
        request.u64(token.generation);
        request.u64(token.backend_instance);
        return client.call(endpoint, MessageType::object_durability_barrier, request.data(),
                           FrameType::read_ahead);
    };

    const auto a = defer(56);
    const auto b = defer(57);
    REQUIRE(a.epoch == b.epoch);
    REQUIRE(a.domain == b.domain);
    REQUIRE(a.backend_instance == b.backend_instance);
    REQUIRE(b.generation > a.generation);

    syncfs_calls = 0;
    track_fsync = true;
    auto first = barrier(a);
    track_fsync = false;
    REQUIRE(first.message.type == MessageType::ok);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);

    // The physical cut for a covered b as well. Make the domain dirty again
    // with c; waiting for b must not flush this newer generation.
    const auto c = defer(58);
    REQUIRE(c.epoch == a.epoch);
    REQUIRE(c.domain == a.domain);
    REQUIRE(c.backend_instance == a.backend_instance);
    REQUIRE(c.generation > b.generation);

    syncfs_calls = 0;
    track_fsync = true;
    auto covered = barrier(b);
    track_fsync = false;
    REQUIRE(covered.message.type == MessageType::ok);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);

    syncfs_calls = 0;
    track_fsync = true;
    auto latest = barrier(c);
    track_fsync = false;
    REQUIRE(latest.message.type == MessageType::ok);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    client.stop();
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] RPC generation reuse check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_authenticated_receiver_enforces_transport_lane) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto port = free_port();
    NodeInfo server_info{random_node_id(), "127.0.0.1", "server", port};
    std::atomic_bool object_dispatched{false};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& message) {
            if (message.type == MessageType::get_object)
                object_dispatched = true;
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    int fd = connect_idle(port);
    NodeInfo client_info{random_node_id(), "127.0.0.1", "client", free_port()};
    SecureChannel channel(fd, keys, client_info, 64 * 1024);
    (void)channel.client_handshake(TransportLane::control);
    // The receiver rejects the frame on its header and closes the session, so
    // the rest of this send may fail: that is the rejection, seen from here.
    try {
        channel.send_fragment(1, FrameType::foreground, MessageType::get_object, true, true, {});
    } catch (const std::runtime_error&) {
    }

    // The session ends without a reply; after that nothing can be dispatched.
    timeval timeout{5, 0};
    REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint8_t byte = 0;
    const auto received = ::recv(fd, &byte, 1, 0);
    const bool timed_out = received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    CHECK(received <= 0);
    CHECK(!timed_out);

    // The receiver, not the sender's lane choice, enforces that object traffic
    // never dispatches on a CONTROL channel.
    CHECK(!object_dispatched.load());

    channel.shutdown();
    server.stop();
}

} // namespace

#if defined(__linux__)
extern "C" int fsync(int fd) {
    if (track_fsync.load(std::memory_order_relaxed))
        fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(::syscall(SYS_fsync, fd));
}

extern "C" int syncfs(int fd) {
    if (track_fsync.load(std::memory_order_relaxed))
        syncfs_calls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(::syscall(SYS_syncfs, fd));
}
#endif
