#pragma once
/*
 * Compatibility shim: the decoder uses helix_malloc/free (upstream's hlxclib).
 * Keep the decoder state in INTERNAL RAM — HE-AAC decode hits it constantly, and
 * PSRAM's latency makes decode too slow to stay ahead of real time (the decoder
 * then spins CPU-bound and trips the task watchdog). TLS headroom is freed
 * elsewhere (LVGL buffers in PSRAM + smaller mbedtls TLS buffers).
 */
#include <stdlib.h>

#define helix_malloc(size)      malloc(size)
#define helix_free(ptr)         free(ptr)
#define helix_realloc(ptr, sz)  realloc((ptr), (sz))
