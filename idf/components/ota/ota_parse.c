#include "ota_parse.h"

#include <stdlib.h>
#include <string.h>

// Copy the JSON string value that follows `key` (e.g. "\"tag_name\":") into
// out. Returns false if the key or a well-formed value is missing.
static bool json_str_after(const char *json, const char *key,
                           char *out, size_t cap)
{
    const char *k = strstr(json, key);
    if (!k) return false;
    const char *q1 = strchr(k + strlen(key), '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t)(q2 - q1 - 1);
    if (n >= cap) n = cap - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
}

bool ota_parse_release(const char *json, char *tag, size_t tag_cap,
                       char *url, size_t url_cap)
{
    if (!json_str_after(json, "\"tag_name\"", tag, tag_cap)) return false;

    // First browser_download_url whose value ends in ".bin".
    const char *p = json;
    while ((p = strstr(p, "\"browser_download_url\"")) != NULL) {
        char u[256];
        if (json_str_after(p, "\"browser_download_url\"", u, sizeof(u))) {
            size_t n = strlen(u);
            if (n > 4 && strcmp(u + n - 4, ".bin") == 0) {
                if (n >= url_cap) return false;
                memcpy(url, u, n + 1);
                return true;
            }
        }
        p += 1;
    }
    return false;
}

int ota_version_cmp(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (int i = 0; i < 3; i++) {
        long na = strtol(a, (char **)&a, 10);
        long nb = strtol(b, (char **)&b, 10);
        if (na != nb) return na < nb ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}
