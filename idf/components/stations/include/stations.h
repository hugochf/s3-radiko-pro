/*
 * Nationwide Radiko station database + area-filtered active list (Phase 30).
 *
 * STATIONS_ALL[] (generated in stations_gen.c) holds every station across all
 * 47 prefectures, each tagged with an area bitmask and byte offsets/dims into
 * the packed logo blob. The blob lives in the `logos` flash data partition and
 * is mmap'd at boot, so logo pixels are read zero-copy straight from flash
 * (like the old EMBED_FILES logos, but for all of Japan without bloating the
 * app slot).
 *
 * The UI works against the ACTIVE list — the subset for the currently selected
 * area (stations_set_area) — via the accessors below.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

// Busiest single area (JP9) has 17 stations; 20 gives headroom.
#define MAX_STATIONS 20

typedef struct {
    const char *id;         // Radiko station id (e.g. "TBS")
    const char *name;       // UTF-8 display name
    uint64_t    area_mask;  // bit (n-1) set => station broadcasts in JPn
    uint32_t    big_off;    // player logo (fit 300x56) offset in logos.bin
    uint16_t    big_w, big_h;
    uint32_t    sm_off;     // list logo (fit 138x38) offset in logos.bin
    uint16_t    sm_w, sm_h;
} station_t;

extern const station_t STATIONS_ALL[];
extern const int        STATIONS_ALL_COUNT;

// mmap the `logos` partition. Call once at boot before stations_set_area.
esp_err_t stations_init(void);

// Rebuild the active list = stations broadcast in JP `area` (1..47).
void stations_set_area(int area);

// Active (area-filtered) list — what the player and list screens iterate.
int         stations_count(void);
const char *station_id(int i);
const char *station_name(int i);
const lv_image_dsc_t *station_logo_big(int i);    // player, blit 1:1
const lv_image_dsc_t *station_logo_small(int i);  // list row, blit 1:1
