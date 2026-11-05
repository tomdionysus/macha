// SPDX-License-Identifier: GPL-3.0-or-later
// BookConversions with a stand-in for mobitool: a shell script that
// converts, fails or hangs, chosen by the book's contents.
#include "catalogue/book_conversion.hpp"

#include "test_support.hpp"

#include <fstream>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// Writes "converted:" and the input for an ordinary book; the input's first
// word picks a failure: HANG sleeps, FAIL exits nonzero, EMPTY writes nothing.
std::filesystem::path fake_converter(const std::filesystem::path& dir) {
    const auto path = dir / "mobitool";
    std::ofstream out(path);
    out << "#!/bin/sh\n"
           "[ \"$1\" = -e ] && [ \"$2\" = -o ] || exit 2\n"
           "in=\"$4\"; name=$(basename \"$in\"); out=\"$3/${name%.*}.epub\"\n"
           "case \"$(head -c 5 \"$in\")\" in\n"
           "  HANG*) sleep 30 ;;\n"
           "  FAIL*) echo 'bad input' >&2; exit 3 ;;\n"
           "  EMPTY*) exit 0 ;;\n"
           "  *) { printf 'converted:'; cat \"$in\"; } > \"$out\" ;;\n"
           "esac\n";
    out.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}

// A Palm database whose first record marks its text encrypted.
std::string encrypted_book() {
    std::string bytes(86, '\0');
    bytes[81] = 86; // record 0 at offset 86
    std::string record0(16, '\0');
    record0[13] = 2; // encryption type 2
    return bytes + record0;
}

MediaContentSource source_of(std::string text, std::string name = "book.mobi") {
    auto bytes = std::make_shared<std::string>(std::move(text));
    return {bytes->size(), std::move(name), [bytes](uint64_t offset, std::span<uint8_t> into) -> size_t {
                if (offset >= bytes->size()) return 0;
                const auto n = std::min<size_t>(into.size(), bytes->size() - offset);
                std::copy_n(bytes->data() + offset, n, into.data());
                return n;
            }};
}

std::string read_all(const MediaContentSource& source) {
    std::string out(source.size, '\0');
    size_t done = 0;
    while (done < out.size()) {
        const auto n = source.read(done, std::span<uint8_t>(reinterpret_cast<uint8_t*>(out.data()) + done,
                                                            out.size() - done));
        if (!n) break;
        done += n;
    }
    out.resize(done);
    return out;
}

ConversionState wait_settled(BookConversions& conversions, const std::string& media,
                             const std::string& format) {
    ConversionState last;
    REQUIRE(wait_until([&] {
        auto state = conversions.state(media, format);
        if (!state) return false;
        last = *state;
        return state->phase != ConversionState::Phase::converting;
    }, 10s));
    return last;
}

} // namespace

MACHA_TEST("book_conversion", test_conversions_run_report_fail_and_are_kept) {
    TempDir dir;
    BookConversionConfig config;
    config.enabled = true;
    config.mobitool = fake_converter(dir.path());
    config.timeout = 1s;
    config.max_concurrent = 2;
    const auto id = [](char c) { return "macha:" + std::string(64, c); };
    const std::string media = id('a');
    {
        BookConversions conversions(config, dir.path() / "state");
        conversions.start();
        CHECK((conversions.formats() == std::vector<std::string>{"epub"}));
        CHECK(!conversions.state(media, "epub").has_value());
        CHECK(!conversions.content(media, "epub").has_value());

        auto started = conversions.request(media, "epub", source_of("BOOK"));
        CHECK(started.phase == ConversionState::Phase::converting);
        auto ready = wait_settled(conversions, media, "epub");
        CHECK(ready.phase == ConversionState::Phase::ready);
        CHECK(ready.size == std::string("converted:BOOK").size());
        auto content = conversions.content(media, "epub");
        REQUIRE(content.has_value());
        CHECK(read_all(*content) == "converted:BOOK");
        // Asking again answers ready without converting again.
        CHECK(conversions.request(media, "epub", source_of("OTHER")).phase == ConversionState::Phase::ready);
        CHECK(read_all(*conversions.content(media, "epub")) == "converted:BOOK");
        // An AZW3 converts the same way.
        conversions.request(id('e'), "epub", source_of("KF8", "book.AZW3"));
        CHECK(wait_settled(conversions, id('e'), "epub").phase == ConversionState::Phase::ready);

        auto failed = conversions.request(id('f'), "epub", source_of("FAIL"));
        CHECK(failed.phase == ConversionState::Phase::converting);
        auto settled = wait_settled(conversions, id('f'), "epub");
        CHECK(settled.phase == ConversionState::Phase::failed);
        CHECK(settled.error_code == "conversion_failed");
        CHECK(!conversions.content(id('f'), "epub").has_value());

        conversions.request(id('1'), "epub", source_of("EMPTY"));
        CHECK(wait_settled(conversions, id('1'), "epub").error_code == "conversion_failed");

        conversions.request(id('2'), "epub", source_of("HANG"));
        CHECK(wait_settled(conversions, id('2'), "epub").error_code == "conversion_timed_out");

        // An encrypted book is refused without running the converter.
        conversions.request(id('3'), "epub", source_of(encrypted_book()));
        CHECK(wait_settled(conversions, id('3'), "epub").error_code == "encrypted");

        // A source mobitool does not read, or another format, is refused at once.
        auto pdf = conversions.request(id('4'), "epub", source_of("BOOK", "book.pdf"));
        CHECK(pdf.phase == ConversionState::Phase::failed);
        CHECK(pdf.error_code == "unsupported_source");
        CHECK(conversions.request(media, "azw3", source_of("BOOK")).phase == ConversionState::Phase::failed);

        // Too large a source is refused before anything runs.
        config.max_source_bytes = 2;
        BookConversions small(config, dir.path() / "small-state");
        auto refused = small.request(media, "epub", source_of("BOOK"));
        CHECK(refused.phase == ConversionState::Phase::failed);
        CHECK(refused.error_code == "source_too_large");

        // An unreadable source fails as such.
        MediaContentSource broken{100, "book.mobi",
                                  [](uint64_t, std::span<uint8_t>) -> size_t { throw std::runtime_error("gone"); }};
        conversions.request(id('5'), "epub", broken);
        CHECK(wait_settled(conversions, id('5'), "epub").error_code == "source_unreadable");
        // A failed one is tried again when asked.
        CHECK(conversions.request(id('5'), "epub", source_of("BOOK")).phase == ConversionState::Phase::converting);
        CHECK(wait_settled(conversions, id('5'), "epub").phase == ConversionState::Phase::ready);
        conversions.stop();
    }

    // A restart finds the finished copy; erase drops it.
    config.max_source_bytes = 512ULL * 1024 * 1024;
    BookConversions again(config, dir.path() / "state");
    again.start();
    auto kept = again.state(media, "epub");
    REQUIRE(kept.has_value());
    CHECK(kept->phase == ConversionState::Phase::ready);
    CHECK(read_all(*again.content(media, "epub")) == "converted:BOOK");
    CHECK(again.erase(media, "epub"));
    CHECK(!again.state(media, "epub").has_value());
    CHECK(!again.erase(media, "epub"));
    again.stop();
}

MACHA_TEST("book_conversion", test_the_cache_keeps_the_most_recently_served_within_its_limit) {
    TempDir dir;
    BookConversionConfig config;
    config.enabled = true;
    config.mobitool = fake_converter(dir.path());
    config.cache_limit = 64ULL * 1024 * 1024;
    BookConversions conversions(config, dir.path() / "state");
    conversions.start();
    const std::string big(40 * 1024 * 1024, 'x');
    const std::string first = "macha:" + std::string(64, 'b');
    const std::string second = "macha:" + std::string(64, 'c');
    conversions.request(first, "epub", source_of(big));
    CHECK(wait_settled(conversions, first, "epub").phase == ConversionState::Phase::ready);
    std::this_thread::sleep_for(20ms);
    conversions.request(second, "epub", source_of(big));
    CHECK(wait_settled(conversions, second, "epub").phase == ConversionState::Phase::ready);
    // Two 40 MiB copies pass the 64 MiB limit: the older one goes.
    CHECK(!conversions.state(first, "epub").has_value());
    CHECK(conversions.state(second, "epub").has_value());
    conversions.stop();
}

MACHA_TEST("book_conversion", test_stop_kills_a_running_conversion) {
    TempDir dir;
    BookConversionConfig config;
    config.enabled = true;
    config.mobitool = fake_converter(dir.path());
    config.timeout = 60s;
    BookConversions conversions(config, dir.path() / "state");
    conversions.start();
    const std::string media = "macha:" + std::string(64, 'd');
    conversions.request(media, "epub", source_of("HANG"));
    std::this_thread::sleep_for(300ms);
    const auto started = std::chrono::steady_clock::now();
    conversions.stop();
    CHECK(std::chrono::steady_clock::now() - started < 5s);
}
