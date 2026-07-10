/*
 * Radiko HLS stream client (hand-written): fetch the media playlist (m3u8),
 * fetch AAC segments, decode (libhelix), feed audio. Phase 12.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Increment A: fetch + parse the media playlist for a station, log segments.
void stream_probe(const char *station_id);

#ifdef __cplusplus
}
#endif
