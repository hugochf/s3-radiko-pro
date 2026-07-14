/*
 * Station logos as embedded assets.
 *
 * The RGB565 pixel data is embedded from raw *.bin files at build time via
 * CMake EMBED_FILES (not a giant generated pixel array). logos_gen.c wraps each
 * embedded blob in an LVGL v9 image descriptor. Order matches components/stations.
 */
#pragma once

#include "lvgl.h"

// Full-size logos (fit 300x56) for the player screen.
extern const lv_image_dsc_t *const STATION_LOGOS[];

// Pre-scaled list-row logos (fit 138x38). Rows blit these 1:1 — LVGL's
// software image transform must never run at runtime (it starved the audio
// decoder in Phase 13 and wedged the LVGL task outright in Phase 17).
extern const lv_image_dsc_t *const STATION_LOGOS_SMALL[];
