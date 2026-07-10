/*
 * s3-radiko-pro — Phase 0: project skeleton.
 *
 * No peripherals yet. This just proves the toolchain, partition table, PSRAM,
 * and USB console all work end-to-end, and prints a boot banner we can watch
 * over `idf.py monitor`. Real drivers arrive in Phase 1 (display) onward.
 *
 * See ../../PLAN.md for the roadmap.
 */
#include <inttypes.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "display.h"
#include "i2c_bus.h"
#include "radiko.h"
#include "settings.h"
#include "timesync.h"
#include "touch.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "boot";

// Phase 10: once WiFi is up, authenticate with Radiko (auth1 + auth2). The token
// authorises the stream in Phase 12; the area drives program info in Phase 14.
static void radiko_auth_task(void *arg)
{
    while (wifi_get_state() != WIFI_CONNECTED) vTaskDelay(pdMS_TO_TICKS(200));

    radiko_auth_t auth;
    if (radiko_authenticate(&auth) == ESP_OK) {
        ESP_LOGI(TAG, "Radiko auth OK: area=%s", auth.area);
    } else {
        ESP_LOGE(TAG, "Radiko auth failed");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "s3-radiko-pro %s (built %s %s, IDF %s)",
             app->version, app->date, app->time, app->idf_ver);

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    // chip.revision is encoded as major*100 + minor (e.g. 002 -> v0.2).
    ESP_LOGI(TAG, "chip: %s rev v%d.%d, %d core(s), flash %" PRIu32 " MB",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
             chip.cores, flash_size / (1024 * 1024));

    // PSRAM is mandatory for the audio pipeline in Tier C — fail loud if absent.
    size_t psram = esp_psram_get_size();
    if (psram == 0) {
        ESP_LOGE(TAG, "PSRAM not detected! Check board / sdkconfig (SPIRAM_MODE_OCT).");
    } else {
        ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(psram / 1024));
    }

    ESP_LOGI(TAG, "free heap: %" PRIu32 " KB (internal), %u KB (PSRAM)",
             esp_get_free_heap_size() / 1024,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    // NVS is required by esp_wifi; init it early (Phase 7 expands its use).
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Phase 7: load persistent settings (before UI so it starts from saved state).
    ESP_ERROR_CHECK(settings_init());

    // Phase 1: ILI9341 panel. Phase 2: LVGL. Phase 3: shared I2C bus + FT6336 touch.
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());          // lv_init happens here, before touch indev
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(touch_init());

    // Phase 11: I2S + ES8311 audio output.
    ESP_ERROR_CHECK(audio_init());

    // Phase 5: event-driven WiFi station. Phase 6: on-device setup if no creds.
    ESP_ERROR_CHECK(wifi_start());
    if (!wifi_has_creds()) {
        ESP_LOGI(TAG, "no WiFi creds — showing setup screen");
        ui_show_wifi_setup();
    }

    // Phase 8: SNTP (JST) — syncs in the background once WiFi is up.
    timesync_start();

    // Phase 10: authenticate with Radiko once WiFi is up.
    xTaskCreate(radiko_auth_task, "radiko_auth", 8192, NULL, 5, NULL);

    display_backlight_set(255);
    ESP_LOGI(TAG, "display + LVGL + touch + wifi + audio up");

    // Phase 11 verification: audible boot tone.
    audio_test_tone(1000, 1200);

    // Idle heartbeat so the watchdog stays happy and we can see it's alive.
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive (%" PRIu32 "), free heap %" PRIu32 " KB",
                 ++tick, esp_get_free_heap_size() / 1024);
    }
}
