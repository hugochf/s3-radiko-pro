/*
 * Task-watchdog policy for the whole app (Phase 18).
 *
 * The default TWDT only watches the idle tasks and only PRINTS when starved —
 * the Phase 17 LVGL wedge sat frozen for minutes doing exactly that. Policy
 * here: the tasks whose silent death bricks the radio (LVGL, I2S writer, HLS
 * fetcher/decoder) subscribe explicitly, and a starved watchdog PANICS, which
 * saves a coredump to flash (Phase 19 reads it back) and reboots into a
 * working radio. A 20 s frozen-then-recovered radio beats a frozen-forever one.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reconfigure the boot-time TWDT (timeout, panic-on-starve, watch both idles).
esp_err_t app_watchdog_init(void);

// Subscribe/unsubscribe the CALLING task. Tasks that exit (per-play stream
// tasks) MUST call remove before vTaskDelete or the dead handle stays watched.
esp_err_t app_watchdog_add(void);
esp_err_t app_watchdog_remove(void);

// Feed for the calling task. Every subscribed task must call this more often
// than the timeout — including across its worst-case legitimate blocking op
// (a full 10 s HTTP timeout in the fetcher sets the floor for the timeout).
void app_watchdog_feed(void);

#ifdef __cplusplus
}
#endif
