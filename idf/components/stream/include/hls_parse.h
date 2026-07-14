/*
 * Pure HLS/stream helpers — libc only, no ESP-IDF dependencies, so they are
 * unit-tested on the host (test/host). Keep them free of I/O and state.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// First non-comment, non-empty line of an m3u8 body -> out (truncated to cap).
bool hls_first_url_line(const char *body, char *out, size_t cap);

// Capped exponential backoff: 0 -> 1000 ms, doubling to a 10 s ceiling.
int hls_next_backoff(int cur_ms);

// fetch() returns -status for HTTP errors; 401/403 mean the auth token was
// rejected — retrying with the same token can never succeed.
bool hls_auth_rejected(int fetch_ret);

#ifdef __cplusplus
}
#endif
