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

// Take/release the LVGL mutex (recursive). timeout_ms < 0 waits forever.
bool ui_lock(int timeout_ms);
void ui_unlock(void);

#ifdef __cplusplus
}
#endif
