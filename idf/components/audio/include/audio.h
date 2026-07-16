/*
 * Audio output — I2S (master) -> ES8311 codec -> FM8002E amp.
 *
 * I2S runs 48 kHz / 16-bit stereo, MCLK = 256*fs = 12.288 MHz on the MCLK pin.
 * The ES8311 is on the shared I2C bus. Phase 11: init + a test tone. The HLS
 * decode pipeline feeds audio_write() in Phase 12.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);                 // I2S + ES8311 + amp enable
void      audio_set_volume(int vol);        // 0..100 (codec DAC volume)

// Write interleaved 16-bit stereo PCM (back-pressures the caller until played).
esp_err_t audio_write(const void *pcm, size_t bytes, size_t *written);

// One-shot callback the first time real PCM enters the pipeline (boot splash
// dismissal). Runs in the decoder task; keep it quick.
void audio_on_first_audio(void (*cb)(void));

// Instant silence: mute + drop all buffered PCM. audio_resume() restarts output.
void audio_flush(void);
void audio_resume(void);

// Phase 11 verification: play a sine tone for `ms` milliseconds (blocking).
void      audio_test_tone(int freq_hz, int ms);

/*
 * PCM tap for the visualiser (Phase 32).
 *
 * Returns a copy of the most recent mono-mixed window that was handed to the
 * DAC — NOT what the decoder just produced. That distinction is the whole point:
 * the PCM ring is 30 s deep, so a decoder-side tap would drive the bars from
 * audio the user won't hear for half a minute. Sampled inside i2s_writer_task,
 * this leads the speaker by only the chunk (~21 ms) + DMA queue (~30 ms).
 *
 * Copies up to `max` samples into `dst`; returns the count (0 if no fresh
 * window, e.g. paused). Safe to call from any task — the writer never blocks on
 * this, so a slow caller costs a dropped frame of animation, never audio.
 */
size_t    audio_tap_read(int16_t *dst, size_t max);

// Bytes of PCM currently buffered ahead of the DAC. 4 B/frame @ 48 kHz, so
// bytes/192000 = seconds of slack. Near zero means the decoder is only just
// keeping up and any extra load will underrun the I2S DMA (an audible crack).
size_t    audio_ring_bytes(void);

#ifdef __cplusplus
}
#endif
