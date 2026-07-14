#include "app_watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watchdog";

// 15 s: must clear the fetcher's worst legitimate silence (a full 10 s
// esp_http_client timeout with no events, e.g. stopping a stream on a dead
// network) with margin. Detection latency is secondary — a wedge that lasted
// 14 s was going to last forever (Phase 17's did).
#define TWDT_TIMEOUT_MS 15000

esp_err_t app_watchdog_init(void)
{
    // The TWDT already runs (CONFIG_ESP_TASK_WDT_INIT) — reshape it. Panic on
    // starvation comes from CONFIG_ESP_TASK_WDT_PANIC, so a wedged subscriber
    // ends as panic -> coredump to flash -> reboot, not an endless log storm.
    esp_task_wdt_config_t cfg = {
        .timeout_ms    = TWDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),   // keep watching both idle tasks
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfigure failed (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TWDT: %d ms, panic on starvation, both idles watched",
             TWDT_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t app_watchdog_add(void)
{
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "watching '%s'", pcTaskGetName(NULL));
    } else {
        ESP_LOGW(TAG, "add '%s' failed (%s)", pcTaskGetName(NULL),
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_watchdog_remove(void)
{
    return esp_task_wdt_delete(NULL);
}

void app_watchdog_feed(void)
{
    esp_task_wdt_reset();
}
