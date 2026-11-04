// SPDX-License-Identifier: GPL-3.0-or-later
// BookConversions with a stand-in for ebook-convert: a shell script that
// copies, fails or hangs, chosen by the output's extension.
#include "catalogue/book_conversion.hpp"

#include "test_support.hpp"

#include <fstream>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// Copies with a marker for epub, hangs for pdf, fails for fb2, writes
// nothing for mobi.
std::filesystem::path fake_converter(const std::filesystem::path& dir) {
    const auto path = dir / "ebook-convert";
    std::ofstream out(path);
    out << "#!/bin/sh\n"
           "case \"$2\" in\n"
           "  *.epub) { printf 'converted:'; cat \"$1\"; } > \"$2\" ;;\n"
           "  *.azw3) printf 'ok' > \"$2\" ;;\n"
           "  *.pdf) sleep 30 ;;\n"
           "  *.fb2) echo 'conversion error: bad input' >&2; exit 3 ;;\n"
           "  *.mobi) exit 0 ;;\n"
           "esac\n";
    out.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
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
    config.ebook_convert = fake_converter(dir.path());
    config.timeout = 1s;
    config.max_concurrent = 2;
    const std::string media = "macha:" + std::string(64, 'a');
    {
        BookConversions conversions(config, dir.path() / "state");
        conversions.start();
        CHECK((conversions.formats() == std::vector<std::string>{"epub", "azw3", "mobi", "pdf", "fb2"}));
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

        auto failed = conversions.request(media, "fb2", source_of("BOOK"));
        CHECK(failed.phase == ConversionState::Phase::converting);
        auto settled = wait_settled(conversions, media, "fb2");
        CHECK(settled.phase == ConversionState::Phase::failed);
        CHECK(settled.error_code == "conversion_failed");
        CHECK(!conversions.content(media, "fb2").has_value());

        auto empty = conversions.request(media, "mobi", source_of("BOOK"));
        (void)empty;
        CHECK(wait_settled(conversions, media, "mobi").error_code == "conversion_failed");

        conversions.request(media, "pdf", source_of("BOOK"));
        auto hung = wait_settled(conversions, media, "pdf");
        CHECK(hung.error_code == "conversion_timed_out");

        // Too large a source is refused before anything runs.
        config.max_source_bytes = 2;
        BookConversions small(config, dir.path() / "small-state");
        auto refused = small.request(media, "azw3", source_of("BOOK"));
        CHECK(refused.phase == ConversionState::Phase::failed);
        CHECK(refused.error_code == "source_too_large");
        CHECK(small.request(media, "docx", source_of("BOOK")).phase == ConversionState::Phase::failed);

        // An unreadable source fails as such.
        MediaContentSource broken{100, "book.mobi",
                                  [](uint64_t, std::span<uint8_t>) -> size_t { throw std::runtime_error("gone"); }};
        conversions.request(media, "azw3", broken);
        CHECK(wait_settled(conversions, media, "azw3").error_code == "source_unreadable");
        // A failed one is tried again when asked.
        CHECK(conversions.request(media, "azw3", source_of("BOOK")).phase == ConversionState::Phase::converting);
        CHECK(wait_settled(conversions, media, "azw3").phase == ConversionState::Phase::ready);
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
    config.ebook_convert = fake_converter(dir.path());
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
    config.ebook_convert = fake_converter(dir.path());
    config.timeout = 60s;
    BookConversions conversions(config, dir.path() / "state");
    conversions.start();
    const std::string media = "macha:" + std::string(64, 'd');
    conversions.request(media, "pdf", source_of("BOOK"));
    std::this_thread::sleep_for(300ms);
    const auto started = std::chrono::steady_clock::now();
    conversions.stop();
    CHECK(std::chrono::steady_clock::now() - started < 5s);
}
