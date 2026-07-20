#include "radiko_parse.h"

#include <stdio.h>
#include <string.h>

void radiko_xml_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r != '&') { *w++ = *r++; continue; }
        if      (!strncmp(r, "&amp;",  5)) { *w++ = '&';  r += 5; }
        else if (!strncmp(r, "&lt;",   4)) { *w++ = '<';  r += 4; }
        else if (!strncmp(r, "&gt;",   4)) { *w++ = '>';  r += 4; }
        else if (!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; }
        else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
        else if (!strncmp(r, "&#39;",  5)) { *w++ = '\''; r += 5; }
        else                               { *w++ = *r++; }
    }
    *w = '\0';
}

const uint8_t *radiko_gzip_body(const uint8_t *b, size_t len, size_t *out_len)
{
    if (len < 18 || b[0] != 0x1f || b[1] != 0x8b) return NULL;
    size_t h = 10;
    uint8_t flg = b[3];
    if (flg & 0x04) { if (h + 2 > len) return NULL; h += 2 + b[h] + (b[h + 1] << 8); }  // FEXTRA
    if (flg & 0x08) { while (h < len && b[h]) h++; h++; }   // FNAME
    if (flg & 0x10) { while (h < len && b[h]) h++; h++; }   // FCOMMENT
    if (flg & 0x02) h += 2;                                 // FHCRC
    if (h + 8 >= len) return NULL;
    *out_len = len - h - 8;
    return b + h;
}

void radiko_parse_now(const char *xml, const char *station_id,
                      char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';

    char tag[40];
    snprintf(tag, sizeof(tag), "<station id=\"%s\"", station_id);
    const char *st = strstr(xml, tag);
    if (!st) return;

    const char *next = strstr(st + 1, "<station id=");
    const char *pend = strstr(st, "</prog>");      // end of the on-air prog
    const char *ts   = strstr(st, "<title>");
    const char *te   = ts ? strstr(ts, "</title>") : NULL;
    if (!ts || !te || (next && te >= next)) return;

    ts += 7;
    size_t n = (size_t)(te - ts);
    if (n >= cap) n = cap - 1;
    memcpy(out, ts, n);
    out[n] = '\0';
    radiko_xml_unescape(out);
    size_t len = strlen(out);

    // Performers, if present and non-empty within this same prog block.
    const char *ps = strstr(te, "<pfm>");
    const char *pe = ps ? strstr(ps, "</pfm>") : NULL;
    if (ps && pe && pe > ps + 5 &&
        (!pend || pe < pend) && (!next || pe < next) &&
        len + 3 < cap - 1) {
        ps += 5;
        memcpy(out + len, " / ", 3);
        len += 3;
        size_t n2 = (size_t)(pe - ps);
        if (len + n2 >= cap) n2 = cap - 1 - len;
        memcpy(out + len, ps, n2);
        out[len + n2] = '\0';
        radiko_xml_unescape(out + len);
    }
}

// Copy an attribute value that starts right after `attr` (e.g. "ft=\"") up to the
// closing quote, capped at `cap`-1. Returns 0 if the attribute isn't in [p, end).
static int copy_attr(const char *p, const char *end, const char *attr,
                     char *out, size_t cap)
{
    const char *a = strstr(p, attr);
    if (!a || a >= end) { out[0] = '\0'; return 0; }
    a += strlen(attr);
    const char *q = strchr(a, '"');
    if (!q || q > end) { out[0] = '\0'; return 0; }
    size_t n = (size_t)(q - a);
    if (n >= cap) n = cap - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return 1;
}

int radiko_parse_day(const char *xml, const char *station_id,
                     radiko_prog_t *out, int max)
{
    if (max <= 0) return 0;
    char tag[40];
    snprintf(tag, sizeof(tag), "<station id=\"%s\"", station_id);
    const char *st = strstr(xml, tag);
    if (!st) return 0;
    const char *next = strstr(st + 1, "<station id=");   // block boundary

    int n = 0;
    const char *p = st;
    while (n < max) {
        const char *prog = strstr(p, "<prog ");
        if (!prog || (next && prog >= next)) break;
        const char *pend = strstr(prog, "</prog>");
        if (!pend) break;

        const char *ts = strstr(prog, "<title>");
        const char *te = ts ? strstr(ts, "</title>") : NULL;
        // A well-formed <prog …> needs ft/to attributes and a title in its block.
        if (copy_attr(prog, pend, "ft=\"", out[n].ft, sizeof(out[n].ft)) &&
            copy_attr(prog, pend, "to=\"", out[n].to, sizeof(out[n].to)) &&
            ts && te && te < pend) {
            ts += 7;
            size_t tn = (size_t)(te - ts);
            if (tn >= RADIKO_PROG_TITLE) tn = RADIKO_PROG_TITLE - 1;
            memcpy(out[n].title, ts, tn);
            out[n].title[tn] = '\0';
            radiko_xml_unescape(out[n].title);
            n++;
        }
        p = pend + 7;   // continue after this </prog>
    }
    return n;
}
