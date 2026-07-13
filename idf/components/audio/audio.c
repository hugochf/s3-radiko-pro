#include "audio.h"

#include <math.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "audio";

// Pin map (lcdwiki ES3C28P) — from the Arduino build.
#define PIN_AMP_EN   1    // FM8002E enable, active-LOW
#define PIN_I2S_MCK  4
#define PIN_I2S_BCK  5
#define PIN_I2S_WS   7
#define PIN_I2S_DOUT 8

#define SAMPLE_RATE  48000
#define MCLK_FREQ    (256 * SAMPLE_RATE)   // 12.288 MHz
#define ES8311_ADDR  0x18

static i2s_chan_handle_t        s_tx  = NULL;
static i2c_master_dev_handle_t  s_es  = NULL;

// PCM ring buffer between the decoder (producer, bursty) and I2S (consumer,
// real-time) — absorbs network/decode jitter so segments play gaplessly.
// ~15 s of 16-bit stereo in PSRAM. Must exceed one HLS segment (5 s) with plenty
// of headroom so the decoder can run ahead of the ~2.4 s-per-segment fetch and
// front-load the initial playlist window (otherwise audio_write blocks mid-
// segment, pinning throughput to real time and starving on fetch latency).
#define PCM_BUF_BYTES (SAMPLE_RATE * 4 * 30)   // 30 s: rides out session-expiry re-resolve
static StreamBufferHandle_t s_pcm = NULL;
static volatile bool        s_active    = true;   // false -> audio_write drops
static volatile bool        s_flush_req = false;  // ask the writer to discard buffered PCM

// Only the writer task touches the buffer's receive side, so flushing here (vs
// xStreamBufferReset) can't race a blocked reader.
static void i2s_writer_task(void *arg)
{
    static uint8_t chunk[4096];
    while (true) {
        if (s_flush_req) {
            while (xStreamBufferReceive(s_pcm, chunk, sizeof(chunk), 0) > 0) { }  // discard
            s_flush_req = false;
        }
        size_t n = xStreamBufferReceive(s_pcm, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (n) {
            size_t bw;
            i2s_channel_write(s_tx, chunk, n, &bw, portMAX_DELAY);
        }
    }
}

esp_err_t audio_init(void)
{
    // Speaker amp on (active low).
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << PIN_AMP_EN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&amp));
    gpio_set_level(PIN_AMP_EN, 0);

    // ES8311 on the shared I2C bus.
    i2c_device_config_t escfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_handle(), &escfg, &s_es),
                        TAG, "es8311 i2c");

    // I2S standard TX, master, 48 kHz / 16-bit stereo, MCLK on the MCLK pin.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // on underflow send silence, not the last buffer on loop
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s chan");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),   // mclk = 256*fs
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCK,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");  // MCLK starts here

    // Codec init after MCLK is running so its PLL locks.
    es8311_clock_config_t clk = {
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = MCLK_FREQ,
        .sample_frequency   = SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(es8311_init(s_es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
                        TAG, "es8311 init");
    es8311_voice_volume_set(s_es, 70, NULL);
    es8311_voice_mute(s_es, false);

    // PCM ring buffer (PSRAM storage) + the sole I2S writer task.
    static StaticStreamBuffer_t sb_struct;
    uint8_t *sb_storage = heap_caps_malloc(PCM_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(sb_storage, ESP_ERR_NO_MEM, TAG, "pcm buf");
    s_pcm = xStreamBufferCreateStatic(PCM_BUF_BYTES, 1, sb_storage, &sb_struct);
    xTaskCreatePinnedToCore(i2s_writer_task, "i2s_wr", 4096, NULL, 6, NULL, 1);

    ESP_LOGI(TAG, "audio up: I2S %d Hz, ES8311 ready", SAMPLE_RATE);
    return ESP_OK;
}

void audio_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (s_es) es8311_voice_volume_set(s_es, vol, NULL);
}

esp_err_t audio_write(const void *pcm, size_t bytes, size_t *written)
{
    // Back-pressure when full (paces the decoder), but bounded so a flush can
    // unblock us quickly: while flushing/stopped (!s_active), drop the data.
    const uint8_t *p = pcm;
    size_t sent = 0;
    while (sent < bytes && s_active) {
        sent += xStreamBufferSend(s_pcm, p + sent, bytes - sent, pdMS_TO_TICKS(100));
    }
    if (written) *written = sent;
    return ESP_OK;
}

// Instant silence: mute, stop accepting PCM, and drop everything buffered.
void audio_flush(void)
{
    if (s_es) es8311_voice_mute(s_es, true);
    s_active    = false;                 // audio_write returns promptly, dropping
    s_flush_req = true;                  // writer discards the ring buffer
    vTaskDelay(pdMS_TO_TICKS(70));       // let both take effect
}

// Resume output with an empty buffer (fresh, live audio).
void audio_resume(void)
{
    s_flush_req = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    s_active = true;
    if (s_es) es8311_voice_mute(s_es, false);
}

void audio_test_tone(int freq_hz, int ms)
{
    const int N = 240;                 // stereo frames per chunk
    static int16_t buf[240 * 2];
    int total = SAMPLE_RATE * ms / 1000;
    double phase = 0, step = 2.0 * M_PI * freq_hz / SAMPLE_RATE;

    ESP_LOGI(TAG, "test tone %d Hz for %d ms", freq_hz, ms);
    for (int done = 0; done < total; done += N) {
        for (int i = 0; i < N; i++) {
            int16_t s = (int16_t)(sin(phase) * 8000);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
            phase += step;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
        }
        audio_write(buf, sizeof(buf), NULL);   // through the ring buffer
    }
}
