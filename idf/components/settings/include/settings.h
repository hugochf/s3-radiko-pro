/*
 * Persistent settings — a single versioned blob in NVS.
 *
 * Stored as one struct + a version field so load is atomic and self-validating:
 * wrong size or version, or a read error, falls back to defaults (corruption
 * recovery). Range-clamped on load. Call settings_init() once after NVS init.
 *
 * NVS encryption is deferred until flash encryption is enabled (Phase 25); the
 * nvs_keys partition is already reserved for it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t version;       // schema version (validated on load)
    int8_t   station;       // current station index
    uint8_t  volume;        // 0..100
    uint8_t  brightness;    // 0..3 backlight level
    uint8_t  led_mode;      // 0..6
    uint8_t  sleep_mins;    // 0/30/60/90
    uint32_t dim_ms;        // screen dim timeout
    uint32_t off_ms;        // screen off timeout
    uint8_t  rotation;      // 1 or 3
    bool     saver;         // screen-saver enabled
    uint8_t  area;          // Radiko auth area, JP number 1..47 (Phase 30)
    uint8_t  viz;           // player tile: 0=logo, 1=rainbow bars, 2=LED/car bars (Phase 32)
} settings_t;

// Long-press the player logo cycles these. uint8_t (not bool) so a third style
// costs no schema bump — same size, same offset, v3 blobs stay valid.
#define VIZ_MODE_LOGO    0
#define VIZ_MODE_RAINBOW 1
#define VIZ_MODE_LED     2
#define VIZ_MODE_COUNT   3

esp_err_t   settings_init(void);   // load, or defaults on miss/mismatch/corruption
settings_t *settings_get(void);    // mutable; edit fields then settings_save()
esp_err_t   settings_save(void);   // persist current struct

#ifdef __cplusplus
}
#endif
