/*
 * Station logos as embedded assets.
 *
 * The RGB565 pixel data is embedded from raw *.bin files at build time via
 * CMake EMBED_FILES (not a giant generated pixel array). logos_gen.c wraps each
 * embedded blob in an LVGL v9 image descriptor. Order matches components/stations.
 */
#pragma once

#include "lvgl.h"

extern const lv_image_dsc_t *const STATION_LOGOS[];
