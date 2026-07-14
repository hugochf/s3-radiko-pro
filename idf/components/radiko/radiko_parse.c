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
