#include "ota.h"

#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_parse.h"

static const char *TAG = "ota";

#define RELEASES_URL \
    "https://api.github.com/repos/hugochf/esp32-radiko-player-pro/releases/latest"
#define JSON_CAP (16 * 1024)   // /releases/latest body (PSRAM, transient)

// ---- release check ----
static struct { char *buf; size_t cap; size_t len; } s_rx;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_rx.buf) {
        size_t space = s_rx.cap - 1 - s_rx.len;
        size_t n = ((size_t)e->data_len < space) ? (size_t)e->data_len : space;
        memcpy(s_rx.buf + s_rx.len, e->data, n);
        s_rx.len += n;
        s_rx.buf[s_rx.len] = '\0';
    }
    return ESP_OK;
}

esp_err_t ota_check(char *tag, size_t tag_cap, char *url, size_t url_cap,
                    bool *newer)
{
    *newer = false;
    char *json = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    if (!json) return ESP_ERR_NO_MEM;
    s_rx.buf = json; s_rx.cap = JSON_CAP; s_rx.len = 0;
    json[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = RELEASES_URL,
        .event_handler     = on_http,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .user_agent        = "esp32-radiko-player-pro-ota",   // GitHub API requires a UA
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(json); return ESP_FAIL; }
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    s_rx.buf = NULL;

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "release check failed (%s, HTTP %d)",
                 esp_err_to_name(err), status);
        free(json);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    bool ok = ota_parse_release(json, tag, tag_cap, url, url_cap);
    free(json);
    if (!ok) {
        ESP_LOGW(TAG, "no parsable release/.bin asset in response");
        return ESP_ERR_NOT_FOUND;
    }

    const char *running = esp_app_get_description()->version;
    *newer = ota_version_cmp(tag, running) > 0;
    ESP_LOGI(TAG, "latest release %s vs running %s -> %s",
             tag, running, *newer ? "UPDATE AVAILABLE" : "up to date");
    return ESP_OK;
}

// ---- download + switch slots ----
esp_err_t ota_update(const char *url, ota_progress_cb_t cb)
{
    ESP_LOGI(TAG, "updating from %s", url);
    if (cb) cb("Connecting...", -1);

    esp_http_client_config_t http = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        // GitHub redirects assets to objects.githubusercontent.com with a
        // signed URL well over 1 KB. The redirected REQUEST has to fit in the
        // TX buffer (default 1 KB -> "HTTP_CLIENT: Out of buffer", found via
        // the elog on the first field test); headers land in the RX buffer.
        .buffer_size       = 8192,
        .buffer_size_tx    = 4096,
        .user_agent        = "esp32-radiko-player-pro-ota",
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed (%s)", esp_err_to_name(err));
        return err;
    }

    int total = esp_https_ota_get_image_size(h);
    int last_pct = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (cb && total > 0) {
            int pct = esp_https_ota_get_image_len_read(h) * 100 / total;
            if (pct != last_pct) {   // don't spam the UI every 4 KB chunk
                last_pct = pct;
                cb("Downloading update", pct);
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "ota perform failed (%s)", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = esp_https_ota_finish(h);   // validates image + sets boot partition
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota finish failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "update written — rebooting into the new image");
    if (cb) cb("Rebooting...", 100);
    vTaskDelay(pdMS_TO_TICKS(800));   // let the UI paint the final status
    esp_restart();
    return ESP_OK;   // not reached
}
