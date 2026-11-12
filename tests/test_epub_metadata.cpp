// SPDX-License-Identifier: GPL-3.0-or-later
// The EPUB metadata reader over zips built here: EPUB 2 and 3 metadata, the
// cover, identifiers, the XML reader's tolerance, and the zip's refusals.
#include "catalogue/epub_metadata.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <random>

#include <zlib.h>

using namespace macha;
using namespace macha::test_support;

namespace {

struct Member {
    std::string name;
    std::string data;
    bool deflate{};
    bool encrypted{};
    // Added to the declared sizes: the member claims bytes that are not there.
    uint32_t extra_declared{};
};

struct ZipOptions {
    bool zip64_locator{};
    bool zip64_sizes{};
};

void put16(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(Bytes& out, uint32_t value) {
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

void put_text(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

Bytes raw_deflate(const std::string& data) {
    z_stream stream{};
    REQUIRE(deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    Bytes out(deflateBound(&stream, static_cast<uLong>(data.size())) + 16);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

// A zip of the members in order, in the layout the reader expects: local
// headers and data, the central directory, the end record.
Bytes build_zip(const std::vector<Member>& members, const ZipOptions& options = {}) {
    Bytes out;
    struct Written {
        uint32_t crc;
        uint32_t compressed;
        uint32_t uncompressed;
        uint32_t offset;
    };
    std::vector<Written> written;
    for (const auto& member : members) {
        const auto payload = member.deflate ? raw_deflate(member.data)
                                            : Bytes(member.data.begin(), member.data.end());
        Written record{};
        record.crc =
            static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(member.data.data()),
                                        static_cast<uInt>(member.data.size())));
        record.compressed = static_cast<uint32_t>(payload.size()) + member.extra_declared;
        record.uncompressed = static_cast<uint32_t>(member.data.size()) + member.extra_declared;
        record.offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, member.encrypted ? 1 : 0);
        put16(out, member.deflate ? 8 : 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, record.crc);
        put32(out, record.compressed);
        put32(out, record.uncompressed);
        put16(out, static_cast<uint32_t>(member.name.size()));
        // A local extra field the central entry does not carry.
        put16(out, 4);
        put_text(out, member.name);
        put32(out, 0x0000ffff);
        out.insert(out.end(), payload.begin(), payload.end());
        written.push_back(record);
    }
    const auto central_offset = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        const auto& record = written[i];
        put32(out, 0x02014b50);
        put16(out, 20);
        put16(out, 20);
        put16(out, member.encrypted ? 1 : 0);
        put16(out, member.deflate ? 8 : 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, record.crc);
        put32(out, record.compressed);
        put32(out, options.zip64_sizes && i == 0 ? 0xFFFFFFFF : record.uncompressed);
        put16(out, static_cast<uint32_t>(member.name.size()));
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, record.offset);
        put_text(out, member.name);
    }
    const auto central_size = static_cast<uint32_t>(out.size()) - central_offset;
    if (options.zip64_locator) {
        put32(out, 0x07064b50);
        put32(out, 0);
        put32(out, 0);
        put32(out, 0);
        put32(out, 1);
    }
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(members.size()));
    put16(out, static_cast<uint32_t>(members.size()));
    put32(out, central_size);
    put32(out, central_offset);
    put16(out, 0);
    return out;
}

std::string container_xml(const std::string& opf_path) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
           "  <rootfiles>\n"
           "    <rootfile full-path=\"" +
           opf_path +
           "\" media-type=\"application/oebps-package+xml\"/>\n"
           "  </rootfiles>\n"
           "</container>\n";
}

Bytes epub(const std::string& opf_path, const std::string& opf, std::vector<Member> extra = {},
           bool deflated = false) {
    std::vector<Member> members;
    members.push_back({"mimetype", "application/epub+zip", false, false, 0});
    members.push_back({"META-INF/container.xml", container_xml(opf_path), deflated, false, 0});
    members.push_back({opf_path, opf, deflated, false, 0});
    for (auto& member : extra)
        members.push_back(std::move(member));
    return build_zip(members);
}

EpubMetadata read_bytes(const Bytes& zip, const EpubReadLimits& limits = {}) {
    const std::function<size_t(uint64_t, std::span<uint8_t>)> read =
        [&zip](uint64_t offset, std::span<uint8_t> destination) -> size_t {
        if (offset >= zip.size())
            return 0;
        const auto n = std::min(destination.size(), zip.size() - static_cast<size_t>(offset));
        std::copy_n(zip.begin() + static_cast<std::ptrdiff_t>(offset), n, destination.begin());
        return n;
    };
    return read_epub_metadata(zip.size(), read, limits);
}

bool read_throws(const Bytes& zip, const EpubReadLimits& limits = {}) {
    try {
        read_bytes(zip, limits);
        return false;
    } catch (const EpubError&) {
        return true;
    }
}

std::string jpeg_bytes(size_t n) {
    std::string out = "\xFF\xD8\xFF\xE0";
    while (out.size() < n)
        out += static_cast<char>(out.size() & 0xFF);
    return out;
}

const std::string epub2_opf =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" xmlns:opf=\"http://www.idpf.org/2007/opf\""
    " unique-identifier=\"BookId\" version=\"2.0\">\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
    "    <dc:title>Guards!\n      Guards!</dc:title>\n"
    "    <dc:creator opf:role=\"aut\" opf:file-as=\"Pratchett, Terry\">Terry "
    "Pratchett</dc:creator>\n"
    "    <dc:identifier id=\"BookId\" opf:scheme=\"ISBN\">0-552-13462-7</dc:identifier>\n"
    "    <dc:language>en-GB</dc:language>\n"
    "    <dc:publisher>Corgi</dc:publisher>\n"
    "    <dc:date opf:event=\"modification\">2010-01-01</dc:date>\n"
    "    <dc:date>1989-11-01</dc:date>\n"
    "    <dc:description>A tale of &quot;dragons&quot; &amp; the Watch.</dc:description>\n"
    "    <meta name=\"calibre:series_index\" content=\"8\"/>\n"
    "    <meta name=\"calibre:series\" content=\"Discworld\"/>\n"
    "    <meta name=\"cover\" content=\"cover-img\"/>\n"
    "  </metadata>\n"
    "  <manifest>\n"
    "    <item id=\"text\" href=\"text.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
    "    <item id=\"cover-img\" href=\"images/cover.jpg\" media-type=\"image/jpeg\"/>\n"
    "  </manifest>\n"
    "</package>\n";

const std::string epub3_opf =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
    "    <dc:identifier id=\"uid\">urn:isbn:978-0-552-13462-0</dc:identifier>\n"
    "    <dc:title id=\"t1\">Men at Arms</dc:title>\n"
    "    <meta refines=\"#t1\" property=\"title-type\">main</meta>\n"
    "    <meta refines=\"#creator01\" property=\"file-as\">Pratchett, Terry</meta>\n"
    "    <dc:creator id=\"creator01\">Terry Pratchett</dc:creator>\n"
    "    <meta refines=\"#creator01\" property=\"role\" scheme=\"marc:relators\">aut</meta>\n"
    "    <dc:creator id=\"creator02\">Stephen Briggs</dc:creator>\n"
    "    <meta refines=\"#creator02\" property=\"role\" scheme=\"marc:relators\">edt</meta>\n"
    "    <dc:language>en</dc:language>\n"
    "    <dc:date>1993-11-04</dc:date>\n"
    "    <meta property=\"dcterms:modified\">2020-01-01T00:00:00Z</meta>\n"
    "    <meta property=\"belongs-to-collection\" id=\"c1\">Discworld</meta>\n"
    "    <meta refines=\"#c1\" property=\"collection-type\">series</meta>\n"
    "    <meta refines=\"#c1\" property=\"group-position\">15</meta>\n"
    "    <meta property=\"belongs-to-collection\" id=\"c2\">City Watch</meta>\n"
    "    <meta refines=\"#c2\" property=\"group-position\">2.5</meta>\n"
    "    <meta property=\"belongs-to-collection\" id=\"c3\">Box Set</meta>\n"
    "    <meta refines=\"#c3\" property=\"collection-type\">set</meta>\n"
    "  </metadata>\n"
    "  <manifest>\n"
    "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
    "properties=\"nav\"/>\n"
    "    <item id=\"cover\" href=\"cover.png\" media-type=\"image/png\" properties=\"scripted "
    "cover-image\"/>\n"
    "  </manifest>\n"
    "</package>\n";

std::string simple_opf(const std::string& metadata, const std::string& manifest = {}) {
    return "<?xml version=\"1.0\"?>\n"
           "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">\n"
           "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
           "xmlns:opf=\"http://www.idpf.org/2007/opf\">\n" +
           metadata + "</metadata>\n<manifest>\n" + manifest + "</manifest>\n</package>\n";
}

} // namespace

MACHA_FAST_TEST("epub_metadata", test_epub2_role_file_as_meta_cover_and_calibre_series) {
    const auto cover = jpeg_bytes(300);
    const auto zip =
        epub("OEBPS/content.opf", epub2_opf, {{"OEBPS/images/cover.jpg", cover, false, false, 0}});
    const auto metadata = read_bytes(zip);
    CHECK(metadata.title == "Guards! Guards!");
    REQUIRE(metadata.creators.size() == 1);
    CHECK(metadata.creators[0].name == "Terry Pratchett");
    CHECK(metadata.creators[0].role == "aut");
    CHECK(metadata.creators[0].file_as == "Pratchett, Terry");
    REQUIRE(metadata.identifiers.size() == 1);
    CHECK(metadata.identifiers[0] == "0-552-13462-7");
    CHECK(metadata.isbn13 == "9780552134620");
    CHECK(metadata.language == "en-GB");
    CHECK(metadata.publisher == "Corgi");
    CHECK(metadata.date == "1989-11-01");
    CHECK(metadata.description == "A tale of \"dragons\" & the Watch.");
    REQUIRE(metadata.series.size() == 1);
    CHECK(metadata.series[0].name == "Discworld");
    CHECK(metadata.series[0].position == 8.0);
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/jpeg");
    CHECK(std::string(metadata.cover->bytes.begin(), metadata.cover->bytes.end()) == cover);
}

MACHA_FAST_TEST("epub_metadata", test_epub3_refines_cover_image_and_collections) {
    const std::string cover = "\x89PNG\r\n\x1a\n....";
    const auto zip = epub("content.opf", epub3_opf, {{"cover.png", cover, false, false, 0}});
    const auto metadata = read_bytes(zip);
    CHECK(metadata.title == "Men at Arms");
    REQUIRE(metadata.creators.size() == 2);
    CHECK(metadata.creators[0].name == "Terry Pratchett");
    CHECK(metadata.creators[0].role == "aut");
    CHECK(metadata.creators[0].file_as == "Pratchett, Terry");
    CHECK(metadata.creators[1].name == "Stephen Briggs");
    CHECK(metadata.creators[1].role == "edt");
    CHECK(metadata.creators[1].file_as.empty());
    CHECK(metadata.isbn13 == "9780552134620");
    CHECK(metadata.language == "en");
    CHECK(metadata.date == "1993-11-04");
    CHECK(!metadata.publisher.has_value());
    CHECK(!metadata.description.has_value());
    // The set is not a series; the untyped collection is.
    REQUIRE(metadata.series.size() == 2);
    CHECK(metadata.series[0].name == "Discworld");
    CHECK(metadata.series[0].position == 15.0);
    CHECK(metadata.series[1].name == "City Watch");
    CHECK(metadata.series[1].position == 2.5);
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/png");
    CHECK(std::string(metadata.cover->bytes.begin(), metadata.cover->bytes.end()) == cover);
}

MACHA_FAST_TEST("epub_metadata", test_two_creators_one_an_editor_in_epub2_attributes) {
    const auto opf = simple_opf("<dc:title>Anthology</dc:title>\n"
                                "<dc:creator opf:role=\"edt\">Ellen Datlow</dc:creator>\n"
                                "<dc:creator opf:role=\"aut\">Kelly Link</dc:creator>\n"
                                "<dc:creator>Nobody Known</dc:creator>\n");
    const auto metadata = read_bytes(epub("book.opf", opf));
    REQUIRE(metadata.creators.size() == 3);
    CHECK(metadata.creators[0].name == "Ellen Datlow");
    CHECK(metadata.creators[0].role == "edt");
    CHECK(metadata.creators[1].name == "Kelly Link");
    CHECK(metadata.creators[1].role == "aut");
    CHECK(metadata.creators[2].role.empty());
}

MACHA_FAST_TEST("epub_metadata", test_series_deduplicated_across_calibre_and_collections) {
    const auto opf =
        simple_opf("<dc:title>Feet of Clay</dc:title>\n"
                   "<meta name=\"calibre:series\" content=\"Discworld\"/>\n"
                   "<meta property=\"belongs-to-collection\" id=\"c1\">Discworld</meta>\n"
                   "<meta refines=\"#c1\" property=\"group-position\">19</meta>\n");
    const auto metadata = read_bytes(epub("book.opf", opf));
    REQUIRE(metadata.series.size() == 1);
    CHECK(metadata.series[0].name == "Discworld");
    // Calibre named it first without a position; the collection supplied one.
    CHECK(metadata.series[0].position == 19.0);
}

MACHA_FAST_TEST("epub_metadata", test_identifiers_and_isbn_detection) {
    // A UUID first, an ISBN-10 with hyphens, a urn, a bad check digit.
    auto metadata = read_bytes(
        epub("book.opf",
             simple_opf("<dc:identifier "
                        "id=\"uid\">urn:uuid:7f3b2c10-4f9e-4a7b-9a2f-5e1c8d2b1a00</dc:identifier>\n"
                        "<dc:identifier>0-306-40615-3</dc:identifier>\n"
                        "<dc:identifier> 0-306-40615-2 </dc:identifier>\n"
                        "<dc:identifier>urn:isbn:9783161484100</dc:identifier>\n")));
    REQUIRE(metadata.identifiers.size() == 4);
    CHECK(metadata.identifiers[0] == "urn:uuid:7f3b2c10-4f9e-4a7b-9a2f-5e1c8d2b1a00");
    CHECK(metadata.identifiers[1] == "0-306-40615-3");
    CHECK(metadata.identifiers[2] == "0-306-40615-2");
    CHECK(metadata.identifiers[3] == "urn:isbn:9783161484100");
    // The bad check digit is skipped; the first good ISBN wins, converted.
    CHECK(metadata.isbn13 == "9780306406157");

    // An identifier declared as an ISBN beats an earlier one found by shape.
    metadata = read_bytes(
        epub("book.opf",
             simple_opf("<dc:identifier>urn:isbn:9780306406157</dc:identifier>\n"
                        "<dc:identifier opf:scheme=\"ISBN\">978-3-16-148410-0</dc:identifier>\n")));
    CHECK(metadata.isbn13 == "9783161484100");

    // Nothing ISBN-shaped: no ISBN.
    metadata =
        read_bytes(epub("book.opf", simple_opf("<dc:identifier>urn:uuid:abc</dc:identifier>\n"
                                               "<dc:identifier>0-306-40615-3</dc:identifier>\n")));
    CHECK(!metadata.isbn13.has_value());
    CHECK(metadata.identifiers.size() == 2);
}

MACHA_FAST_TEST("epub_metadata", test_description_with_markup_and_entities) {
    auto metadata = read_bytes(epub(
        "book.opf",
        simple_opf(
            "<dc:title>T</dc:title>\n"
            "<dc:description>&lt;p&gt;A &lt;b&gt;bold&lt;/b&gt; tale &amp;amp; more.&lt;/p&gt;\n\n"
            "   second   line&lt;br/&gt;</dc:description>\n")));
    CHECK(metadata.description == "A bold tale & more. second line");

    // Markup arriving in CDATA is markup all the same; the plain case keeps
    // its angle-free text and decoded entities.
    metadata = read_bytes(
        epub("book.opf",
             simple_opf(
                 "<dc:description><![CDATA[<p>Raw &amp; <i>markup</i></p>]]></dc:description>\n")));
    CHECK(metadata.description == "Raw & markup");

    metadata =
        read_bytes(epub("book.opf", simple_opf("<dc:description>  Caf&#233; &#x2019;quoted&#x2019; "
                                               "3 &gt; 2 &amp; 1 &lt; 2  </dc:description>\n")));
    CHECK(metadata.description == "Caf\xC3\xA9 \xE2\x80\x99quoted\xE2\x80\x99 3 > 2 & 1 < 2");
}

MACHA_FAST_TEST("epub_metadata", test_bom_and_crlf) {
    std::string opf = "\xEF\xBB\xBF";
    for (char c : epub2_opf) {
        if (c == '\n')
            opf += '\r';
        opf += c;
    }
    const auto container = "\xEF\xBB\xBF" + container_xml("OEBPS/content.opf");
    const auto zip = build_zip({{"META-INF/container.xml", container, false, false, 0},
                                {"OEBPS/content.opf", opf, false, false, 0}});
    const auto metadata = read_bytes(zip);
    CHECK(metadata.title == "Guards! Guards!");
    CHECK(metadata.creators.size() == 1);
    CHECK(metadata.isbn13 == "9780552134620");
    CHECK(metadata.series.size() == 1);
    // The cover member is absent: no cover, no failure.
    CHECK(!metadata.cover.has_value());
}

MACHA_FAST_TEST("epub_metadata", test_prefixed_and_default_namespace_opf_read_alike) {
    const std::string prefixed =
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<opf:package xmlns:opf='http://www.idpf.org/2007/opf' "
        "xmlns:dc='http://purl.org/dc/elements/1.1/' version='2.0'>\n"
        "<opf:metadata>\n"
        "<dc:title>Prefixed</dc:title>\n"
        "<dc:creator opf:role='ill' opf:file-as='Kidby, Paul'>Paul Kidby</dc:creator>\n"
        "<dc:language>en</dc:language>\n"
        "<opf:meta name='cover' content='c'/>\n"
        "</opf:metadata>\n"
        "<opf:manifest>\n"
        "<opf:item id='c' href='c.gif' media-type='image/gif'/>\n"
        "</opf:manifest>\n"
        "</opf:package>\n";
    const std::string plain =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "<dc:title>Prefixed</dc:title>\n"
        "<dc:creator role=\"ill\" file-as=\"Kidby, Paul\">Paul Kidby</dc:creator>\n"
        "<dc:language>en</dc:language>\n"
        "<meta name=\"cover\" content=\"c\"/>\n"
        "</metadata>\n"
        "<manifest>\n"
        "<item id=\"c\" href=\"c.gif\" media-type=\"image/gif\"/>\n"
        "</manifest>\n"
        "</package>\n";
    for (const auto& opf : {prefixed, plain}) {
        const auto metadata =
            read_bytes(epub("x.opf", opf, {{"c.gif", "GIF89a..", false, false, 0}}));
        CHECK(metadata.title == "Prefixed");
        REQUIRE(metadata.creators.size() == 1);
        CHECK(metadata.creators[0].role == "ill");
        CHECK(metadata.creators[0].file_as == "Kidby, Paul");
        CHECK(metadata.language == "en");
        REQUIRE(metadata.cover.has_value());
        CHECK(metadata.cover->mime_type == "image/gif");
        CHECK(metadata.cover->bytes.size() == 8);
    }
}

MACHA_FAST_TEST("epub_metadata",
                test_opf_in_subdirectory_resolves_relative_and_percent_encoded_hrefs) {
    const auto opf = simple_opf(
        "<dc:title>Deep</dc:title>\n",
        "<item id=\"cover\" href=\"../Images/My%20Cover%21.jpg\" properties=\"cover-image\"/>\n");
    auto metadata =
        read_bytes(epub("OEBPS/text/content.opf", opf,
                        {{"OEBPS/Images/My Cover!.jpg", jpeg_bytes(64), false, false, 0}}));
    REQUIRE(metadata.cover.has_value());
    // No media-type on the item: the extension names it.
    CHECK(metadata.cover->mime_type == "image/jpeg");
    CHECK(metadata.cover->bytes.size() == 64);

    // A sibling href, and an absolute one from the zip root.
    metadata =
        read_bytes(epub("OEBPS/content.opf",
                        simple_opf("<dc:title>Deep</dc:title>\n",
                                   "<item id=\"cover\" href=\"./art/cover.webp\" "
                                   "media-type=\"image/webp\" properties=\"cover-image\"/>\n"),
                        {{"OEBPS/art/cover.webp", "RIFF....WEBP", false, false, 0}}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/webp");

    metadata =
        read_bytes(epub("OEBPS/content.opf",
                        simple_opf("<dc:title>Deep</dc:title>\n",
                                   "<item id=\"cover\" href=\"/cover.svg\" "
                                   "media-type=\"image/svg+xml\" properties=\"cover-image\"/>\n"),
                        {{"cover.svg", "<svg/>", false, false, 0}}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/svg+xml");
}

MACHA_FAST_TEST("epub_metadata", test_no_cover_is_not_an_error) {
    // No cover declared.
    auto metadata = read_bytes(epub("book.opf", simple_opf("<dc:title>Bare</dc:title>\n")));
    CHECK(metadata.title == "Bare");
    CHECK(!metadata.cover.has_value());

    // Declared, but the manifest has no such item.
    metadata =
        read_bytes(epub("book.opf", simple_opf("<dc:title>Bare</dc:title>\n"
                                               "<meta name=\"cover\" content=\"nope\"/>\n")));
    CHECK(!metadata.cover.has_value());

    // Declared and in the manifest, but not in the zip.
    metadata = read_bytes(
        epub("book.opf", simple_opf("<dc:title>Bare</dc:title>\n",
                                    "<item id=\"c\" href=\"gone.png\" media-type=\"image/png\" "
                                    "properties=\"cover-image\"/>\n")));
    CHECK(!metadata.cover.has_value());

    // An EPUB 2 meta naming the href rather than the id still finds it.
    metadata = read_bytes(
        epub("book.opf",
             simple_opf("<dc:title>Bare</dc:title>\n"
                        "<meta name=\"cover\" content=\"cover.png\"/>\n",
                        "<item id=\"c\" href=\"cover.png\" media-type=\"image/png\"/>\n"),
             {{"cover.png", "PNG", false, false, 0}}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->bytes.size() == 3);
}

MACHA_FAST_TEST("epub_metadata", test_oversized_or_unreadable_cover_is_skipped) {
    const auto opf = simple_opf("<dc:title>Big</dc:title>\n",
                                "<item id=\"c\" href=\"cover.jpg\" media-type=\"image/jpeg\" "
                                "properties=\"cover-image\"/>\n");
    EpubReadLimits limits;
    limits.max_cover_bytes = 100;
    auto metadata = read_bytes(
        epub("book.opf", opf, {{"cover.jpg", jpeg_bytes(200), false, false, 0}}), limits);
    CHECK(metadata.title == "Big");
    CHECK(!metadata.cover.has_value());

    // Under the cover limit but over what the call has left.
    limits = {};
    limits.max_total_bytes = 2000;
    metadata = read_bytes(epub("book.opf", opf, {{"cover.jpg", jpeg_bytes(1800), false, false, 0}}),
                          limits);
    CHECK(metadata.title == "Big");
    CHECK(!metadata.cover.has_value());

    // An encrypted or truncated cover is skipped, not fatal.
    metadata = read_bytes(epub("book.opf", opf, {{"cover.jpg", jpeg_bytes(50), false, true, 0}}));
    CHECK(!metadata.cover.has_value());
    metadata =
        read_bytes(epub("book.opf", opf, {{"cover.jpg", jpeg_bytes(50), false, false, 100000}}));
    CHECK(metadata.title == "Big");
    CHECK(!metadata.cover.has_value());
}

MACHA_FAST_TEST("epub_metadata", test_oversized_opf_or_container_throws) {
    EpubReadLimits limits;
    // The container fits in 400 bytes; the OPF does not.
    limits.max_member_bytes = 400;
    CHECK(read_throws(epub("book.opf", epub2_opf), limits));
    CHECK(!read_throws(epub("b.opf", simple_opf("<dc:title>x</dc:title>")), limits));
    // Nor does the container in 100.
    limits.max_member_bytes = 100;
    CHECK(read_throws(epub("b.opf", simple_opf("<dc:title>x</dc:title>")), limits));
    limits.max_member_bytes = 1024 * 1024;
    limits.max_total_bytes = 300;
    CHECK(read_throws(epub("book.opf", epub2_opf), limits));
}

MACHA_FAST_TEST("epub_metadata", test_truncated_zip_throws) {
    const auto zip = epub("book.opf", epub2_opf);
    for (size_t cut : {size_t{1}, size_t{10}, size_t{22}, size_t{60}, zip.size() / 2}) {
        Bytes shorter(zip.begin(), zip.end() - static_cast<std::ptrdiff_t>(cut));
        CHECK(read_throws(shorter));
    }
    CHECK(read_throws({}));
    CHECK(read_throws(Bytes(100, 0)));
    // A member whose declared bytes run past the end of the file.
    std::vector<Member> members{
        {"META-INF/container.xml", container_xml("book.opf"), false, false, 0},
        {"book.opf", epub2_opf, false, false, 100000}};
    CHECK(read_throws(build_zip(members)));
    // A deflated member whose stream is cut short of its declared size.
    members = {{"META-INF/container.xml", container_xml("book.opf"), false, false, 0},
               {"book.opf", epub2_opf, true, false, 0}};
    auto zipped = build_zip(members);
    // The local header's name and its four-byte extra field, then the stream.
    const Bytes needle{'b', 'o', 'o', 'k', '.', 'o', 'p', 'f', 0xff, 0xff, 0x00, 0x00};
    auto at = std::search(zipped.begin(), zipped.end(), needle.begin(), needle.end());
    REQUIRE(at != zipped.end());
    const auto stream_start = static_cast<size_t>(at - zipped.begin()) + needle.size();
    for (size_t i = 0; i < 16; ++i)
        zipped[stream_start + i] = 0xFF;
    CHECK(read_throws(zipped));
}

MACHA_FAST_TEST("epub_metadata", test_encrypted_member_throws) {
    std::vector<Member> members{
        {"META-INF/container.xml", container_xml("book.opf"), false, true, 0},
        {"book.opf", epub2_opf, false, false, 0}};
    CHECK(read_throws(build_zip(members)));
    members[0].encrypted = false;
    members[1].encrypted = true;
    CHECK(read_throws(build_zip(members)));
}

MACHA_FAST_TEST("epub_metadata", test_zip64_throws) {
    const std::vector<Member> members{
        {"META-INF/container.xml", container_xml("book.opf"), false, false, 0},
        {"book.opf", epub2_opf, false, false, 0}};
    CHECK(!read_throws(build_zip(members)));
    CHECK(read_throws(build_zip(members, {true, false})));
    CHECK(read_throws(build_zip(members, {false, true})));
}

MACHA_FAST_TEST("epub_metadata", test_unsupported_method_throws) {
    auto zip = build_zip({{"META-INF/container.xml", container_xml("book.opf"), false, false, 0},
                          {"book.opf", epub2_opf, false, false, 0}});
    // Method 12 (bzip2) in the first central entry: offset 10 after the
    // central signature.
    const std::string needle("PK\x01\x02", 4);
    auto at = std::search(zip.begin(), zip.end(), needle.begin(), needle.end());
    REQUIRE(at != zip.end());
    *(at + 10) = 12;
    CHECK(read_throws(zip));
}

MACHA_FAST_TEST("epub_metadata", test_missing_container_rootfile_package_or_metadata_throws) {
    CHECK(read_throws(build_zip({{"book.opf", epub2_opf, false, false, 0}})));
    CHECK(read_throws(build_zip(
        {{"META-INF/container.xml", "<container><rootfiles/></container>", false, false, 0},
         {"book.opf", epub2_opf, false, false, 0}})));
    // The rootfile names a member that is not there.
    CHECK(read_throws(
        build_zip({{"META-INF/container.xml", container_xml("other.opf"), false, false, 0},
                   {"book.opf", epub2_opf, false, false, 0}})));
    CHECK(read_throws(epub("book.opf", "<?xml version=\"1.0\"?><html><body/></html>")));
    CHECK(read_throws(epub("book.opf", "<package version=\"3.0\"><manifest/></package>")));
    CHECK(read_throws(epub("book.opf", "")));
    // Not a zip at all.
    const std::string text = "this is a plain text file and not a zip archive at all, sorry";
    CHECK(read_throws(Bytes(text.begin(), text.end())));
}

MACHA_FAST_TEST("epub_metadata", test_deflated_members_read) {
    const auto cover = jpeg_bytes(5000);
    const auto zip = epub("OEBPS/content.opf", epub2_opf,
                          {{"OEBPS/images/cover.jpg", cover, true, false, 0}}, true);
    const auto metadata = read_bytes(zip);
    CHECK(metadata.title == "Guards! Guards!");
    CHECK(metadata.isbn13 == "9780552134620");
    REQUIRE(metadata.cover.has_value());
    CHECK(std::string(metadata.cover->bytes.begin(), metadata.cover->bytes.end()) == cover);
}

MACHA_FAST_TEST("epub_metadata", test_member_names_are_normalised) {
    for (const std::string name :
         {"./OEBPS/content.opf", "/OEBPS/content.opf", "OEBPS\\content.opf"}) {
        const auto zip = build_zip(
            {{"META-INF\\container.xml", container_xml("./OEBPS/content.opf"), false, false, 0},
             {name, epub2_opf, false, false, 0}});
        CHECK(read_bytes(zip).title == "Guards! Guards!");
    }
}

MACHA_FAST_TEST("epub_metadata", test_xml_reader_tolerates_what_an_opf_may_contain) {
    const std::string opf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE package [ <!ENTITY x \"y\"> ]>\n"
        "<!-- a comment with <dc:title>Not this</dc:title> inside -->\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version='3.0'>\n"
        "<?processing instruction?>\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "<dc:title>Spaced&#x20;<![CDATA[& <raw>]]> Title</dc:title>\n"
        "<dc:creator opf:role='aut' opf:file-as=\"O'Brien, Flann\">Flann "
        "O&apos;Brien</dc:creator>\n"
        "<dc:publisher\n   >Dalkey\n</dc:publisher>\n"
        "<dc:language>ga</dc:language>\n"
        "<meta name=\"cover\" content=\"c\" />\n"
        "<unknown:thing attr=\"&unknown;\">ignored</unknown:thing>\n"
        "</metadata>\n"
        "<manifest><item id=\"c\" href=\"c.png\" media-type=\"image/png\"/></manifest>\n"
        "</package>\n";
    const auto metadata = read_bytes(epub("book.opf", opf, {{"c.png", "PNG", false, false, 0}}));
    CHECK(metadata.title == "Spaced & <raw> Title");
    REQUIRE(metadata.creators.size() == 1);
    CHECK(metadata.creators[0].name == "Flann O'Brien");
    CHECK(metadata.creators[0].file_as == "O'Brien, Flann");
    CHECK(metadata.publisher == "Dalkey");
    CHECK(metadata.language == "ga");
    CHECK(metadata.cover.has_value());
}

MACHA_FAST_TEST("epub_metadata", test_reader_fetches_only_what_it_needs) {
    // A large chapter between the container and the OPF must never be read.
    std::string chapter;
    while (chapter.size() < 300 * 1024)
        chapter += "<p>Lorem ipsum dolor sit amet.</p>\n";
    const auto zip =
        build_zip({{"META-INF/container.xml", container_xml("book.opf"), false, false, 0},
                   {"chapter.xhtml", chapter, false, false, 0},
                   {"book.opf", epub2_opf, false, false, 0}});
    size_t total_read = 0;
    const std::function<size_t(uint64_t, std::span<uint8_t>)> read =
        [&](uint64_t offset, std::span<uint8_t> destination) -> size_t {
        if (offset >= zip.size())
            return 0;
        const auto n = std::min(destination.size(), zip.size() - static_cast<size_t>(offset));
        std::copy_n(zip.begin() + static_cast<std::ptrdiff_t>(offset), n, destination.begin());
        total_read += n;
        return n;
    };
    const auto metadata = read_epub_metadata(zip.size(), read);
    CHECK(metadata.title == "Guards! Guards!");
    // The 66 KiB tail, the central directory, two small members.
    CHECK(total_read < 80 * 1024);
}

MACHA_FAST_TEST("epub_metadata", test_host_file_overload) {
    TempDir dir;
    const auto path = dir.path() / "book.epub";
    const auto cover = jpeg_bytes(2048);
    const auto zip = epub("OEBPS/content.opf", epub2_opf,
                          {{"OEBPS/images/cover.jpg", cover, true, false, 0}}, true);
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(zip.data()),
                   static_cast<std::streamsize>(zip.size()));
    }
    const auto metadata = read_epub_metadata(path);
    CHECK(metadata.title == "Guards! Guards!");
    CHECK(metadata.isbn13 == "9780552134620");
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->bytes.size() == cover.size());

    bool threw = false;
    try {
        read_epub_metadata(dir.path() / "missing.epub");
    } catch (const EpubError&) {
        threw = true;
    }
    CHECK(threw);
}

MACHA_FAST_TEST("epub_metadata", test_normalise_isbn13) {
    CHECK(normalise_isbn13("0-306-40615-2") == "9780306406157");
    CHECK(normalise_isbn13("0306406152") == "9780306406157");
    CHECK(normalise_isbn13("0 306 40615 2") == "9780306406157");
    CHECK(normalise_isbn13("ISBN 0-306-40615-2") == "9780306406157");
    CHECK(normalise_isbn13("isbn:0306406152") == "9780306406157");
    CHECK(normalise_isbn13("ISBN-10: 0-306-40615-2") == "9780306406157");
    CHECK(normalise_isbn13("urn:isbn:0306406152") == "9780306406157");
    CHECK(normalise_isbn13("URN:ISBN:978-0-306-40615-7") == "9780306406157");
    CHECK(normalise_isbn13("9780306406157") == "9780306406157");
    CHECK(normalise_isbn13("  978-3-16-148410-0\n") == "9783161484100");
    CHECK(normalise_isbn13("ISBN-13: 9783161484100") == "9783161484100");
    // The X check digit, upper or lower case.
    CHECK(normalise_isbn13("0-8044-2957-X") == "9780804429573");
    CHECK(normalise_isbn13("080442957x") == "9780804429573");
    // 979 is an ISBN prefix too.
    CHECK(normalise_isbn13("9790000000001") == "9790000000001");

    // Bad check digits, wrong lengths, foreign characters, other EAN prefixes.
    CHECK(!normalise_isbn13("0-306-40615-3"));
    CHECK(!normalise_isbn13("9780306406158"));
    CHECK(!normalise_isbn13("9770000000001"));
    CHECK(!normalise_isbn13("X306406152"));
    CHECK(!normalise_isbn13("030640615"));
    CHECK(!normalise_isbn13("03064061520"));
    CHECK(!normalise_isbn13("urn:uuid:0306406152"));
    CHECK(!normalise_isbn13("0306406152abc"));
    CHECK(!normalise_isbn13(""));
    CHECK(!normalise_isbn13("isbn:"));
    CHECK(!normalise_isbn13("ISBN"));
}

MACHA_FAST_TEST("epub_metadata", test_truncation_fuzz_never_crashes) {
    std::mt19937 rng(20261104);
    const auto whole = epub2_opf;
    size_t returned = 0;
    size_t threw = 0;
    const auto outcome = [&](const Bytes& zip) {
        try {
            read_bytes(zip);
            ++returned;
        } catch (const EpubError&) {
            ++threw;
        }
    };
    // Truncations of the OPF inside a valid zip: a partial document parses
    // or is refused, never anything else.
    for (int i = 0; i < 300; ++i) {
        const auto length = std::uniform_int_distribution<size_t>(0, whole.size())(rng);
        outcome(epub("book.opf", whole.substr(0, length)));
    }
    // Byte flips in the OPF.
    for (int i = 0; i < 200; ++i) {
        auto mutated = whole;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int f = 0; f < flips; ++f)
            mutated[rng() % mutated.size()] = static_cast<char>(rng());
        outcome(epub("book.opf", mutated));
    }
    // Truncations and byte flips of the zip itself.
    const auto zip = epub("book.opf", whole, {{"cover.jpg", jpeg_bytes(64), true, false, 0}}, true);
    for (int i = 0; i < 200; ++i) {
        const auto length = std::uniform_int_distribution<size_t>(0, zip.size())(rng);
        outcome(Bytes(zip.begin(), zip.begin() + static_cast<std::ptrdiff_t>(length)));
    }
    for (int i = 0; i < 200; ++i) {
        auto mutated = zip;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f)
            mutated[rng() % mutated.size()] = static_cast<uint8_t>(rng());
        outcome(mutated);
    }
    CHECK(returned + threw == 900);
    CHECK(returned > 0);
    CHECK(threw > 0);
}
