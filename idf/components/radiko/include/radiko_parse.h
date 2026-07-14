/*
 * Pure Radiko programme-XML / gzip parsing — libc only, no ESP-IDF
 * dependencies, so it is unit-tested on the host (test/host).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode the XML entities that appear in programme titles, in place
// (shrink-only). UTF-8 passes through untouched; unknown entities are kept.
void radiko_xml_unescape(char *s);

// Strip the gzip wrapper: returns a pointer to the raw DEFLATE stream and its
// length (excluding the 8-byte trailer), or NULL if not a valid gzip member.
// Handles the FEXTRA/FNAME/FCOMMENT/FHCRC header flags.
const uint8_t *radiko_gzip_body(const uint8_t *b, size_t len, size_t *out_len);

// Extract the on-air programme for one station from the /v3/program/now XML:
// "<title>" plus " / <pfm>" when performers are present (Arduino format).
// out is "" when the station or title is absent. Matching is bounded to the
// station's own block (the id match includes the closing quote so JOAK and
// JOAK-FM can't cross-match).
void radiko_parse_now(const char *xml, const char *station_id,
                      char *out, size_t cap);

#ifdef __cplusplus
}
#endif
