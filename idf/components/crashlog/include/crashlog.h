/*
 * Crash post-mortem (Phase 19).
 *
 * Panics (including watchdog starvation panics, Phase 18) write an ELF
 * coredump to the dedicated flash partition. On the next boot crashlog_check()
 * detects it, logs a decoded summary (task, PC, backtrace addresses ready for
 * addr2line), and keeps a one-line description for the Settings screen. The
 * dump itself stays in flash for host-side retrieval (`idf.py coredump-info`,
 * see docs/debugging.md) until the next crash overwrites it.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot (after flash is up). Safe if no dump is stored.
void crashlog_check(void);

// One-line summary of the stored crash ("none" if the last boot was clean).
// Valid after crashlog_check(); shown in Settings > System Info.
const char *crashlog_last(void);

#ifdef __cplusplus
}
#endif
