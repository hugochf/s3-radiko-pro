/*
 * Radiko HLS stream client (hand-written): stream-info XML -> playlist ->
 * media playlist -> AAC segments -> libhelix decode -> audio. Phase 12.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the player-control task once at boot. Play/stop below post to it, so
// they are non-blocking and safe to call from LVGL touch handlers.
void stream_control_start(void);

void stream_play(const char *station_id);   // request: (re)start a station
void stream_play_file(const char *path);     // request: play a recorded .aac from SD
// Time-free (Phase 29b): stream a past programme (ft/to = "YYYYMMDDHHMMSS", JST).
void stream_play_timefree(const char *station, const char *ft, const char *to);
void stream_stop(void);                      // request: stop playback
void stream_pause(bool on);                  // pause/resume file playback (holds position)
void stream_seek_file(int permil);           // seek file playback to permil/1000 (0..1000)
uint32_t stream_file_total_secs(void);       // exact duration of the open recording (0 until known)
void stream_on_playback_end(void (*cb)(void)); // file playback reached EOF (from ctrl task)

#ifdef __cplusplus
}
#endif
