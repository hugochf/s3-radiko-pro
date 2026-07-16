#include "viz.h"

#include <math.h>
#include <string.h>

#include "audio.h"
#include "esp_dsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "viz";

#define FFT_N       512        // 10.7 ms @ 48 kHz -> 93.75 Hz bins
#define SAMPLE_RATE 48000
#define F_LO        60.0f      // below this is mains hum and cabinet noise
#define F_HI        16000.0f   // above this an HE-AAC stream has nothing anyway

// A full-scale sine lands at 0 dB after this normalisation: hann's coherent gain
// is 0.5, so its peak bin is N*A/4. Radio content sits well below that.
#define NORM        ((FFT_N / 4.0f) * (FFT_N / 4.0f))
#define DB_FLOOR    -60.0f
#define DB_CEIL     -10.0f
#define DECAY       0.80f      // per frame; see the attack/decay note below

static float   *s_cf;      // FFT_N complex pairs: re,im,re,im...
static float   *s_wind;    // hann window
static int16_t *s_pcm;     // one window straight from the audio tap
static uint16_t s_lo[VIZ_BANDS], s_hi[VIZ_BANDS];
static uint8_t  s_level[VIZ_BANDS];
static bool     s_ready;

esp_err_t viz_init(void)
{
    // On the ESP32-S3, esp-dsp uses a PRECOMPUTED STATIC twiddle table when
    // table_size <= 1024 — zero heap. Ask for more and it memalign()s
    // sizeof(float)*table_size from internal RAM: 4096 would be 16 KB, which
    // this radio does not have to give (~30 KB free, largest block ~10 KB).
    // 512 is also plenty: 93.75 Hz bins across 16 log bands.
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_N);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fft init: %s", esp_err_to_name(err));
        return err;
    }

    // PSRAM, 16-byte aligned for the S3's SIMD FFT path. Internal RAM is the
    // scarce resource (Phase 31), and 25 FFTs/s over a 4 KB working set lives in
    // cache — the PSRAM penalty is unmeasurable here.
    s_cf   = heap_caps_aligned_alloc(16, sizeof(float) * FFT_N * 2, MALLOC_CAP_SPIRAM);
    s_wind = heap_caps_aligned_alloc(16, sizeof(float) * FFT_N, MALLOC_CAP_SPIRAM);
    s_pcm  = heap_caps_malloc(sizeof(int16_t) * FFT_N, MALLOC_CAP_SPIRAM);
    if (!s_cf || !s_wind || !s_pcm) {
        ESP_LOGE(TAG, "viz buffers: no PSRAM");
        return ESP_ERR_NO_MEM;
    }
    dsps_wind_hann_f32(s_wind, FFT_N);

    // Log-spaced bands. Linear bins would hand bass a single bar and cymbals a
    // hundred, which looks like noise rather than music.
    for (int b = 0; b < VIZ_BANDS; b++) {
        float f0 = F_LO * powf(F_HI / F_LO, (float)b / VIZ_BANDS);
        float f1 = F_LO * powf(F_HI / F_LO, (float)(b + 1) / VIZ_BANDS);
        int lo = (int)(f0 * FFT_N / SAMPLE_RATE);
        int hi = (int)(f1 * FFT_N / SAMPLE_RATE);
        if (lo < 1) lo = 1;                    // skip DC
        if (hi <= lo) hi = lo + 1;             // narrow low bands still get a bin
        if (hi > FFT_N / 2) hi = FFT_N / 2;    // Nyquist
        s_lo[b] = lo;
        s_hi[b] = hi;
    }

    s_ready = true;
    ESP_LOGI(TAG, "visualiser: %d-pt FFT, %d bands, %.0f-%.0f Hz",
             FFT_N, VIZ_BANDS, F_LO, F_HI);
    return ESP_OK;
}

// Fast attack, slow decay. Without it the bars flicker between frames and read
// as static; with it they punch up and fall away like a real analyser.
static uint8_t smooth(uint8_t prev, uint8_t now)
{
    return now >= prev ? now : (uint8_t)(prev * DECAY);
}

bool viz_read(uint8_t *out, int n)
{
    if (!s_ready || !out || n < VIZ_BANDS) return false;

    size_t got = audio_tap_read(s_pcm, FFT_N);
    if (got < FFT_N) {
        // Paused, silent, or the writer was mid-chunk. Decay rather than freeze:
        // a stuck spectrum looks broken, a falling one looks like silence.
        bool alive = false;
        for (int b = 0; b < VIZ_BANDS; b++) {
            s_level[b] = smooth(s_level[b], 0);
            out[b] = s_level[b];
            if (s_level[b]) alive = true;
        }
        return alive;
    }

    for (int i = 0; i < FFT_N; i++) {
        s_cf[i * 2]     = (s_pcm[i] / 32768.0f) * s_wind[i];
        s_cf[i * 2 + 1] = 0.0f;
    }
    dsps_fft2r_fc32(s_cf, FFT_N);
    dsps_bit_rev_fc32(s_cf, FFT_N);

    for (int b = 0; b < VIZ_BANDS; b++) {
        float peak = 0.0f;
        for (int k = s_lo[b]; k < s_hi[b]; k++) {
            float re = s_cf[k * 2], im = s_cf[k * 2 + 1];
            float p = re * re + im * im;
            if (p > peak) peak = p;   // peak, not sum: wide high bands would
        }                             // otherwise tower over narrow bass ones
        float db = 10.0f * log10f(peak / NORM + 1e-12f);
        float t  = (db - DB_FLOOR) / (DB_CEIL - DB_FLOOR);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        s_level[b] = smooth(s_level[b], (uint8_t)(t * 255.0f));
        out[b] = s_level[b];
    }
    return true;
}
