#include "hls_parse.h"

#include <string.h>

bool hls_first_url_line(const char *body, char *out, size_t cap)
{
    for (const char *p = body; *p; ) {
        const char *nl = strpbrk(p, "\r\n");
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[0] != '#') {
            if (len >= cap) len = cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

int hls_next_backoff(int cur_ms)
{
    if (cur_ms <= 0) return 1000;
    int next = cur_ms * 2;
    return next > 10000 ? 10000 : next;
}

bool hls_auth_rejected(int fetch_ret)
{
    return fetch_ret == -401 || fetch_ret == -403;
}
