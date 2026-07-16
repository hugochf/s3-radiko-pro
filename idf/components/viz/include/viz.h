/*
 * Audio visualiser DSP (Phase 32) — spectrum of the audio actually being played.
 *
 * Reads the PCM tap in components/audio, which samples inside i2s_writer_task.
 * That tap point is the whole design: the PCM ring is 30 s deep, so a
 * decoder-side spectrum would lead the speaker by half a minute.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIZ_BANDS 16   // log-spaced, ~60 Hz .. 16 kHz

esp_err_t viz_init(void);

/*
 * Fill `out[0..VIZ_BANDS)` with band levels, 0..255. Already smoothed (fast
 * attack, slow decay) and safe to call straight from an LVGL timer.
 *
 * Returns false only when there is nothing left to draw — no fresh audio AND
 * every band has decayed to zero — so a caller can stop redrawing while paused.
 */
bool viz_read(uint8_t *out, int n);

#ifdef __cplusplus
}
#endif
