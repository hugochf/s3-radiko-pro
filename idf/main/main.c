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

#include "app_watchdog.h"
#include "audio.h"
#include "battery.h"
#include "crashlog.h"
#include "led.h"
#include "display.h"
#include "i2c_bus.h"
#include "radiko.h"
#include "settings.h"
#include "stream.h"
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

    // Retry auth: a transient TLS/connection hiccup must not permanently leave
    // the radio silent. Ramp the delay (3 s → 30 s cap) so a persistent failure
    // (captive portal, Radiko outage) doesn't hammer the server forever.
    radiko_auth_t auth;
    esp_err_t err;
    for (int attempt = 1; (err = radiko_authenticate(&auth)) != ESP_OK; attempt++) {
        int delay_ms = attempt * 3000;
        if (delay_ms > 30000) delay_ms = 30000;
        ESP_LOGW(TAG, "Radiko auth failed (attempt %d); retrying in %d s",
                 attempt, delay_ms / 1000);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_LOGI(TAG, "Radiko auth OK: area=%s", auth.area);
    ui_set_playing(true);                    // auto-play the saved station
    stream_play(ui_current_station_id());    // stream gets its TLS RAM first
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

    // Phase 18: task watchdog policy — 15 s, panic (-> coredump -> reboot) on
    // starvation. Critical tasks subscribe themselves as they start.
    app_watchdog_init();

    // Phase 19: if the last boot ended in a panic, decode and log the stored
    // coredump summary (also surfaced in Settings > System Info).
    crashlog_check();

    // Phase 1: ILI9341 panel. Phase 2: LVGL. Without a working display there is
    // no usable product, so these two still abort into the (visible) boot loop.
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());          // lv_init happens here, before touch indev

    // Phase 17: peripheral failures degrade instead of aborting. A radio with a
    // flaky touch controller or codec should still boot, show the UI, and (once
    // OTA lands) stay updatable — a boot loop is the worst possible field state.
    esp_err_t perr = i2c_bus_init();
    if (perr != ESP_OK)
        ESP_LOGE(TAG, "I2C bus init failed (%s) — touch and codec unavailable",
                 esp_err_to_name(perr));

    perr = touch_init();
    if (perr != ESP_OK)
        ESP_LOGE(TAG, "touch init failed (%s) — running without touch input",
                 esp_err_to_name(perr));

    // Phase 11: I2S + ES8311 audio output.
    perr = audio_init();
    if (perr != ESP_OK)
        ESP_LOGE(TAG, "audio init failed (%s) — radio will be silent",
                 esp_err_to_name(perr));
    else
        audio_set_volume(settings_get()->volume);   // apply persisted volume

    // WS2812 mood LED (Arduino parity; eye button on the player cycles modes).
    led_init();
    led_set_mode(settings_get()->led_mode);   // restore the persisted mode

    // Battery gauge (ADC on GPIO9); the UI polls it from the status-bar tick.
    battery_init();

    // Phase 12: player-control task (UI posts play/stop commands to it).
    stream_control_start();

    // Phase 14: "now on air" titles. Created HERE, at boot, so its task stack is
    // allocated before any TLS session runs — created later it can split the
    // largest free internal block below the ~16 KB contiguous mbedtls needs and
    // permanently break streaming. The task itself waits for Radiko auth.
    radiko_program_start(ui_program_updated);

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

    ui_apply_brightness();   // persisted level, not hard-coded full duty
    ESP_LOGI(TAG, "display + LVGL + touch + wifi + audio up");


    // Idle heartbeat so the watchdog stays happy and we can see it's alive.
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive (%" PRIu32 "), free internal %u KB (largest %u), PSRAM %u KB",
                 ++tick,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
