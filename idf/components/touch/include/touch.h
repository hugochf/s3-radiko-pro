/*
 * FT6336G capacitive touch -> LVGL pointer input device.
 *
 * Shares the I2C bus (i2c_bus). Coordinates are mapped to match the display's
 * upside-down landscape orientation. Requires LVGL to be initialised first
 * (ui_init), since touch_init() registers an lv_indev.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t touch_init(void);

#ifdef __cplusplus
}
#endif
