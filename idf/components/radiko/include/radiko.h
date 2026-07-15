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

// Run auth1 -> derive partial key -> auth2. Fills *out on success and caches it.
esp_err_t radiko_authenticate(radiko_auth_t *out);

// Last successful auth (empty string until authenticated).
const char *radiko_token(void);
const char *radiko_area(void);

// Phase 30: which prefecture to authenticate AS (JP number 1..47, default 13
// Tokyo). This drives the spoofed GPS coordinates in auth2, so the board can
// stream any area from any IP with no VPN. Set from settings; re-auth to apply.
void radiko_set_area(int jp_area);
int  radiko_area_num(void);

// Prefecture name for a JP number (e.g. 13 -> "Tokyo" / "東京"). For the UI.
const char *radiko_area_name(int jp_area);
const char *radiko_area_name_jp(int jp_area);

// --- Program info ("now on air"), Phase 14 -----------------------------------

#include <stddef.h>

// Fetch + parse the area's current-programme XML into the cache. Blocking
// (HTTP + gunzip); call from a task. ESP_OK if the cache was updated.
esp_err_t radiko_program_refresh(void);

// Copy the cached current-programme title for a station id (e.g. "TBS") into
// out. Empty string if unknown/not yet fetched. Thread-safe.
void radiko_program_title(const char *station_id, char *out, size_t out_len);

// Start the background updater: refresh now, then every 5 minutes. on_update
// (may be NULL) is called after each successful refresh, from the task context.
void radiko_program_start(void (*on_update)(void));

#ifdef __cplusplus
}
#endif
