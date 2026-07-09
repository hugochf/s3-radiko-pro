/*
 * Display driver — ILI9341 320x240 over SPI via esp_lcd, plus LEDC backlight.
 *
 * Pin map and 40 MHz SPI clock are ported from the Arduino TFT_eSPI User_Setup.h
 * for the lcdwiki ES3C28P board. RST is tied to the board reset line (no GPIO),
 * so the panel is brought up with a software reset.
 *
 * Phase 1 exposes just enough to prove the panel lights up and addresses
 * correctly. LVGL binds to display_panel() in Phase 2.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Landscape orientation (rotation 1 on the Arduino build).
#define DISPLAY_H_RES 320
#define DISPLAY_V_RES 240

// Bring up SPI bus, ILI9341 panel, and backlight PWM. Backlight starts off;
// call display_backlight_set() after drawing to avoid showing garbage RAM.
esp_err_t display_init(void);

// The esp_lcd panel handle, for LVGL flush (Phase 2) or direct draws.
esp_lcd_panel_handle_t display_panel(void);

// Backlight brightness, 0 (off) .. 255 (full), via LEDC PWM on GPIO45.
void display_backlight_set(uint8_t duty);

// Phase 1 sanity check: draw vertical color bars across the whole panel.
esp_err_t display_test_pattern(void);

#ifdef __cplusplus
}
#endif
