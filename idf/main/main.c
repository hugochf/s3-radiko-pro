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

static const char *TAG = "boot";

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

    // Idle heartbeat so the watchdog stays happy and we can see it's alive.
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive (%" PRIu32 "), free heap %" PRIu32 " KB",
                 ++tick, esp_get_free_heap_size() / 1024);
    }
}
