// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lifecycle recorder (the object ledger plan, T1): each real node's
// configuration, templated from /etc/macha/macha.yaml
// (tests/fixtures/node-configs/), is loaded as the server loads it, run in
// process through start and stop, and every lifecycle step the Service takes
// is compared with the order committed in tests/fixtures/lifecycle/. T5 moves
// the components into a composition root; its derived order must equal this
// one. MACHA_WRITE_LIFECYCLE_FIXTURES=1 rewrites the files instead.
#include "test_backend_support.hpp"

#include <fstream>
#include <sstream>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

std::filesystem::path fixtures() {
    return std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "tests" / "fixtures";
}

std::string replace_all(std::string text, std::string_view from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// The node's configuration with ${ROOT} and the ports filled in,
// and the files and directories it names that a deployed node already has.
Config node_config(std::string_view node, const std::filesystem::path& root) {
    std::ifstream in(fixtures() / "node-configs" / (std::string(node) + ".yaml"));
    REQUIRE(in.is_open());
    std::stringstream text;
    text << in.rdbuf();
    auto yaml = replace_all(text.str(), "${ROOT}", root.string());
    yaml = replace_all(yaml, "${API_PORT}", std::to_string(free_port()));
    yaml = replace_all(yaml, "${PORT}", std::to_string(free_port()));
    yaml = replace_all(yaml, "${TORRENT_PORT}", std::to_string(free_port()));
    yaml = replace_all(yaml, "${DEAD_PORT}", std::to_string(free_port()));
    std::filesystem::create_directories(root / "etc" / "web");
    write_key(root / "etc" / "cluster.key");
    {
        std::ofstream token(root / "etc" / "tmdb.key");
        token << "not-a-token\n";
    }
    const auto path = root / "macha.yaml";
    {
        std::ofstream out(path);
        out << yaml;
    }
    auto config = load_yaml_config(path);
    for (const auto& backend : config.storage_backends)
        std::filesystem::create_directories(backend.path);
    // No subsystem plugins: this records the build under test, not whatever
    // is installed on the machine (see config_for in test_support.hpp).
    config.plugin_path = std::filesystem::path{};
    return config;
}

std::vector<std::string> record_lifecycle(std::string_view node) {
    TempDir root;
    const auto config = node_config(node, root.path());
    const auto keys = load_cluster_keys(config.key_file);
    std::mutex mutex;
    std::vector<std::string> events;
    ServiceInstruments instruments;
    instruments.lifecycle = [&](std::string_view event) {
        std::lock_guard lock(mutex);
        events.emplace_back(event);
    };
    {
        Service service(config, keys, {}, {}, {}, instruments);
        service.start();
        REQUIRE(wait_until([&] { return service.ready(); }, 60s));
        service.stop();
    }
    std::lock_guard lock(mutex);
    return events;
}

void check_lifecycle(std::string_view node) {
    const auto events = record_lifecycle(node);
    const auto path = fixtures() / "lifecycle" / (std::string(node) + ".txt");
    if (const char* write = std::getenv("MACHA_WRITE_LIFECYCLE_FIXTURES"); write && *write == '1') {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::trunc);
        for (const auto& event : events)
            out << event << '\n';
        return;
    }
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::vector<std::string> expected;
    for (std::string line; std::getline(in, line);)
        expected.push_back(line);
    if (events != expected) {
        std::cerr << "lifecycle differs from " << path << "\n--- actual\n";
        for (const auto& event : events)
            std::cerr << event << '\n';
    }
    CHECK(events == expected);
}

MACHA_TEST("lifecycle_record", test_lifecycle_of_gbni_1) {
    check_lifecycle("gbni-1");
}

MACHA_TEST("lifecycle_record", test_lifecycle_of_fi_1) {
    check_lifecycle("fi-1");
}

} // namespace
