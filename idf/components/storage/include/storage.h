/*
 * SD-card storage (Phase 29). Mounts the board's micro-SD slot (SDMMC 4-bit,
 * dedicated pins per the BSP) at boot and keeps it mounted for the recorder and
 * the recordings player. Bring-up was proven on-device: SDHC 15 GB, ~1 MB/s
 * write, worst single-block stall ~113 ms — hence writes belong on their own
 * task, never the decoder's core.
 *
 * No card / unformatted / wiring fault is NOT fatal: storage_ready() returns
 * false and recording is simply unavailable; the radio plays on.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Probe for a card at boot (mounts briefly, reads size, unmounts). The card is
// then mounted ON DEMAND — keeping it mounted starves the TLS handshake of the
// scarce internal RAM. Returns ESP_OK if a card was detected.
esp_err_t storage_init(void);

// Mount the card if not already (refcounted); pair each with storage_release().
// Recording and the recordings browser bracket their SD access with these.
bool      storage_acquire(void);
void      storage_release(void);

bool         storage_ready(void);      // a card was detected at boot
const char  *storage_root(void);       // FAT mount point, e.g. "/sdcard"
uint32_t     storage_total_mb(void);   // 0 if no card
uint32_t     storage_free_mb(void);    // 0 if no card

#ifdef __cplusplus
}
#endif
