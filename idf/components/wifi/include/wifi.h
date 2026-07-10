/*
 * WiFi station state machine (esp_wifi + esp_event).
 *
 * Event-driven: connect on start, auto-reconnect with exponential backoff on
 * disconnect. Credentials come from a gitignored wifi_secrets.h for now;
 * provisioning (Phase 6) and NVS storage (Phase 7) replace that later.
 *
 * NVS must be initialised before wifi_start() (esp_wifi requires it).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_DISCONNECTED = 0,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
} wifi_state_t;

esp_err_t    wifi_start(void);      // init stack + begin connecting
wifi_state_t wifi_get_state(void);
int          wifi_get_rssi(void);   // dBm when connected, else 0
const char  *wifi_get_ip(void);     // "0.0.0.0" when not connected

#ifdef __cplusplus
}
#endif
