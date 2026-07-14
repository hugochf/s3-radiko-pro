/*
 * OTA from GitHub releases (Phase 22).
 *
 * Two-phase on purpose: ota_check() is cheap and leaves playback alone; only
 * when the caller decides to proceed does ota_update() download into the
 * inactive app slot and reboot. Rollback safety comes from the bootloader
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): the new image boots PENDING_VERIFY
 * and main marks it valid after 30 s alive — a crash or watchdog panic before
 * that reverts to this firmware automatically on the next boot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Query the latest GitHub release. On ESP_OK: *newer says whether its version
// beats the running one; tag/url receive the release tag and the .bin asset
// URL. Network errors return the underlying esp_err_t.
esp_err_t ota_check(char *tag, size_t tag_cap, char *url, size_t url_cap,
                    bool *newer);

// Download `url` into the inactive slot and reboot. Does not return on
// success. cb (optional) gets status text + percent (-1 = indeterminate);
// it is called from the OTA task's context.
typedef void (*ota_progress_cb_t)(const char *status, int pct);
esp_err_t ota_update(const char *url, ota_progress_cb_t cb);

#ifdef __cplusplus
}
#endif
