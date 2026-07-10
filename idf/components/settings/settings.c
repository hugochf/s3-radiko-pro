#include "settings.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NS  "settings"
#define NVS_KEY "blob"
#define SETTINGS_VERSION 1

static settings_t s;

static void set_defaults(void)
{
    memset(&s, 0, sizeof(s));
    s.version    = SETTINGS_VERSION;
    s.station    = 0;
    s.volume     = 20;
    s.brightness = 3;
    s.led_mode   = 0;
    s.sleep_mins = 0;
    s.dim_ms     = 300000;   // 5 min
    s.off_ms     = 600000;   // 10 min
    s.rotation   = 3;        // upside-down mount
    s.saver      = false;
}

// Defence in depth: even a version-matched blob gets range-checked.
static void clamp(void)
{
    if (s.volume > 100)   s.volume = 100;
    if (s.brightness > 3) s.brightness = 3;
    if (s.led_mode > 6)   s.led_mode = 0;
    if (s.sleep_mins > 90) s.sleep_mins = 0;
    if (s.rotation != 1 && s.rotation != 3) s.rotation = 3;
    if (s.station < 0)    s.station = 0;   // upper bound checked against station list in UI
}

esp_err_t settings_init(void)
{
    bool loaded = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        settings_t tmp;
        size_t len = sizeof(tmp);
        if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK &&
            len == sizeof(settings_t) && tmp.version == SETTINGS_VERSION) {
            s = tmp;
            loaded = true;
        }
        nvs_close(h);
    }

    if (!loaded) {
        ESP_LOGW(TAG, "no valid settings (missing/old/corrupt) — writing defaults");
        set_defaults();
        settings_save();
    } else {
        clamp();
        ESP_LOGI(TAG, "loaded settings v%" PRIu32 " (vol=%d stn=%d)",
                 s.version, s.volume, s.station);
    }
    return ESP_OK;
}

settings_t *settings_get(void) { return &s; }

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    s.version = SETTINGS_VERSION;
    err = nvs_set_blob(h, NVS_KEY, &s, sizeof(s));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
