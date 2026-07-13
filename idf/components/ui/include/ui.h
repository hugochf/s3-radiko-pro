/*
 * UI layer — LVGL v9 bound to the display driver.
 *
 * ui_init() sets up LVGL (buffers, flush, tick), starts the LVGL task, and builds
 * the current screen. LVGL is NOT thread-safe: any lv_* call from outside the
 * LVGL task must be wrapped in ui_lock()/ui_unlock().
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

// Show the on-device WiFi setup screen (scan + password entry). Safe to call
// from any task; it locks LVGL internally.
void ui_show_wifi_setup(void);

// Reflect playback state in the UI (play/pause icon). Thread-safe.
void ui_set_playing(bool playing);

// The saved/selected station id (for auto-play after auth). Never NULL.
const char *ui_current_station_id(void);

// Re-read the current station's "now on air" title into the player. Thread-safe;
// pass as the radiko_program_start() callback. Safe to call before UI is built.
void ui_program_updated(void);

// Take/release the LVGL mutex (recursive). timeout_ms < 0 waits forever.
bool ui_lock(int timeout_ms);
void ui_unlock(void);

#ifdef __cplusplus
}
#endif
