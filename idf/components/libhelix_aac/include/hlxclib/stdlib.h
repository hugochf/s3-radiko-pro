#pragma once
/*
 * Compatibility shim: the decoder uses helix_malloc/free (upstream's hlxclib).
 * Route the decoder's (one-time, ~tens of KB) state allocations to PSRAM so they
 * don't compete with TLS/WiFi for scarce internal RAM.
 */
#include <stdlib.h>
#include "esp_heap_caps.h"

#define helix_malloc(size)      heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define helix_free(ptr)         heap_caps_free(ptr)
#define helix_realloc(ptr, sz)  heap_caps_realloc((ptr), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
