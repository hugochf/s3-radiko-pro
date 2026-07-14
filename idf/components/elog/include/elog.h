/*
 * Persistent event log (Phase 20).
 *
 * Warnings and errors (plus a boot marker with the reset reason) are captured
 * from the esp_log stream into a RAM buffer and flushed to a dedicated 64 KB
 * raw flash ring ("elog" partition) by a low-priority task. The log survives
 * reboots and crashes, so "what happened before it died in the field" is
 * answerable over USB with no console attached:
 *
 *   parttool.py -p PORT read_partition --partition-name=elog --output elog.bin
 *   python3 tools/elog_dump.py elog.bin
 *
 * Deliberately text-based (the flash content is readable with `strings`) and
 * deliberately low-rate: INFO chatter stays out, so wear on the 64 KB ring is
 * negligible (a full wrap erases each 4 KB sector once per ~64 KB of W/E text).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the esp_log hook, locate the ring head, and write the boot marker.
// Call once, early (before subsystems that might warn). Console output is
// unaffected — every line still goes to the original vprintf.
esp_err_t elog_init(void);

#ifdef __cplusplus
}
#endif
