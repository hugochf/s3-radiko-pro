#include "audio.h"

#include <math.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_check.h"
#include "esp_log.h"
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
    return i2s_channel_write(s_tx, pcm, bytes, written, portMAX_DELAY);
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
        size_t bw;
        i2s_channel_write(s_tx, buf, sizeof(buf), &bw, portMAX_DELAY);
    }
}
