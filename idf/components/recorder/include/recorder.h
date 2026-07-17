/*
 * Recorder (Phase 29a). Captures the live Radiko stream to SD as a `.aac` file
 * with NO transcoding: the fetcher already downloaded ID3+ADTS segments, so we
 * strip the ID3 tag and append the raw ADTS. Concatenated ADTS is a universally
 * playable file, and it's what the on-device player reads back.
 *
 * All file I/O runs on a dedicated writer task fed by a queue. SD writes can
 * stall ~100 ms (measured); isolating them here means a stall costs a queued
 * segment, never the decoder on core 0. recorder_feed() is non-blocking and
 * drops a segment (logged, small gap in the recording) rather than ever block
 * the fetcher.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t recorder_init(void);                     // create the writer task
esp_err_t recorder_start(const char *station_id);  // open a file, begin capturing
void      recorder_stop(void);                     // finish + close the file

bool      recorder_active(void);
uint32_t  recorder_secs(void);   // elapsed recording time (UI)
uint32_t  recorder_kb(void);     // bytes written so far (UI)

// Called by the fetcher for each downloaded segment (ID3+ADTS) while active.
// Non-blocking: copies + enqueues, or drops on a full queue.
void      recorder_feed(const void *seg, int len);

// --- Recordings library (for the player UI) ---
#define REC_NAME_MAX 64
// List up to `max` recording basenames (e.g. "FMT_20260717_113605.aac") into
// `names`, newest first. Returns the count.
int       recorder_list(char (*names)[REC_NAME_MAX], int max);
// Full path for a basename, into `out` (e.g. "/sdcard/rec/FMT_...aac").
void      recorder_path(const char *name, char *out, int out_sz);
int       recorder_count(void);

#ifdef __cplusplus
}
#endif
