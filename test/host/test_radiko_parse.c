#include <string.h>
#include "puff.h"
#include "radiko_parse.h"
#include "unity.h"

// ---- xml_unescape ----

static void test_unescape_all_entities(void)
{
    char s[] = "A&amp;B &lt;x&gt; &quot;q&quot; &apos;a&apos; &#39;n&#39;";
    radiko_xml_unescape(s);
    TEST_ASSERT_EQUAL_STRING("A&B <x> \"q\" 'a' 'n'", s);
}

static void test_unescape_passes_utf8_and_unknown_entities(void)
{
    char s[] = "こんにちは&nbsp;世界";   // unknown entity is kept verbatim
    radiko_xml_unescape(s);
    TEST_ASSERT_EQUAL_STRING("こんにちは&nbsp;世界", s);
}

// ---- gzip_body + puff (round-trip on a real gzip member) ----

// python3: gzip.compress("Hello, Radiko! こんにちは".encode(), 6)
static const unsigned char GZ_FIXTURE[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xf3, 0x48,
    0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x08, 0x4a, 0x4c, 0xc9, 0xcc, 0xce, 0x57,
    0x54, 0x78, 0xdc, 0x38, 0xf9, 0x71, 0xd3, 0xe4, 0xc7, 0x8d, 0xab, 0x1f,
    0x37, 0x2e, 0x7c, 0xdc, 0xb8, 0x1e, 0x00, 0xbc, 0x00, 0x0c, 0x1d, 0x1e,
    0x00, 0x00, 0x00,
};
#define GZ_FIXTURE_ORIG "Hello, Radiko! こんにちは"

static void test_gzip_roundtrip_through_puff(void)
{
    size_t dlen = 0;
    const uint8_t *deflate = radiko_gzip_body(GZ_FIXTURE, sizeof(GZ_FIXTURE), &dlen);
    TEST_ASSERT_NOT_NULL(deflate);

    unsigned char out[128];
    unsigned long outlen = sizeof(out) - 1, srclen = dlen;
    TEST_ASSERT_EQUAL_INT(0, puff(out, &outlen, deflate, &srclen));
    out[outlen] = '\0';
    TEST_ASSERT_EQUAL_STRING(GZ_FIXTURE_ORIG, (const char *)out);
}

static void test_gzip_body_fname_flag(void)
{
    // Header with FNAME: 10-byte base + "a.xml\0", tiny fake payload + trailer.
    unsigned char b[10 + 6 + 4 + 8] = {
        0x1f, 0x8b, 0x08, 0x08, 0, 0, 0, 0, 0, 0xff,
        'a', '.', 'x', 'm', 'l', 0,
        0xAA, 0xBB, 0xCC, 0xDD,
    };
    size_t dlen = 0;
    const uint8_t *d = radiko_gzip_body(b, sizeof(b), &dlen);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_size_t(4, dlen);
    TEST_ASSERT_EQUAL_UINT8(0xAA, d[0]);
}

static void test_gzip_body_rejects_bad_input(void)
{
    size_t dlen = 0;
    unsigned char not_gz[24] = { 'P', 'K', 3, 4 };   // wrong magic
    TEST_ASSERT_NULL(radiko_gzip_body(not_gz, sizeof(not_gz), &dlen));
    TEST_ASSERT_NULL(radiko_gzip_body(GZ_FIXTURE, 10, &dlen));   // truncated
}

// ---- radiko_parse_now ----

static const char *XML =
    "<stations>"
    "<station id=\"TBS\"><name>TBSラジオ</name>"
    "<progs><prog><title>荻上チキ&amp;Session</title>"
    "<pfm>荻上チキ / 南部広美</pfm></prog>"
    "<prog><title>NEXT SHOW</title></prog></progs></station>"
    "<station id=\"QRR\"><name>文化放送</name>"
    "<progs><prog><title>くにまるジャパン</title><pfm></pfm></prog>"
    "</progs></station>"
    "</stations>";

static void test_parse_now_title_and_pfm(void)
{
    char out[256];
    radiko_parse_now(XML, "TBS", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("荻上チキ&Session / 荻上チキ / 南部広美", out);
}

static void test_parse_now_empty_pfm_gives_title_only(void)
{
    char out[256];
    radiko_parse_now(XML, "QRR", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("くにまるジャパン", out);   // no trailing " / "
}

static void test_parse_now_missing_station_is_empty(void)
{
    char out[256] = "junk";
    radiko_parse_now(XML, "LFR", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_parse_now_does_not_cross_station_boundary(void)
{
    // Station block with no <title> of its own must not steal the next one's.
    const char *xml =
        "<station id=\"AAA\"><progs></progs></station>"
        "<station id=\"BBB\"><progs><prog><title>Bs Show</title></prog>"
        "</progs></station>";
    char out[64];
    radiko_parse_now(xml, "AAA", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    radiko_parse_now(xml, "BBB", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Bs Show", out);
}

static void test_parse_now_id_prefix_does_not_cross_match(void)
{
    // "JOAK" must not match "JOAK-FM" (the closing quote is part of the tag).
    const char *xml =
        "<station id=\"JOAK-FM\"><progs><prog><title>FM Prog</title></prog>"
        "</progs></station>";
    char out[64];
    radiko_parse_now(xml, "JOAK", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    radiko_parse_now(xml, "JOAK-FM", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("FM Prog", out);
}

void run_radiko_parse_tests(void)
{
    RUN_TEST(test_unescape_all_entities);
    RUN_TEST(test_unescape_passes_utf8_and_unknown_entities);
    RUN_TEST(test_gzip_roundtrip_through_puff);
    RUN_TEST(test_gzip_body_fname_flag);
    RUN_TEST(test_gzip_body_rejects_bad_input);
    RUN_TEST(test_parse_now_title_and_pfm);
    RUN_TEST(test_parse_now_empty_pfm_gives_title_only);
    RUN_TEST(test_parse_now_missing_station_is_empty);
    RUN_TEST(test_parse_now_does_not_cross_station_boundary);
    RUN_TEST(test_parse_now_id_prefix_does_not_cross_match);
}
