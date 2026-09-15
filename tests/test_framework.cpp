// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace macha::test {
namespace {

#if defined(__linux__) && (defined(__GNUC__) || defined(__clang__))
// Test cases deliberately use _Exit() after fork so child teardown cannot run
// parent-owned process handlers. That also bypasses LeakSanitizer's atexit
// hook, so sanitizer builds must request the check explicitly after the test
// function (and all of its local owners) has unwound. The weak symbol keeps
// ordinary builds independent of the sanitizer runtime.
extern "C" int __lsan_do_recoverable_leak_check() __attribute__((weak));
#endif

using Clock = std::chrono::steady_clock;

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

std::atomic_int child_failures{0};
std::size_t current_case_index{};

struct Options {
    unsigned slots{};
    unsigned timeout_scale{};
    // Run every selected case this many times and report a per-case failure
    // count at the end. This is the measurement the "known flake" habit never
    // had: a rate on named hardware at real parallelism, instead of a
    // recollection (TODO/2026-09-14-test-suite-must-be-deterministic-plan.md).
    unsigned repeat{1};
    bool list{};
    bool verbose{};
    std::string filter;
};

unsigned default_slots() {
    const auto detected = std::thread::hardware_concurrency();
    if (detected <= 2) return 2;
    return std::min(12u, detected);
}

// Each case declares a wall-clock deadline (30s fast, 60s integration, 120s
// heavy) chosen against an ordinary build. A sanitizer build is slower by a
// large constant factor -- roughly 2-3x for AddressSanitizer, 5-15x for
// ThreadSanitizer -- so those same deadlines would fire as timeouts on code
// that is behaving correctly, and a spurious timeout is worse than useless
// here: it hides the sanitizer report the run existed to produce. Scale the
// deadlines by default when the binary is actually instrumented, so
// MACHA_SANITIZE builds are usable without every caller remembering a flag.
unsigned default_timeout_scale() {
#if defined(__SANITIZE_THREAD__)
    return 10;
#elif defined(__SANITIZE_ADDRESS__)
    return 3;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return 10;
#elif __has_feature(address_sanitizer)
    return 3;
#else
    return 1;
#endif
#else
    return 1;
#endif
}

unsigned parse_unsigned(std::string_view value, const char* what) {
    if (value.empty()) throw std::runtime_error(std::string("missing ") + what);
    const std::string text(value);
    char* end = nullptr;
    errno = 0;
    const auto number = std::strtoul(text.c_str(), &end, 10);
    if (errno || !end || *end != '\0' || number == 0 || number > 256)
        throw std::runtime_error(std::string("invalid ") + what + ": " + text);
    return static_cast<unsigned>(number);
}

Options parse_options(int argc, char** argv) {
    Options options;
    options.slots = default_slots();
    if (const char* env = std::getenv("MACHA_TEST_JOBS"))
        options.slots = parse_unsigned(env, "MACHA_TEST_JOBS");
    options.timeout_scale = default_timeout_scale();
    if (const char* env = std::getenv("MACHA_TEST_TIMEOUT_SCALE"))
        options.timeout_scale = parse_unsigned(env, "MACHA_TEST_TIMEOUT_SCALE");

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--list") {
            options.list = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--serial") {
            options.slots = 1;
        } else if (arg == "--jobs") {
            if (++i >= argc) throw std::runtime_error("--jobs requires a value");
            options.slots = parse_unsigned(argv[i], "--jobs");
        } else if (arg.starts_with("--jobs=")) {
            options.slots = parse_unsigned(arg.substr(7), "--jobs");
        } else if (arg == "--filter") {
            if (++i >= argc) throw std::runtime_error("--filter requires a value");
            options.filter = argv[i];
        } else if (arg.starts_with("--filter=")) {
            options.filter = std::string(arg.substr(9));
        } else if (arg == "--timeout-scale") {
            if (++i >= argc) throw std::runtime_error("--timeout-scale requires a value");
            options.timeout_scale = parse_unsigned(argv[i], "--timeout-scale");
        } else if (arg.starts_with("--timeout-scale=")) {
            options.timeout_scale = parse_unsigned(arg.substr(16), "--timeout-scale");
        } else if (arg == "--repeat") {
            if (++i >= argc) throw std::runtime_error("--repeat requires a value");
            options.repeat = parse_unsigned(argv[i], "--repeat");
        } else if (arg.starts_with("--repeat=")) {
            options.repeat = parse_unsigned(arg.substr(9), "--repeat");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "macha-tests [--list] [--filter TEXT] [--jobs N|--serial] [--verbose]\n"
                         "            [--timeout-scale N] [--repeat N]\n"
                         "Tests run in isolated child processes. MACHA_TEST_JOBS overrides the\n"
                         "default parallel slot budget. Integration tests consume two slots and\n"
                         "heavy lifecycle tests consume three.\n"
                         "MACHA_TEST_TIMEOUT_SCALE (or --timeout-scale) multiplies every case\n"
                         "deadline; it defaults above 1 in a sanitizer build, where the same\n"
                         "work legitimately takes several times longer.\n"
                         "MACHA_TEST_LOG_LEVEL=DEBUG raises the product log level inside each\n"
                         "case; the output is captured and shown only for a failing case.\n"
                         "--repeat runs every selected case N times, interleaved across the\n"
                         "parallel slots like an ordinary run, and reports a per-case failure\n"
                         "count: the way to measure a suspected flake rather than recall it.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown test option: " + std::string(arg));
        }
    }
    return options;
}

unsigned slots_for(TestCost cost) {
    return static_cast<unsigned>(cost);
}

std::string full_name(const TestCase& test) {
    return std::string(test.group) + "/" + std::string(test.name);
}

bool selected(const TestCase& test, const Options& options) {
    if (options.filter.empty()) return true;
    return full_name(test).find(options.filter) != std::string::npos;
}

struct RunningCase {
    std::size_t selection_index{};
    const TestCase* test{};
    pid_t pid{-1};
    int log_fd{-1};
    unsigned slots{};
    Clock::time_point started{};
    // The case's declared timeout after the scale factor, so the deadline the
    // scheduler enforces and the one a failure reports are the same number.
    std::chrono::seconds deadline{};
};

int create_capture_file() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "macha-test-output-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const int fd = ::mkstemp(mutable_pattern.data());
    if (fd < 0)
        throw std::runtime_error("mkstemp for test output failed: " +
                                 std::string(std::strerror(errno)));
    (void)::unlink(mutable_pattern.data());
    return fd;
}

std::string read_capture(int fd) {
    if (::lseek(fd, 0, SEEK_SET) < 0) return {};
    std::string output;
    char buffer[8192];
    while (true) {
        const auto n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        output.append(buffer, static_cast<std::size_t>(n));
    }
    return output;
}

// Coverage counters are written by an atexit handler, and a test child
// deliberately never runs one: it leaves through std::_Exit, which is what
// stops a forked child flushing buffers it inherited from the parent. So the
// counters have to be dumped by hand, and reset on entry -- a child inherits
// the parent's accumulated counts at fork(), and without a reset every case
// would re-report the parent's startup as its own.
//
// Without this, every case in the suite contributes nothing at all and the
// report reads 0% however many tests pass.
#if defined(MACHA_COVERAGE)
#if defined(__clang__)
extern "C" void __llvm_profile_reset_counters(void);
extern "C" int __llvm_profile_write_file(void);
void coverage_reset() { __llvm_profile_reset_counters(); }
void coverage_dump() { (void)__llvm_profile_write_file(); }
#elif defined(__GNUC__)
extern "C" void __gcov_reset(void);
extern "C" void __gcov_dump(void);
void coverage_reset() { __gcov_reset(); }
void coverage_dump() { __gcov_dump(); }
#else
void coverage_reset() {}
void coverage_dump() {}
#endif
#else
void coverage_reset() {}
void coverage_dump() {}
#endif

[[noreturn]] void child_run(const TestCase& test, std::size_t selection_index, int capture_fd) {
    if (::dup2(capture_fd, STDOUT_FILENO) < 0 || ::dup2(capture_fd, STDERR_FILENO) < 0)
        std::_Exit(125);
    if (capture_fd > STDERR_FILENO) ::close(capture_fd);

    coverage_reset();
    child_failures.store(0, std::memory_order_relaxed);
    set_case_index(selection_index + 1);
    // A case's output is only ever shown when it fails, so a more verbose
    // level costs nothing on a green run and is the difference between a
    // diagnosable failure and "it timed out". The product's own `shutdown:`
    // and `peer ... liveness failure:` lines are DEBUG.
    if (const char* level = std::getenv("MACHA_TEST_LOG_LEVEL")) {
        try {
            Log::set_logger(std::make_shared<ConsoleLogger>(parse_log_level(level)));
        } catch (const std::exception& e) {
            std::cerr << "MACHA_TEST_LOG_LEVEL: " << e.what() << '\n';
        }
    }
    try {
        test.function();
    } catch (const std::exception& e) {
        std::cerr << "uncaught exception: " << e.what() << '\n';
        child_failures.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        std::cerr << "uncaught non-standard exception\n";
        child_failures.fetch_add(1, std::memory_order_relaxed);
    }
#if defined(__linux__) && (defined(__GNUC__) || defined(__clang__))
    if (__lsan_do_recoverable_leak_check && __lsan_do_recoverable_leak_check()) {
        std::cerr << "LeakSanitizer reported live allocations after test teardown\n";
        child_failures.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    std::cout.flush();
    std::cerr.flush();
    coverage_dump();
    std::_Exit(child_failures.load(std::memory_order_relaxed) == 0 ? 0 : 1);
}

RunningCase launch(const TestCase& test, std::size_t selection_index,
                   std::chrono::seconds deadline) {
    const int capture_fd = create_capture_file();
    const auto started = Clock::now();
    // fork() duplicates userspace stream buffers.  If the parent has reported
    // completed cases into a redirected (and therefore fully-buffered) stream,
    // a later child would otherwise flush that inherited text into its own
    // capture file.  That made parallel failures appear to contain results and
    // timeouts from unrelated tests.
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(capture_fd);
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }
    if (pid == 0) child_run(test, selection_index, capture_fd);
    return RunningCase{selection_index, &test,           pid,     capture_fd,
                       slots_for(test.cost), started, deadline};
}

struct Result {
    std::size_t selection_index{};
    const TestCase* test{};
    bool passed{};
    bool timed_out{};
    int signal{};
    int exit_code{};
    std::chrono::seconds deadline{};
    std::chrono::milliseconds elapsed{};
    std::string output;
};

std::optional<Result> poll_finished(RunningCase& running) {
    int status = 0;
    pid_t waited = -1;
    do { waited = ::waitpid(running.pid, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - running.started);

    if (waited == 0 && now - running.started < running.deadline)
        return std::nullopt;

    Result result;
    result.selection_index = running.selection_index;
    result.test = running.test;
    result.deadline = running.deadline;
    result.elapsed = elapsed;

    if (waited == 0) {
        result.timed_out = true;
        (void)::kill(running.pid, SIGKILL);
        do { waited = ::waitpid(running.pid, &status, 0); } while (waited < 0 && errno == EINTR);
    }

    if (!result.timed_out) {
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
            result.passed = result.exit_code == 0;
        } else if (WIFSIGNALED(status)) {
            result.signal = WTERMSIG(status);
        }
    }
    result.output = read_capture(running.log_fd);
    ::close(running.log_fd);
    running.log_fd = -1;
    return result;
}

void print_result(const Result& result, bool verbose) {
    const auto name = full_name(*result.test);
    if (result.passed) {
        std::cout << "[PASS] " << name << " " << result.elapsed.count() << "ms\n";
        if (verbose && !result.output.empty()) std::cout << result.output;
        return;
    }

    std::cerr << "[FAIL] " << name << " " << result.elapsed.count() << "ms";
    if (result.timed_out)
        std::cerr << " timeout=" << result.deadline.count() << "s";
    else if (result.signal)
        std::cerr << " signal=" << result.signal;
    else
        std::cerr << " exit=" << result.exit_code;
    std::cerr << '\n';
    if (!result.output.empty()) {
        std::cerr << "----- " << name << " output -----\n"
                  << result.output;
        if (result.output.back() != '\n') std::cerr << '\n';
        std::cerr << "----- end output -----\n";
    }
}

} // namespace

Registrar::Registrar(std::string_view group, std::string_view name, TestFunction function,
                     TestCost cost, std::chrono::seconds timeout) {
    registry().push_back({group, name, function, cost, timeout});
}

void check(bool passed, std::string_view expression, const char* file, int line) {
    if (passed) return;
    std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
    child_failures.fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void require_failed(std::string_view expression, const char* file, int line) {
    throw std::runtime_error(std::string(file) + ':' + std::to_string(line) +
                             ": REQUIRE failed: " + std::string(expression));
}

void set_case_index(std::size_t index) noexcept { current_case_index = index; }
std::size_t case_index() noexcept { return current_case_index; }

int run_all(int argc, char** argv) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "test runner: " << e.what() << '\n';
        return 2;
    }

    auto tests = registry();
    std::stable_sort(tests.begin(), tests.end(), [](const auto& a, const auto& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.name < b.name;
    });

    std::vector<const TestCase*> selected_tests;
    selected_tests.reserve(tests.size());
    for (const auto& test : tests)
        if (selected(test, options)) selected_tests.push_back(&test);
    const auto distinct_cases = selected_tests.size();
    // Repetitions are scheduled as further selections of the same cases, so
    // a case's repeats are spread across the run and land next to unrelated
    // work, which is the condition a suspected flake has to be measured in.
    for (unsigned rep = 1; rep < options.repeat; ++rep)
        for (std::size_t i = 0; i < distinct_cases; ++i)
            selected_tests.push_back(selected_tests[i]);

    if (options.list) {
        for (const auto* test : selected_tests)
            std::cout << full_name(*test) << '\n';
        return 0;
    }
    if (selected_tests.empty()) {
        std::cerr << "No tests selected\n";
        return 2;
    }

    // A case can require more slots than a deliberately small --jobs value.
    // Clamp its effective cost to the configured budget so --jobs=1 is a real
    // serial mode rather than a deadlock.
    auto effective_slots = [&](const TestCase& test) {
        return std::min(options.slots, slots_for(test.cost));
    };

    // Two runners on one machine -- a developer's filtered run beside CI, or
    // two sweeps -- would otherwise hand the same case index, and therefore
    // the same 64-port block, to two different tests at once. Every test
    // cluster shares one deterministic key, so a collision is not a bind
    // failure but a foreign node authenticating into this test's cluster
    // ("metadata write-floor policy mismatch peer=..." from a test that
    // started no such peer, 2026-09-14). Each runner therefore shifts the
    // block namespace by a salt its children inherit; set it explicitly to
    // reproduce a run's exact ports.
    if (!std::getenv("MACHA_TEST_PORT_SALT")) {
        const auto salt = static_cast<unsigned>(
            (static_cast<unsigned long>(::getpid()) * 2654435761UL) ^
            static_cast<unsigned long>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        ::setenv("MACHA_TEST_PORT_SALT", std::to_string(salt % 1000003u).c_str(), 1);
    }

    const auto suite_started = Clock::now();
    std::vector<RunningCase> running;
    std::vector<Result> results;
    results.reserve(selected_tests.size());
    std::vector<std::size_t> pending;
    pending.reserve(selected_tests.size());
    for (std::size_t i = 0; i < selected_tests.size(); ++i) pending.push_back(i);
    unsigned occupied = 0;

    std::cout << "Running " << distinct_cases << " tests with " << options.slots
              << " parallel slots (process isolated)";
    if (options.repeat != 1) std::cout << ", x" << options.repeat << " repeats";
    if (options.timeout_scale != 1) std::cout << ", deadlines x" << options.timeout_scale;
    std::cout << '\n' << std::flush;

    while (results.size() < selected_tests.size()) {
        bool launched_any = false;
        while (!pending.empty()) {
            const auto available = options.slots - occupied;
            const auto candidate = std::find_if(pending.begin(), pending.end(), [&](std::size_t i) {
                return effective_slots(*selected_tests[i]) <= available;
            });
            if (candidate == pending.end()) break;

            const auto selection_index = *candidate;
            const auto& test = *selected_tests[selection_index];
            const auto need = effective_slots(test);
            auto child = launch(test, selection_index, test.timeout * options.timeout_scale);
            child.slots = need;
            occupied += need;
            running.push_back(std::move(child));
            pending.erase(candidate);
            launched_any = true;
        }

        bool finished_any = false;
        for (std::size_t i = 0; i < running.size();) {
            auto result = poll_finished(running[i]);
            if (!result) {
                ++i;
                continue;
            }
            occupied -= running[i].slots;
            print_result(*result, options.verbose);
            results.push_back(std::move(*result));
            running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
            finished_any = true;
        }

        // Child completion is the only event that can free scheduler slots.
        // This short parent-only poll interval does not delay or pace a test.
        if (!finished_any && (!launched_any || pending.empty()))
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - suite_started);
    const auto failed = static_cast<std::size_t>(std::count_if(
        results.begin(), results.end(), [](const auto& result) { return !result.passed; }));
    std::chrono::milliseconds serial_time{};
    for (const auto& result : results) serial_time += result.elapsed;

    if (failed) {
        std::cerr << failed << '/' << results.size() << " tests failed; wall=" << elapsed.count()
                  << "ms case-sum=" << serial_time.count() << "ms\n";
        if (options.repeat == 1) {
            std::cerr << "Failed cases:\n";
            for (const auto& result : results)
                if (!result.passed)
                    std::cerr << "  " << full_name(*result.test) << '\n';
        } else {
            // One line per distinct case that failed at least once, with its
            // rate: the number this whole mode exists to produce.
            std::cerr << "Failed cases (failures/" << options.repeat << " runs):\n";
            std::vector<std::pair<const TestCase*, unsigned>> tally;
            for (const auto& result : results) {
                if (result.passed) continue;
                auto found = std::find_if(tally.begin(), tally.end(), [&](const auto& item) {
                    return item.first == result.test;
                });
                if (found == tally.end())
                    tally.emplace_back(result.test, 1u);
                else
                    ++found->second;
            }
            std::stable_sort(tally.begin(), tally.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
            for (const auto& [test, count] : tally)
                std::cerr << "  " << count << '/' << options.repeat << "  " << full_name(*test)
                          << '\n';
        }
        return 1;
    }
    std::cout << "All " << results.size() << " tests passed; wall=" << elapsed.count()
              << "ms case-sum=" << serial_time.count() << "ms";
    if (elapsed.count() > 0)
        std::cout << " effective_parallelism="
                  << static_cast<double>(serial_time.count()) / static_cast<double>(elapsed.count());
    std::cout << '\n';
    return 0;
}

} // namespace macha::test
