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

// Write interleaved 16-bit stereo PCM to I2S (blocking).
esp_err_t audio_write(const void *pcm, size_t bytes, size_t *written);

// Phase 11 verification: play a sine tone for `ms` milliseconds (blocking).
void      audio_test_tone(int freq_hz, int ms);

#ifdef __cplusplus
}
#endif
