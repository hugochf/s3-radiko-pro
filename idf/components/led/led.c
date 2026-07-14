#include "led.h"

#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led";

#define PIN_RGB_LED 42   // WS2812B (same as Arduino build)

static led_strip_handle_t s_strip = NULL;
static volatile int       s_mode  = 0;

static const char *MODE_NAMES[LED_MODES] =
    {"Rainbow", "Ocean", "Sunset", "Candle", "Cycle", "Pulse", "LED OFF"};

// Straight port of the Arduino hsv_to_rgb (h 0..359, s/v 0..255).
static void hsv_to_rgb(int h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 60;
    uint8_t rem    = (h % 60) * 255 / 60;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

// 20 ms tick — same effect math as the Arduino rgb_update().
static void led_task(void *arg)
{
    uint16_t phase = 0;
    for (;;) {
        phase++;
        uint8_t r = 0, g = 0, b = 0;
        float breath;
        switch (s_mode) {
        case 0:   // Rainbow Breathing
            breath = (1.0f - cosf(phase * 2.0f * (float)M_PI / 256.0f)) / 2.0f;
            hsv_to_rgb((phase / 3) % 360, 255, (uint8_t)(breath * 255), &r, &g, &b);
            break;
        case 1:   // Ocean — blue/cyan breathing
            breath = (1.0f - cosf(phase * 2.0f * (float)M_PI / 200.0f)) / 2.0f;
            g = (uint8_t)(breath * 80); b = (uint8_t)(breath * 255);
            break;
        case 2:   // Sunset — warm red/orange breathing
            breath = (1.0f - cosf(phase * 2.0f * (float)M_PI / 200.0f)) / 2.0f;
            r = (uint8_t)(breath * 255); g = (uint8_t)(breath * 80);
            break;
        case 3:   // Candle — warm flicker
            r = 120 + rand() % 136; g = r / 3;
            break;
        case 4:   // Rainbow Cycle
            hsv_to_rgb((phase / 2) % 360, 255, 180, &r, &g, &b);
            break;
        case 5: { // Pulse — quick flash then fade
            uint16_t p = phase % 120;
            uint8_t v = p < 10 ? 255 : (p < 60 ? 255 - (p - 10) * 5 : 0);
            r = v; g = v; b = v;
            break;
        }
        default:  // OFF
            break;
        }
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_RGB_LED,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) return err;
    led_strip_clear(s_strip);
    // Priority 1 on core 0: cosmetic; must never compete with audio or UI.
    xTaskCreatePinnedToCore(led_task, "led", 2560, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "WS2812 up (GPIO%d)", PIN_RGB_LED);
    return ESP_OK;
}

const char *led_cycle_mode(void)
{
    s_mode = (s_mode + 1) % LED_MODES;
    return MODE_NAMES[s_mode];
}

void led_set_mode(int mode)
{
    if (mode >= 0 && mode < LED_MODES) s_mode = mode;
}

int led_mode(void) { return s_mode; }
