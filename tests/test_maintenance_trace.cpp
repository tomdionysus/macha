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
#include <set>
#include <sstream>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

// Object and node ids differ on every run (random keys, random node ids),
// so each distinct id -- 64 hex digits for an object, 32 for a node --
// becomes #1, #2, ... in order of first appearance: which is which, and the
// order, still have to match.
std::vector<std::string> normalise(const std::vector<std::string>& lines) {
    static const std::regex id("[0-9a-f]{64}|[0-9a-f]{32}");
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
    std::set<std::string, std::less<>> ignored_;
    std::vector<std::string> lines_;
    std::vector<std::string> actions_;
    std::map<std::string, std::string, std::less<>> gates_;
    std::map<std::string, std::string, std::less<>> verdicts_;
    bool compare_verdicts_ = true;

  public:
    // Gate verdicts are compared only where the fixture controls when a gate
    // falls due; a reason at the call site.
    void skip_verdicts() {
        std::lock_guard lock(mutex_);
        compare_verdicts_ = false;
    }
    // A kind the fixture does not compare, with its reason at the call site.
    void ignore(std::string kind) {
        std::lock_guard lock(mutex_);
        ignored_.insert(std::move(kind));
    }
    void add(std::string_view kind, std::string_view detail) {
        std::lock_guard lock(mutex_);
        if (ignored_.contains(kind))
            return;
        // Gates and the two derived views are state: the last value before
        // a step is the settled one. A pass may rebuild a view at an
        // intermediate generation while a fixture is still writing.
        // Repair's gate is scheduling only (its share of time, and whether
        // it is waiting for an event); what repair did is in the actions.
        if (kind == "gate.repair")
            return;
        if (kind.starts_with("gate.")) {
            // Two things per gate: the node conditions it last saw, and its
            // verdict on the last pass where it was due and its inventory
            // not rebuilt in the same pass -- the gate function's answer on
            // settled inputs, with the inputs it was given ("not yet" until
            // such a pass).
            const bool decided = detail.find("due=1") != std::string_view::npos &&
                                 detail.find("rebuilt=1") == std::string_view::npos;
            auto& verdict = verdicts_[std::string(kind)];
            if (decided)
                verdict = std::string(detail.substr(0, 4)) + " when " + node_conditions(detail);
            else if (verdict.empty())
                verdict = "not yet decided";
            gates_[std::string(kind)] =
                compare_verdicts_ ? node_conditions(detail) + "; decided " + verdict
                                  : node_conditions(detail);
            return;
        }
        // A claim walk stopping for credit is pacing: credit is scaled by
        // the process's real CPU load, so whether one pass lacked it is
        // timing. What was restored, and in what order, is compared.
        else if (kind == "claim-walk" && detail.ends_with("waiting-for-credit"))
            return;
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
        // kinds is which pass ran when. An action repeated by later passes
        // (a claim that cannot be restored, retried every pass) is the same
        // decision: it is kept once, where it was first taken.
        std::stable_sort(actions_.begin(), actions_.end(), [](const auto& a, const auto& b) {
            return a.substr(0, a.find(':')) < b.substr(0, b.find(':'));
        });
        std::set<std::string> seen;
        std::erase_if(actions_, [&](const std::string& action) {
            return !seen.insert(action).second;
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
            std::to_string(node.claims().retained_ids(RetentionClass::data).size()) +
            " control_claims=" +
            std::to_string(node.claims().retained_ids(RetentionClass::control).size())};
        for (const auto& [name, id] : watched_)
            lines.push_back("object " + name + ": held=" +
                            (node.local_store().has(id) ? "1" : "0") + " claimed=" +
                            (node.claims().retained(RetentionClass::data, id) ? "1"
                                                                                       : "0"));
        return lines;
    }

  public:
    explicit TracedNode(std::string_view name) {
        const auto keyfile = temp_.path() / "cluster.key";
        write_key(keyfile);
        keys_ = load_cluster_keys(keyfile);
        configure(config_for(temp_.path() / std::string(name), keyfile, free_port(), {},
                             ConfigProfile::isolated));
    }
    // A node of a cluster the fixture builds itself.
    TracedNode(Config config, ClusterKeys keys) : keys_(keys) {
        configure(std::move(config));
    }
    // The settings every node of a trace fixture runs with, traced or not.
    static void apply_trace_settings(Config& config) {
        config.replication = 1;
        config.metadata_min_write_replicas = 1;
        config.min_write_replicas = 1;
        // The quiet window, on the manual clock like everything else; the
        // fixture steps past it explicitly.
        config.maintenance.foreground_quiet = 50ms;
        config.maintenance.garbage_grace = 1h;
        config.maintenance.no_progress_backoff = 5min;
        config.maintenance.scrub_fraction = 0.0;
    }

  private:
    void configure(Config config) {
        config_ = std::move(config);
        apply_trace_settings(config_);
    }

  public:
    ~TracedNode() {
        if (service_)
            service_->stop();
    }

    Config& config() {
        return config_;
    }
    Service& start() {
        ServiceInstruments instruments;
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
    void ignore(std::string kind) {
        trace_.ignore(std::move(kind));
    }
    void skip_verdicts() {
        trace_.skip_verdicts();
    }
    const std::filesystem::path& backend() const {
        return config_.storage_backends.front().path;
    }
    // The store's activity clock (idle_for) is this same manual clock, so
    // stepping past the quiet window first makes the pass after the advance
    // an idle one, however quickly the fixture got here.
    // Settling first means every event already raised (a write, a peer
    // lost) has reset its quiet window before time moves past it. A pass
    // that rebuilds the inventory schedules one follow-up a quiet window
    // later (so a new inventory is never used destructively in the pass that
    // built it); the second, small step makes that follow-up happen here,
    // whichever side of the first step the rebuild fell.
    void advance(Clock::duration by) {
        const auto quiet = config_.maintenance.foreground_quiet * 2;
        settle();
        clock_->advance(quiet);
        settle();
        clock_->advance(by);
        settle();
        clock_->advance(quiet);
        settle();
    }

    // Waits until the pass is parked in its wait with no wake-up for a
    // stretch of real time.
    void settle() {
        const auto deadline = Clock::now() + 20s;
        uint64_t seen = service_->maintenance_wakeups();
        int quiet = 0;
        while (Clock::now() < deadline && quiet < 5) {
            std::this_thread::sleep_for(20ms);
            // Parked, not finished: work deferred to a deadline on the
            // manual clock (a metadata retry, say) waits for the fixture to
            // move time, and is part of the settled state.
            const auto wakeups = service_->maintenance_wakeups();
            const bool parked = std::string_view(service_->maintenance_stage()) == "wait";
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

// ---- The clock: a primitive, tested exhaustively ---------------------------

MACHA_FAST_TEST("maintenance_trace", test_manual_clock_moves_only_when_advanced) {
    ManualMaintenanceClock clock;
    const auto steady = clock.now();
    const auto wall = clock.wall_ns();
    CHECK(wall > 0);
    std::this_thread::sleep_for(5ms);
    CHECK(clock.now() == steady);
    CHECK(clock.wall_ns() == wall);
    clock.advance(1500ms);
    CHECK(clock.now() == steady + 1500ms);
    CHECK(clock.wall_ns() == wall + 1'500'000'000);
    CHECK(clock.wall_ms() == static_cast<uint64_t>(wall + 1'500'000'000) / 1'000'000);
}

MACHA_FAST_TEST("maintenance_trace", test_wall_ms_of_a_clock_before_the_epoch_is_zero) {
    struct BeforeEpoch final : MaintenanceClock {
        Clock::time_point now() const override {
            return {};
        }
        int64_t wall_ns() const override {
            return -5;
        }
        void wait_until(std::condition_variable_any&, std::unique_lock<std::mutex>&,
                        std::stop_token, Clock::time_point, const std::function<bool()>&) override {
        }
    } clock;
    CHECK(clock.wall_ms() == 0);
}

MACHA_FAST_TEST("maintenance_trace", test_manual_clock_wait_returns_on_ready_stop_or_deadline) {
    ManualMaintenanceClock clock(1ms);
    std::condition_variable_any cv;
    std::mutex mutex;
    std::stop_source stop;

    // Ready already: no wait at all.
    {
        std::unique_lock lock(mutex);
        clock.wait_until(cv, lock, stop.get_token(), Clock::time_point::max(), [] { return true; });
    }
    // A deadline already reached in this clock's time.
    {
        std::unique_lock lock(mutex);
        clock.wait_until(cv, lock, stop.get_token(), clock.now(), [] { return false; });
    }
    // A deadline reached only when another thread advances the clock; real
    // time alone never gets there.
    {
        std::atomic_bool advanced{};
        std::jthread advancer([&] {
            std::this_thread::sleep_for(20ms);
            advanced = true;
            clock.advance(1h);
        });
        std::unique_lock lock(mutex);
        clock.wait_until(cv, lock, stop.get_token(), clock.now() + 1h, [] { return false; });
        CHECK(advanced.load());
    }
    // An unbounded wait ends when the predicate becomes true.
    {
        std::atomic_bool ready{};
        std::jthread setter([&] {
            std::this_thread::sleep_for(20ms);
            ready = true;
            cv.notify_all();
        });
        std::unique_lock lock(mutex);
        clock.wait_until(cv, lock, stop.get_token(), Clock::time_point::max(),
                         [&] { return ready.load(); });
        CHECK(ready.load());
    }
    // And when stop is requested.
    {
        std::jthread stopper([&] {
            std::this_thread::sleep_for(20ms);
            stop.request_stop();
        });
        std::unique_lock lock(mutex);
        clock.wait_until(cv, lock, stop.get_token(), Clock::time_point::max(), [] { return false; });
        CHECK(stop.stop_requested());
    }
}

MACHA_FAST_TEST("maintenance_trace", test_system_clock_reads_and_waits_in_real_time) {
    SystemMaintenanceClock clock;
    const auto before = Clock::now();
    CHECK(clock.now() >= before);
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    CHECK(std::llabs(clock.wall_ns() - wall) < 1'000'000'000);
    std::condition_variable_any cv;
    std::mutex mutex;
    std::stop_source stop;
    {
        std::unique_lock lock(mutex);
        const auto started = Clock::now();
        clock.wait_until(cv, lock, stop.get_token(), started + 20ms, [] { return false; });
        CHECK(Clock::now() - started >= 20ms);
    }
    {
        // Unbounded means unbounded: still waiting after 50 ms, ended only by
        // stop. A timed wait to time_point::max() can overflow and return at
        // once on some standard libraries, which would make this a busy loop.
        std::atomic_bool returned{};
        std::jthread waiter([&] {
            std::unique_lock lock(mutex);
            clock.wait_until(cv, lock, stop.get_token(), Clock::time_point::max(),
                             [] { return false; });
            returned = true;
        });
        std::this_thread::sleep_for(50ms);
        CHECK(!returned.load());
        stop.request_stop();
        waiter.join();
        CHECK(returned.load());
    }
}

MACHA_FAST_TEST("maintenance_trace", test_normalise_names_ids_by_first_appearance) {
    const std::string a(64, 'a');
    const std::string b(64, 'b');
    const std::string node(32, 'c');
    CHECK(normalise({"x " + b + " " + a, a + "," + b + " y", "none", "to " + node + " " + a}) ==
          (std::vector<std::string>{"x #1 #2", "#2,#1 y", "none", "to #3 #2"}));
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
    // An unrelated event wakes the pass mid-grace. Without one the pass
    // sleeps until the deadline it armed at retirement plus grace, and only
    // the grace check in collect_garbage keeps the tombstone when it wakes
    // sooner -- as it does on a node with anything else going on.
    write_file(fs, "/unrelated.bin", pattern(64 * 1024, 30));
    node.advance(100ms);
    node.step("half the grace, an unrelated write");
    node.advance(31min);
    node.step("grace passed");
    node.advance(100ms);
    node.step("the sweep after the erasure");
    check_against_fixture("tombstones-maturing", node.trace());
}

// A deleted file's content is written again under another name before its
// tombstone matures: the object is live again, so the tombstone is stale and
// is erased at once, not collected.
MACHA_TEST("maintenance_trace", test_trace_revived_tombstone) {
    TracedNode node("trace-revived");
    auto& service = node.start();
    auto& fs = service.filesystem();
    write_file(fs, "/first.bin", pattern(64 * 1024, 4));
    node.watch("first", "/first.bin");
    node.advance(100ms);
    node.step("file written");
    fs.unlink("/first.bin");
    node.advance(100ms);
    node.step("file deleted");
    write_file(fs, "/again.bin", pattern(64 * 1024, 4));
    node.watch("again", "/again.bin");
    node.advance(100ms);
    node.step("same content written again");
    node.advance(100ms);
    node.step("the pass after");
    check_against_fixture("revived-tombstone", node.trace());
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

// Claimed objects vanish from the only node that held them (a disk losing
// files). The claim walk visits each claim and the pull pass each object
// the node should own, in their own orders; with no peer to fetch from,
// every visit says so. A change to either order changes this trace.
MACHA_TEST("maintenance_trace", test_trace_claimed_objects_lost) {
    TracedNode node("trace-lost");
    auto& service = node.start();
    auto& fs = service.filesystem();
    for (int index = 0; index < 3; ++index) {
        const auto path = "/lost-" + std::to_string(index) + ".bin";
        write_file(fs, path, pattern(64 * 1024, static_cast<uint8_t>(10 + index)));
        node.watch("lost-" + std::to_string(index), path);
    }
    node.advance(100ms);
    node.step("three files written");
    for (const auto& id : service.node().claims().retained_ids(RetentionClass::data))
        REQUIRE(service.node().local_store().remove(id));
    node.advance(10min);
    // Nothing is walked: repair waits for an event, and a lost file is not
    // one (0.73.2 behaviour, recorded, not endorsed).
    node.step("their objects lost");
    write_file(fs, "/unrelated.bin", pattern(64 * 1024, 20));
    node.advance(100ms);
    node.step("an unrelated file written");
    check_against_fixture("claimed-objects-lost", node.trace());
}

// A peer drops out of a two-node cluster: every known node is no longer
// reachable, so nothing destructive runs, and a tombstone made while it is
// away waits for it. Reachability is judged on real time (dead_after).
MACHA_TEST("maintenance_trace", test_trace_peer_unreachable_and_back) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto port_a = free_port();
    const auto port_b = free_port();
    auto config_b = cluster.node_config("trace-peer-b", port_b, {{"127.0.0.1", port_a}});
    TracedNode::apply_trace_settings(config_b);
    TracedNode node(cluster.node_config("trace-peer-a", port_a), cluster.keys());
    // Both nodes own every object, so what each holds does not depend on
    // which of them copied first; and which did -- this node pushing or the
    // peer pulling -- is a race by design, so repair is not compared here.
    // Repair's order is compared on one node (test_trace_claimed_objects_lost).
    node.config().replication = 2;
    config_b.replication = 2;
    // And the write claims on both, not on whichever the barrier found first.
    node.config().min_write_replicas = 2;
    config_b.min_write_replicas = 2;
    node.ignore("repair");
    // Whether a GC-due pass falls inside the window while the peer is away
    // depends on when its topology events land, which this fixture does
    // not control; the gates' node conditions and every action (a tombstone
    // erased or a claim released while it is away) are still compared.
    node.skip_verdicts();
    auto peer = std::make_unique<Service>(config_b, cluster.keys());
    auto& service = node.start();
    peer->start();
    REQUIRE(wait_until([&] { return service.node().membership().all_known_reachable() &&
                                    service.node().membership().active().size() == 2; },
                       10s));
    write_file(service.filesystem(), "/doomed.bin", pattern(64 * 1024, 4));
    node.watch("doomed", "/doomed.bin");
    node.advance(100ms);
    node.step("peer joined, file written");

    peer->stop();
    peer.reset();
    REQUIRE(wait_until([&] { return !service.node().membership().all_known_reachable(); }, 10s));
    service.filesystem().unlink("/doomed.bin");
    node.advance(100ms);
    node.step("peer gone, file deleted");
    node.advance(2h);
    node.step("peer gone past the grace");

    peer = std::make_unique<Service>(config_b, cluster.keys());
    peer->start();
    // Rejoining is membership and a metadata merge, over real time; either
    // arriving after the clock steps restarts GC's quiet window. Step once
    // the cluster has settled.
    REQUIRE(wait_until(
        [&] {
            return service.node().membership().all_known_reachable() &&
                   service.metadata_manager().cluster_status().stable;
        },
        10s));
    node.settle();
    node.advance(100ms);
    node.step("peer back");
    node.advance(100ms);
    node.step("the pass after the peer returned");
    peer->stop();
    check_against_fixture("peer-unreachable", node.trace());
}

// The catalogue's manifest is lost from the only node that held it: the
// catalogue inventory is incomplete, so every destructive gate that needs it
// stays shut, and the release horizon cannot be completed either, so a file
// deleted afterwards keeps its claim.
MACHA_TEST("maintenance_trace", test_trace_incomplete_catalogue_and_release_horizon) {
    TracedNode node("trace-catalogue");
    auto& service = node.start();
    auto& fs = service.filesystem();
    CatalogueItem item;
    item.id = "movie:trace";
    item.kind = CatalogueKind::movie;
    item.title = "Trace";
    (void)service.catalogue().upsert(item);
    write_file(fs, "/claimed.bin", pattern(64 * 1024, 5));
    node.watch("claimed", "/claimed.bin");
    node.advance(100ms);
    node.step("catalogue item and a file added");
    const auto root = service.metadata_manager().snapshot().catalogue_root;
    REQUIRE(root.has_value());
    REQUIRE(service.node().control_store().remove(*root));
    fs.unlink("/claimed.bin");
    node.advance(100ms);
    node.step("catalogue root lost, file deleted");
    node.advance(2h);
    node.step("past the grace");
    check_against_fixture("incomplete-catalogue", node.trace());
}

} // namespace
