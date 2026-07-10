#include "wifi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

// Seed credentials: local, gitignored (see wifi_secrets.h.example). Used only to
// seed NVS on first boot; the on-device setup screen replaces them thereafter.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif

static const char *TAG = "wifi";

#define NVS_NS "wifi"
#define RETRY_BASE_MS   500
#define RETRY_MAX_MS    30000
#define RETRY_MAX_SHIFT 6

static volatile wifi_state_t s_state = WIFI_DISCONNECTED;
static bool                  s_have_creds = false;
static int                   s_retry = 0;
static char                  s_ip[16] = "0.0.0.0";
static esp_timer_handle_t    s_retry_timer = NULL;

// ---- NVS credential persistence ----
static bool creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK);
    nvs_close(h);
    return ok && strlen(ssid) > 0;
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

bool wifi_has_creds(void)
{
    char ssid[33] = {0}, pass[65] = {0};
    return creds_load(ssid, sizeof(ssid), pass, sizeof(pass));
}

// ---- Reconnect backoff ----
static void retry_timer_cb(void *arg) { esp_wifi_connect(); }

static void schedule_reconnect(void)
{
    int shift = s_retry > RETRY_MAX_SHIFT ? RETRY_MAX_SHIFT : s_retry;
    int delay_ms = RETRY_BASE_MS << shift;
    if (delay_ms > RETRY_MAX_MS) delay_ms = RETRY_MAX_MS;
    s_retry++;
    ESP_LOGW(TAG, "retry #%d in %d ms", s_retry, delay_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        if (s_have_creds) {
            s_state = WIFI_CONNECTING;
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        strcpy(s_ip, "0.0.0.0");
        if (s_have_creds) {
            ESP_LOGW(TAG, "disconnect reason=%d (15=bad pw, 201=AP not found)", e->reason);
            s_state = WIFI_CONNECTING;
            schedule_reconnect();
        } else {
            s_state = WIFI_DISCONNECTED;  // awaiting setup — don't spin
        }
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

static void apply_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = strlen(pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
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

    const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    // Seed NVS from wifi_secrets.h on first boot (dev convenience).
    char ssid[33] = {0}, pass[65] = {0};
    if (!creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) && strlen(WIFI_SSID) > 0) {
        ESP_LOGI(TAG, "seeding NVS creds from wifi_secrets.h");
        creds_save(WIFI_SSID, WIFI_PASSWORD);
        creds_load(ssid, sizeof(ssid), pass, sizeof(pass));
    }
    s_have_creds = strlen(ssid) > 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_have_creds) apply_config(ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_start());  // STA_START handler connects if creds

    if (s_have_creds) ESP_LOGI(TAG, "starting, ssid='%s'", ssid);
    else              ESP_LOGW(TAG, "no creds - awaiting on-device setup");
    return ESP_OK;
}

esp_err_t wifi_connect_creds(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "new creds for '%s'", ssid);
    bool was_connected = (s_state == WIFI_CONNECTED);
    creds_save(ssid, pass);
    apply_config(ssid, pass);
    s_have_creds = true;
    s_retry = 0;
    s_state = WIFI_CONNECTING;
    // If already associated, disconnect so the reconnect handler re-associates
    // with the new config; otherwise connect directly. (Calling esp_wifi_connect
    // while connected is a no-op and would leave us stuck "connecting".)
    if (was_connected) {
        esp_wifi_disconnect();
    } else {
        esp_wifi_connect();
    }
    return ESP_OK;
}

int wifi_scan(wifi_ap_info_t *out, int max)
{
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) return 0;

    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_clear_ap_list(); return 0; }
    esp_wifi_scan_get_ap_records(&found, recs);

    int n = 0;
    for (int i = 0; i < found && n < max; i++) {
        if (recs[i].ssid[0] == '\0') continue;  // skip hidden
        strncpy(out[n].ssid, (char *)recs[i].ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        out[n].rssi   = recs[i].rssi;
        out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    free(recs);
    ESP_LOGI(TAG, "scan: %d networks", n);
    return n;
}

wifi_state_t wifi_get_state(void) { return s_state; }

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

const char *wifi_get_ip(void) { return s_ip; }
