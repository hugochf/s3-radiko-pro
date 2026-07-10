/*
 * SNTP time sync (JST). Multi-server, polls in the background and syncs once the
 * network is up. Non-blocking: timesync_valid() is false (UI shows --:--) until
 * the first successful sync.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void timesync_start(void);
bool timesync_valid(void);   // true once the clock has been set

#ifdef __cplusplus
}
#endif
