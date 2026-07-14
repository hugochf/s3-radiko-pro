#include <string.h>
#include "hls_parse.h"
#include "unity.h"

static void test_first_url_line_skips_comments_and_blanks(void)
{
    char out[128];
    const char *body =
        "#EXTM3U\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=52973\r\n"
        "https://f-radiko.smartstream.ne.jp/TBS/x/chunklist.m3u8\r\n";
    TEST_ASSERT_TRUE(hls_first_url_line(body, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "https://f-radiko.smartstream.ne.jp/TBS/x/chunklist.m3u8", out);
}

static void test_first_url_line_lf_only_and_no_trailing_newline(void)
{
    char out[64];
    TEST_ASSERT_TRUE(hls_first_url_line("#c\nhttp://x/y", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://x/y", out);
}

static void test_first_url_line_none_found(void)
{
    char out[64];
    TEST_ASSERT_FALSE(hls_first_url_line("#only\n#comments\n\n", out, sizeof(out)));
    TEST_ASSERT_FALSE(hls_first_url_line("", out, sizeof(out)));
}

static void test_first_url_line_truncates_to_cap(void)
{
    char out[8];
    TEST_ASSERT_TRUE(hls_first_url_line("http://very-long-url\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://", out);   // 7 chars + NUL
}

static void test_backoff_doubles_and_caps(void)
{
    TEST_ASSERT_EQUAL_INT(1000, hls_next_backoff(0));
    TEST_ASSERT_EQUAL_INT(2000, hls_next_backoff(1000));
    TEST_ASSERT_EQUAL_INT(4000, hls_next_backoff(2000));
    TEST_ASSERT_EQUAL_INT(8000, hls_next_backoff(4000));
    TEST_ASSERT_EQUAL_INT(10000, hls_next_backoff(8000));    // capped, not 16000
    TEST_ASSERT_EQUAL_INT(10000, hls_next_backoff(10000));   // stays at cap
}

static void test_auth_rejected_only_401_403(void)
{
    TEST_ASSERT_TRUE(hls_auth_rejected(-401));
    TEST_ASSERT_TRUE(hls_auth_rejected(-403));
    TEST_ASSERT_FALSE(hls_auth_rejected(-404));
    TEST_ASSERT_FALSE(hls_auth_rejected(-1));    // transport error
    TEST_ASSERT_FALSE(hls_auth_rejected(0));
    TEST_ASSERT_FALSE(hls_auth_rejected(1234));  // success (body length)
}

void run_hls_parse_tests(void)
{
    RUN_TEST(test_first_url_line_skips_comments_and_blanks);
    RUN_TEST(test_first_url_line_lf_only_and_no_trailing_newline);
    RUN_TEST(test_first_url_line_none_found);
    RUN_TEST(test_first_url_line_truncates_to_cap);
    RUN_TEST(test_backoff_doubles_and_caps);
    RUN_TEST(test_auth_rejected_only_401_403);
}
