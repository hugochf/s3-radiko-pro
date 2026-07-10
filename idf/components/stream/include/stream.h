/*
 * Radiko HLS stream client (hand-written): stream-info XML -> playlist ->
 * media playlist -> AAC segments -> libhelix decode -> audio. Phase 12.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the player-control task once at boot. Play/stop below post to it, so
// they are non-blocking and safe to call from LVGL touch handlers.
void stream_control_start(void);

void stream_play(const char *station_id);   // request: (re)start a station
void stream_stop(void);                      // request: stop playback

#ifdef __cplusplus
}
#endif
