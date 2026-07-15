/*
 * Battery gauge — Li-ion cell behind a ×2 divider on GPIO9 (ADC1 ch8) on the
 * lcdwiki ES3C28P. Ported from the Arduino build: 8-sample average + EMA
 * smoothing, approximate discharge-curve percentage, and a voltage-trend
 * charging heuristic (the TP4054's CHRG pin only drives an LED, so charge
 * state can't be read directly).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t battery_init(void);

// Smoothed cell voltage in mV (samples the ADC and updates the average).
// Returns -1 if the ADC is unavailable.
int battery_mv(void);

// 0..100 from the discharge curve, or -1 if unavailable.
int battery_pct(void);

// True only when the cell voltage is CLEARLY rising (charging a not-full
// battery). This board has no charge-status pin, so a full battery topping off
// on the charger reads as "not charging" — see battery.c. Call at a steady
// ~2 s cadence (the status-bar refresh); the trend window assumes it.
bool battery_charging(void);

#ifdef __cplusplus
}
#endif
