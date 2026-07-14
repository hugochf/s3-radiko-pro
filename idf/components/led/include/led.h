/*
 * WS2812B mood LED (GPIO42), ported from the Arduino build: 7 modes cycled by
 * the player-screen eye button. Effects run on a low-priority 30 ms tick task
 * (the Arduino's exact cadence); the phase derives from the wall clock so
 * scheduling pressure can never slow the animations down.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_MODES 7   // 0=rainbow breath,1=ocean,2=sunset,3=candle,4=cycle,5=pulse,6=off

esp_err_t   led_init(void);
// Advance to the next mode; returns its display name (e.g. "Ocean", "OFF").
const char *led_cycle_mode(void);
int         led_mode(void);          // current mode index (6 = off)
void        led_set_mode(int mode);  // jump to a mode (restore from settings)

#ifdef __cplusplus
}
#endif
