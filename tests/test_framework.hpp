// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace macha::test {

using TestFunction = void (*)();

enum class TestCost : uint8_t {
    fast = 1,
    integration = 2,
    heavy = 3,
};

struct TestCase {
    std::string_view group;
    std::string_view name;
    TestFunction function{};
    TestCost cost{TestCost::integration};
    std::chrono::seconds timeout{60};
};

class Registrar {
  public:
    Registrar(std::string_view group, std::string_view name, TestFunction function,
              TestCost cost = TestCost::integration,
              std::chrono::seconds timeout = std::chrono::seconds{60});
};

void check(bool passed, std::string_view expression, const char* file, int line);
[[noreturn]] void require_failed(std::string_view expression, const char* file, int line);

// The parent runner assigns each child a deterministic namespace before the
// test body starts. Test helpers use it for collision-free loopback ports.
void set_case_index(std::size_t index) noexcept;
std::size_t case_index() noexcept;

int run_all(int argc, char** argv);

} // namespace macha::test

#define MACHA_TEST_CONCAT_INNER(a, b) a##b
#define MACHA_TEST_CONCAT(a, b) MACHA_TEST_CONCAT_INNER(a, b)
#define MACHA_TEST_IMPL(group_name, test_name, cost_name, timeout_seconds)                           \
    static void test_name();                                                                        \
    static const ::macha::test::Registrar MACHA_TEST_CONCAT(test_registrar_, __LINE__)(             \
        group_name, #test_name, &test_name, cost_name, std::chrono::seconds{timeout_seconds});       \
    static void test_name()

#define MACHA_TEST(group_name, test_name)                                                            \
    MACHA_TEST_IMPL(group_name, test_name, ::macha::test::TestCost::integration, 60)
#define MACHA_FAST_TEST(group_name, test_name)                                                       \
    MACHA_TEST_IMPL(group_name, test_name, ::macha::test::TestCost::fast, 30)
#define MACHA_HEAVY_TEST(group_name, test_name)                                                      \
    MACHA_TEST_IMPL(group_name, test_name, ::macha::test::TestCost::heavy, 120)

#define CHECK(expr) ::macha::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define REQUIRE(expr)                                                                               \
    do {                                                                                            \
        if (!(expr)) ::macha::test::require_failed(#expr, __FILE__, __LINE__);                      \
    } while (0)
