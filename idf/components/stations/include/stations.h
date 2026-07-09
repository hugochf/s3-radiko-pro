/*
 * Radiko station table (Kanto area). Ported from the Arduino build, minus the
 * logo images — those become embedded assets in Phase 13; until then the UI
 * shows a coloured tile with the abbreviation.
 */
#pragma once

#include <stdint.h>

// Compile-time count (for array sizing). STATION_COUNT below is the runtime value.
#define NUM_STATIONS 15

typedef struct {
    const char *id;     // Radiko station id
    const char *name;   // UTF-8 Japanese display name
    const char *abbr;   // short label (logo placeholder)
    uint32_t    color;  // tile background, 0xRRGGBB
} station_t;

extern const station_t STATIONS[];
extern const int        STATION_COUNT;
