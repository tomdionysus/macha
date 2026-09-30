// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decision traces of the maintenance pass (the object ledger plan, T1). A
// fixture drives one node through a scripted history on a manual clock and
// records what the pass decided: gate verdicts as they change, and every
// action. The trace is compared with a committed file under
// tests/fixtures/maintenance-traces/, so any change to what maintenance
// decides -- a gate, an order, a release -- fails here in seconds, where it
// used to need a cluster soak to see. MACHA_WRITE_TRACE_FIXTURES=1 rewrites
// the files instead of comparing (then read the diff before committing it).
#include "service/maintenance_clock.hpp"
#include "test_backend_support.hpp"

#include <fstream>
#include <regex>
#include <sstream>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

// Object and node ids differ on every run (random keys, random node ids),
// so each distinct 64-hex id becomes #1, #2, ... in order of first
// appearance: which object is which, and the order, still have to match.
std::vector<std::string> normalise(const std::vector<std::string>& lines) {
    static const std::regex id("[0-9a-f]{64}");
    std::map<std::string, std::string> names;
    std::vector<std::string> out;
    for (const auto& line : lines) {
        std::string result;
        auto rest = line.cbegin();
        for (std::sregex_iterator it(line.begin(), line.end(), id), end; it != end; ++it) {
            result.append(rest, line.cbegin() + it->position());
            auto [named, inserted] =
                names.try_emplace(it->str(), "#" + std::to_string(names.size() + 1));
            result += named->second;
            rest = line.cbegin() + it->position() + it->length();
        }
        result.append(rest, line.cend());
        out.push_back(std::move(result));
    }
    return out;
}

// What a fixture compares, per step: the outcome (store and claim counts,
// named objects), the actions the pass took since the previous step, grouped
// by kind in the order taken, then the settled state: every gate's verdict
// and the inventory and release horizon it last built. How many
// passes ran between two steps, and so the path a gate took to its verdict,
// depends on when asynchronous events land (a first pass either sees a
// startup event or does not); the actions and the settled verdicts are the
// decisions.
// A gate's inputs without the scheduling ones. Whether a gate was due, or
// its inventory was rebuilt in the same pass, and so whether it reads open or
// shut at the moment a step is taken, depends on which passes ran; what it
// did when it opened is in the actions. The node-state inputs (stable,
// destructive, catalogue complete, generation current...) are the fixture's.
std::string node_conditions(std::string_view detail) {
    std::istringstream words{std::string(detail)};
    std::string kept;
    for (std::string word; words >> word;) {
        if (word == "open" || word == "shut" || word.starts_with("due=") ||
            word.starts_with("rebuilt=") || word.starts_with("share=") ||
            word.starts_with("quiescent="))
            continue;
        kept += (kept.empty() ? "" : " ") + word;
    }
    return kept;
}

class TraceLog {
    std::mutex mutex_;
    std::vector<std::string> lines_;
    std::vector<std::string> actions_;
    std::map<std::string, std::string, std::less<>> gates_;

  public:
    void add(std::string_view kind, std::string_view detail) {
        std::lock_guard lock(mutex_);
        // Gates and the two derived views are state: the last value before
        // a step is the settled one. A pass may rebuild a view at an
        // intermediate generation while a fixture is still writing.
        // Repair's gate is scheduling only (its share of time, and whether
        // it is waiting for an event); what repair did is in the actions.
        if (kind == "gate.repair")
            return;
        if (kind.starts_with("gate."))
            gates_[std::string(kind)] = node_conditions(detail);
        else if (kind == "inventory" || kind == "release-horizon")
            gates_[std::string(kind)] = std::string(detail);
        else
            actions_.push_back(std::string(kind) + ": " + std::string(detail));
    }
    void step(std::string_view name, const std::vector<std::string>& state) {
        std::lock_guard lock(mutex_);
        lines_.push_back("step: " + std::string(name));
        for (const auto& line : state)
            lines_.push_back("  " + line);
        // Order within a kind is the pass's decision order; order across
        // kinds is which pass ran when.
        std::stable_sort(actions_.begin(), actions_.end(), [](const auto& a, const auto& b) {
            return a.substr(0, a.find(':')) < b.substr(0, b.find(':'));
        });
        for (auto& action : actions_)
            lines_.push_back("  " + std::move(action));
        actions_.clear();
        for (const auto& [gate, verdict] : gates_)
            lines_.push_back("  " + gate + " = " + verdict);
    }
    std::vector<std::string> lines() {
        std::lock_guard lock(mutex_);
        return lines_;
    }
};

// One isolated node whose maintenance pass runs on a manual clock and
// reports its decisions to a TraceLog. Fixture steps are marked in the trace
// ("step: ...") so a difference says where it happened.
class TracedNode {
    TempDir temp_;
    ClusterKeys keys_;
    Config config_;
    std::shared_ptr<ManualMaintenanceClock> clock_ = std::make_shared<ManualMaintenanceClock>();
    TraceLog trace_;
    std::unique_ptr<Service> service_;
    std::vector<std::pair<std::string, ObjectId>> watched_;

    // The outcome, beside the decisions: how many objects each store holds
    // and each class has claims on, and for each object the fixture named,
    // whether it is held and whether it is claimed.
    std::vector<std::string> state() {
        auto& node = service_->node();
        std::vector<std::string> lines{
            "state: data_objects=" + std::to_string(node.local_store().list().size()) +
            " data_claims=" +
            std::to_string(node.retention_store().retained_ids(RetentionClass::data).size()) +
            " control_claims=" +
            std::to_string(node.retention_store().retained_ids(RetentionClass::control).size())};
        for (const auto& [name, id] : watched_)
            lines.push_back("object " + name + ": held=" +
                            (node.local_store().has(id) ? "1" : "0") + " claimed=" +
                            (node.retention_store().retained(RetentionClass::data, id) ? "1"
                                                                                       : "0"));
        return lines;
    }

  public:
    explicit TracedNode(std::string_view name) {
        const auto keyfile = temp_.path() / "cluster.key";
        write_key(keyfile);
        keys_ = load_cluster_keys(keyfile);
        config_ = config_for(temp_.path() / std::string(name), keyfile, free_port(), {},
                             ConfigProfile::isolated);
        config_.replication = 1;
        config_.metadata_min_write_replicas = 1;
        config_.min_write_replicas = 1;
        // Short enough that real-time idleness (the store's idle_for, not yet
        // on the clock) is reached within one settle; the fake clock is then
        // stepped past it explicitly.
        config_.maintenance.foreground_quiet = 50ms;
        config_.maintenance.garbage_grace = 1h;
        config_.maintenance.no_progress_backoff = 5min;
        config_.maintenance.scrub_fraction = 0.0;
    }
    ~TracedNode() {
        if (service_)
            service_->stop();
    }

    Config& config() {
        return config_;
    }
    Service& start() {
        MaintenanceInstruments instruments;
        instruments.clock = clock_;
        instruments.trace = [this](std::string_view kind, std::string_view detail) {
            trace_.add(kind, detail);
        };
        service_ = std::make_unique<Service>(config_, keys_, NodeRuntime::StartupStageHook{},
                                             Service::MaintenanceStageHook{},
                                             Service::StartupStallHandler{}, instruments);
        service_->start();
        (void)service_->filesystem();
        // Whether a startup event lands before the first pass decides whether
        // that pass opens GC or waits out a quiet window; past the window the
        // two histories agree, so the first recorded step starts there.
        advance(config_.maintenance.foreground_quiet * 2);
        step("started");
        return *service_;
    }
    Service& service() {
        return *service_;
    }
    void step(std::string_view name) {
        settle();
        trace_.step(name, state());
    }
    // Names the first extent of `path` in every later step's state.
    void watch(std::string name, const std::string& path) {
        const auto entry = service_->filesystem().getattr(path);
        REQUIRE(!entry.extents.empty());
        watched_.emplace_back(std::move(name), entry.extents.front().id);
    }
    const std::filesystem::path& backend() const {
        return config_.storage_backends.front().path;
    }
    // The store's activity clock (idle_for) is still real time, and a pass
    // that sees the foreground busy arms its wake-up on this clock. Being
    // quiet in real time first makes the pass after the advance an idle one,
    // however quickly the fixture got here.
    void advance(Clock::duration by) {
        std::this_thread::sleep_for(config_.maintenance.foreground_quiet * 2);
        clock_->advance(by);
        settle();
    }

    // Waits until the pass is parked in its wait with nothing scheduled and
    // no wake-up for a stretch of real time.
    void settle() {
        const auto deadline = Clock::now() + 20s;
        uint64_t seen = service_->maintenance_wakeups();
        int quiet = 0;
        while (Clock::now() < deadline && quiet < 5) {
            std::this_thread::sleep_for(20ms);
            const auto wakeups = service_->maintenance_wakeups();
            const auto convergence = service_->metadata_convergence_diagnostics();
            const bool parked = std::string_view(service_->maintenance_stage()) == "wait" &&
                                !convergence.scheduled &&
                                convergence.runs_scheduled == convergence.runs_completed;
            quiet = parked && wakeups == seen ? quiet + 1 : 0;
            seen = wakeups;
        }
        REQUIRE(quiet >= 5);
    }

    std::vector<std::string> trace() {
        return normalise(trace_.lines());
    }
};

std::filesystem::path fixture_path(std::string_view name) {
    return std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "tests" / "fixtures" /
           "maintenance-traces" / (std::string(name) + ".txt");
}

void check_against_fixture(std::string_view name, const std::vector<std::string>& trace) {
    const auto path = fixture_path(name);
    if (const char* write = std::getenv("MACHA_WRITE_TRACE_FIXTURES"); write && *write == '1') {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::trunc);
        for (const auto& line : trace)
            out << line << '\n';
        return;
    }
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::vector<std::string> expected;
    for (std::string line; std::getline(in, line);)
        expected.push_back(line);
    if (trace != expected) {
        std::cerr << "trace differs from " << path << "\n--- expected\n";
        for (const auto& line : expected)
            std::cerr << line << '\n';
        std::cerr << "--- actual\n";
        for (const auto& line : trace)
            std::cerr << line << '\n';
    }
    CHECK(trace == expected);
}

MACHA_FAST_TEST("maintenance_trace", test_normalise_names_ids_by_first_appearance) {
    const std::string a(64, 'a');
    const std::string b(64, 'b');
    CHECK(normalise({"x " + b + " " + a, a + "," + b + " y", "none"}) ==
          (std::vector<std::string>{"x #1 #2", "#2,#1 y", "none"}));
}

// A node that has only started: what the pass decides with nothing to do,
// through the quiet window and past the no-progress back-off.
MACHA_TEST("maintenance_trace", test_trace_quiet_node) {
    TracedNode node("trace-quiet");
    node.start();
    node.advance(100ms);
    node.step("quiet window passed");
    node.advance(10min);
    node.step("back-off passed");
    check_against_fixture("quiet-node", node.trace());
}

// A file deleted: its tombstone is protected for the whole garbage grace and
// erased once it has matured, measured on the maintenance clock.
MACHA_TEST("maintenance_trace", test_trace_tombstones_maturing) {
    TracedNode node("trace-tombstones");
    auto& service = node.start();
    auto& fs = service.filesystem();
    write_file(fs, "/kept.bin", pattern(64 * 1024, 1));
    write_file(fs, "/deleted.bin", pattern(64 * 1024, 2));
    node.watch("kept", "/kept.bin");
    node.watch("deleted", "/deleted.bin");
    node.advance(100ms);
    node.step("two files written");
    fs.unlink("/deleted.bin");
    node.advance(100ms);
    node.step("one deleted");
    node.advance(30min);
    node.step("half the grace");
    node.advance(31min);
    node.step("grace passed");
    node.advance(100ms);
    node.step("the sweep after the erasure");
    check_against_fixture("tombstones-maturing", node.trace());
}

// The DATA backend disappears (the mount is lost; on fi-1 on 2026-09-29, a
// USB drive dropping off the bus) and comes back.
MACHA_TEST("maintenance_trace", test_trace_backend_offline_and_back) {
    TracedNode node("trace-backend");
    auto& service = node.start();
    write_file(service.filesystem(), "/on-the-backend.bin", pattern(64 * 1024, 3));
    node.watch("file", "/on-the-backend.bin");
    node.advance(100ms);
    node.step("file written");
    auto away = node.backend();
    away += ".away";
    std::filesystem::rename(node.backend(), away);
    REQUIRE(wait_until([&] { return service.node().local_store().online_backends() == 0; }, 5s));
    node.advance(100ms);
    node.step("backend offline");
    node.advance(10min);
    node.step("offline past the back-off");
    std::filesystem::rename(away, node.backend());
    REQUIRE(wait_until([&] { return service.node().local_store().online_backends() == 1; }, 5s));
    node.advance(100ms);
    node.step("backend back");
    check_against_fixture("backend-offline", node.trace());
}

} // namespace
