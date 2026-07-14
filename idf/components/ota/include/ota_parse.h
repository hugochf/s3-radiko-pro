/*
 * Pure helpers for the OTA release check — libc only, host-unit-tested
 * (test/host), same discipline as hls_parse/radiko_parse.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extract "tag_name" and the first asset "browser_download_url" ending in
// ".bin" from a GitHub /releases/latest JSON body. Tolerant scanner, not a
// JSON parser: GitHub's field order and escaping for these fields are stable.
bool ota_parse_release(const char *json, char *tag, size_t tag_cap,
                       char *url, size_t url_cap);

// Compare dotted versions numerically ("v" / "V" prefixes ignored):
// negative if a<b, 0 if equal, positive if a>b. Missing fields count as 0
// ("1.2" == "1.2.0").
int ota_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
