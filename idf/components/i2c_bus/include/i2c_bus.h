/*
 * Shared I2C master bus (SDA=16, SCL=15) for the lcdwiki ES3C28P.
 *
 * Both the FT6336G touch controller (Phase 3) and the ES8311 audio codec
 * (Phase 11) hang off this one bus. i2c_master is thread-safe, so devices on
 * different tasks can share it. Call i2c_bus_init() once at startup.
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_bus_init(void);
i2c_master_bus_handle_t i2c_bus_handle(void);

#ifdef __cplusplus
}
#endif
