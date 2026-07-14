#include <string.h>
#include "ota_parse.h"
#include "unity.h"

// Trimmed but structurally faithful /releases/latest response: several fields
// we ignore, a non-.bin asset first, the .bin asset second.
static const char *RELEASE_JSON =
    "{\"url\":\"https://api.github.com/repos/x/y/releases/1\","
    "\"tag_name\":\"v0.22.1\",\"name\":\"Phase 22 test\",\"draft\":false,"
    "\"assets\":["
    "{\"name\":\"notes.txt\",\"browser_download_url\":"
    "\"https://github.com/x/y/releases/download/v0.22.1/notes.txt\"},"
    "{\"name\":\"s3_radiko_pro.bin\",\"browser_download_url\":"
    "\"https://github.com/x/y/releases/download/v0.22.1/s3_radiko_pro.bin\"}"
    "],\"body\":\"changelog\"}";

static void test_parse_release_tag_and_bin_asset(void)
{
    char tag[32], url[256];
    TEST_ASSERT_TRUE(ota_parse_release(RELEASE_JSON, tag, sizeof(tag),
                                       url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("v0.22.1", tag);
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/x/y/releases/download/v0.22.1/s3_radiko_pro.bin",
        url);
}

static void test_parse_release_requires_bin_asset(void)
{
    char tag[32], url[256];
    const char *no_bin =
        "{\"tag_name\":\"v1.0.0\",\"assets\":[{\"browser_download_url\":"
        "\"https://x/y/notes.txt\"}]}";
    TEST_ASSERT_FALSE(ota_parse_release(no_bin, tag, sizeof(tag),
                                        url, sizeof(url)));
    TEST_ASSERT_FALSE(ota_parse_release("{}", tag, sizeof(tag),
                                        url, sizeof(url)));
}

static void test_version_cmp(void)
{
    TEST_ASSERT_TRUE(ota_version_cmp("v0.22.1", "0.22.0") > 0);
    TEST_ASSERT_TRUE(ota_version_cmp("0.22.0", "v0.22.1") < 0);
    TEST_ASSERT_EQUAL_INT(0, ota_version_cmp("v1.2.0", "1.2"));   // missing = 0
    TEST_ASSERT_TRUE(ota_version_cmp("1.0.0", "0.99.9") > 0);     // numeric, not lexical
    TEST_ASSERT_TRUE(ota_version_cmp("0.9.0", "0.10.0") < 0);
    TEST_ASSERT_EQUAL_INT(0, ota_version_cmp("2.0.0", "V2.0.0"));
}

void run_ota_parse_tests(void)
{
    RUN_TEST(test_parse_release_tag_and_bin_asset);
    RUN_TEST(test_parse_release_requires_bin_asset);
    RUN_TEST(test_version_cmp);
}
