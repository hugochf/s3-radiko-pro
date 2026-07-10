#pragma once
/*
 * ESP32 has a unified address space — no separate program memory. PROGMEM is a
 * no-op and pgm_read_* are plain dereferences (replaces upstream's AVR-oriented
 * utils/helix_pgm.h).
 */
#ifndef PROGMEM
#define PROGMEM
#endif

#define pgm_read_byte(addr) (*(const unsigned char  *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
