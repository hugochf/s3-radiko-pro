/*
 * Radiko HLS stream client (hand-written): stream-info XML -> playlist ->
 * media playlist -> AAC segments -> libhelix decode -> audio. Phase 12.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void stream_play(const char *station_id);   // (re)start streaming a station
void stream_stop(void);                      // stop playback

#ifdef __cplusplus
}
#endif
