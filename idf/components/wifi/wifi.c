#include "wifi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

// Credentials: local, gitignored (see wifi_secrets.h.example). Falls back to
// empty so CI still builds; provisioning/NVS supersede this in Phases 6-7.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif

static const char *TAG = "wifi";

// Exponential backoff, capped. Reset to 0 once we get an IP.
#define RETRY_BASE_MS 500
#define RETRY_MAX_MS  30000
#define RETRY_MAX_SHIFT 6

static volatile wifi_state_t s_state = WIFI_DISCONNECTED;
static int                   s_retry = 0;
static char                  s_ip[16] = "0.0.0.0";
static esp_timer_handle_t    s_retry_timer = NULL;

static void retry_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "reconnecting...");
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    int shift = s_retry > RETRY_MAX_SHIFT ? RETRY_MAX_SHIFT : s_retry;
    int delay_ms = RETRY_BASE_MS << shift;
    if (delay_ms > RETRY_MAX_MS) delay_ms = RETRY_MAX_MS;
    s_retry++;
    ESP_LOGW(TAG, "disconnected; retry #%d in %d ms", s_retry, delay_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        s_state = WIFI_CONNECTING;
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnect reason=%d (15=bad password, 201=AP not found)",
                 e->reason);
        s_state = WIFI_CONNECTING;   // we keep trying
        strcpy(s_ip, "0.0.0.0");
        schedule_reconnect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
    s_retry = 0;
    s_state = WIFI_CONNECTED;
    ESP_LOGI(TAG, "connected, ip=%s", s_ip);
}

esp_err_t wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode =
        strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no credentials (wifi_secrets.h missing) — will not connect");
    } else {
        ESP_LOGI(TAG, "starting, ssid='%s'", WIFI_SSID);
    }
    return ESP_OK;
}

wifi_state_t wifi_get_state(void) { return s_state; }

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

const char *wifi_get_ip(void) { return s_ip; }
