/*
 * Radiko authentication (auth1 + auth2).
 *
 * auth1 returns a token plus a (offset,length) into a well-known app key; the
 * base64 of that key slice is the "partial key" sent to auth2, which returns the
 * listener's area. The resulting token authorises the stream (Phase 12).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char token[256];   // X-Radiko-AuthToken (for stream requests)
    char area[16];     // e.g. "JP14"
} radiko_auth_t;

// Run auth1 -> derive partial key -> auth2. Fills *out on success.
esp_err_t radiko_authenticate(radiko_auth_t *out);

#ifdef __cplusplus
}
#endif
