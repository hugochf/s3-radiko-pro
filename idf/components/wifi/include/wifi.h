/*
 * WiFi station state machine (esp_wifi + esp_event) with on-device setup.
 *
 * Credentials persist in NVS. On first boot they're seeded from a gitignored
 * wifi_secrets.h if present (dev convenience); otherwise the UI's WiFi setup
 * screen scans, takes a password, and calls wifi_connect_creds().
 *
 * NVS must be initialised before wifi_start().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_DISCONNECTED = 0,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
} wifi_state_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;   // needs a password
} wifi_ap_info_t;

esp_err_t    wifi_start(void);       // init stack; connect if creds exist
wifi_state_t wifi_get_state(void);
int          wifi_get_rssi(void);    // dBm when connected, else 0
const char  *wifi_get_ip(void);
bool         wifi_has_creds(void);   // NVS holds a saved network

// Blocking scan (~1.5 s) — call OFF the LVGL thread. Fills up to `max`, returns count.
int          wifi_scan(wifi_ap_info_t *out, int max);

// Persist creds to NVS and (re)connect to them.
esp_err_t    wifi_connect_creds(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
