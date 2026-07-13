/*
 * Radiko "now on air" program info.
 *
 * radiko.jp/v3/program/now/{area}.xml returns the current + upcoming programmes
 * for every station in the area, in one gzip'd XML (~14 KB compressed, ~68 KB
 * raw). Radiko always gzips, even unasked, so we decompress with the ROM miniz
 * (tinfl) — no bundled zlib. Fetched over plain HTTP on purpose: the data is
 * public and this avoids a second TLS session competing with the stream's for
 * the scarce ~40 KB of contiguous internal RAM that mbedtls needs.
 */
#include "radiko.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "puff.h"           // zlib's minimal inflate: ~2 KB stack, no heap
#include "httpc.h"
#include "stations.h"

static const char *TAG = "radiko_prog";

#define GZ_CAP   (48 * 1024)    // compressed response (~14 KB) + headroom
#define XML_CAP  (128 * 1024)   // decompressed XML (~68 KB) + headroom
#define PROG_MAX 256            // programme title + performers (pfm), UTF-8

// Current-programme titles, parallel to STATIONS[]. Guarded by s_lock.
static char             s_prog[NUM_STATIONS][PROG_MAX];
static SemaphoreHandle_t s_lock;
static void (*s_on_update)(void);

// Decode the handful of XML entities that show up in Japanese programme titles.
// Full-width text is UTF-8 and passes through untouched. In-place, shrink-only.
static void xml_unescape(char *s)
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

// Strip the gzip wrapper, returning a pointer to the raw DEFLATE stream and its
// length (excluding the 8-byte CRC/size trailer). NULL if not a gzip member.
static const uint8_t *gzip_body(const uint8_t *b, size_t len, size_t *out_len)
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

// Pull each station's current programme out of the flat XML. The "now" feed
// lists the current programme first, so the first <prog> per <station> is
// what's on air. Extracts <title> plus <pfm> (performers/出演者), matching the
// Arduino UI. All matches are bounded to their own <station>/<prog> block.
static void parse_all(const char *xml)
{
    for (int i = 0; i < STATION_COUNT; i++) {
        char tag[40];
        snprintf(tag, sizeof(tag), "<station id=\"%s\"", STATIONS[i].id);  // trailing
        const char *st  = strstr(xml, tag);        // quote in %s distinguishes JOAK / JOAK-FM
        char text[PROG_MAX] = "";
        size_t len = 0;
        if (st) {
            const char *next = strstr(st + 1, "<station id=");
            const char *pend = strstr(st, "</prog>");      // end of the on-air prog
            const char *ts   = strstr(st, "<title>");
            const char *te   = ts ? strstr(ts, "</title>") : NULL;
            if (ts && te && (!next || te < next)) {
                ts += 7;
                size_t n = (size_t)(te - ts);
                if (n >= PROG_MAX) n = PROG_MAX - 1;
                memcpy(text, ts, n);
                text[n] = '\0';
                xml_unescape(text);
                len = strlen(text);

                // Performers, if present within this same prog block.
                const char *ps = strstr(te, "<pfm>");
                const char *pe = ps ? strstr(ps, "</pfm>") : NULL;
                if (ps && pe && pe > ps + 5 &&   // non-empty (avoid trailing " / ")
                    (!pend || pe < pend) && (!next || pe < next) &&
                    len + 3 < PROG_MAX - 1) {
                    ps += 5;
                    memcpy(text + len, " / ", 3);
                    len += 3;
                    size_t n2 = (size_t)(pe - ps);
                    if (len + n2 >= PROG_MAX) n2 = PROG_MAX - 1 - len;
                    memcpy(text + len, ps, n2);
                    text[len + n2] = '\0';
                    xml_unescape(text + len);
                }
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strcpy(s_prog[i], text);
        xSemaphoreGive(s_lock);
    }
}

esp_err_t radiko_program_refresh(void)
{
    if (radiko_area()[0] == '\0') return ESP_ERR_INVALID_STATE;

    char url[96];
    snprintf(url, sizeof(url), "http://radiko.jp/v3/program/now/%s.xml", radiko_area());

    uint8_t *gz = heap_caps_malloc(GZ_CAP, MALLOC_CAP_SPIRAM);
    if (!gz) return ESP_ERR_NO_MEM;

    http_req_t req = {
        .url = url, .method = HTTP_METHOD_GET,
        .body = (char *)gz, .body_cap = GZ_CAP,
    };
    esp_err_t err = httpc_do(&req);
    if (err != ESP_OK || req.status != 200 || req.body_len < 18) {
        ESP_LOGW(TAG, "fetch failed (err=%s status=%d len=%u)",
                 esp_err_to_name(err), req.status, (unsigned)req.body_len);
        free(gz);
        return ESP_FAIL;
    }

    char *xml = heap_caps_malloc(XML_CAP, MALLOC_CAP_SPIRAM);
    if (!xml) { free(gz); return ESP_ERR_NO_MEM; }

    size_t xlen = 0, dlen = 0;
    const uint8_t *deflate = gzip_body(gz, req.body_len, &dlen);
    if (deflate) {
        // puff inflates raw DEFLATE using the output buffer itself as the history
        // window and only ~2 KB of stack — no multi-KB decompressor object. That
        // matters here: the internal heap is fragmented to ~15 KB blocks while the
        // stream's TLS holds its ~16 KB buffers, so anything bigger can't allocate.
        unsigned long outlen = XML_CAP - 1, srclen = dlen;
        int pr = puff((unsigned char *)xml, &outlen, deflate, &srclen);
        if (pr == 0) xlen = outlen;
        else ESP_LOGW(TAG, "puff=%d body=%u deflate=%u out=%lu", pr,
                      (unsigned)req.body_len, (unsigned)dlen, outlen);
    } else if (req.body_len < XML_CAP) {   // not gzip (unexpected) — treat as plain XML
        memcpy(xml, gz, req.body_len);
        xlen = req.body_len;
    }
    free(gz);

    if (xlen == 0) { free(xml); ESP_LOGW(TAG, "decompress failed"); return ESP_FAIL; }
    xml[xlen] = '\0';

    parse_all(xml);
    free(xml);
    ESP_LOGI(TAG, "program info updated (%u bytes XML)", (unsigned)xlen);
    if (s_on_update) s_on_update();
    return ESP_OK;
}

void radiko_program_title(const char *station_id, char *out, size_t out_len)
{
    if (out_len) out[0] = '\0';
    if (!s_lock || !station_id) return;
    for (int i = 0; i < STATION_COUNT; i++) {
        if (strcmp(STATIONS[i].id, station_id) != 0) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, out_len, "%s", s_prog[i]);
        xSemaphoreGive(s_lock);
        return;
    }
}

// Refresh now, then every 5 min. Runs on core 0 (with wifi/fetcher). The caller
// defers creating this task until the stream is established, so its 16 KB stack
// isn't allocated while the stream's TLS handshake needs contiguous internal RAM.
static void prog_task(void *arg)
{
    // Created at boot (see radiko_program_start) so this stack is allocated
    // before any TLS session runs — a mid-flight stack allocation can split the
    // largest free internal block below the ~16 KB contiguous that mbedtls needs
    // and permanently break the stream's TLS. Waits for auth before first use,
    // then gives the stream a head start on its own TLS session.
    while (radiko_area()[0] == '\0') vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(8000));
    for (;;) {
        radiko_program_refresh();
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    }
}

void radiko_program_start(void (*on_update)(void))
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_on_update = on_update;
    // 6 KB stack is ample now decompression is puff (~2 KB) instead of tinfl.
    BaseType_t ok = xTaskCreatePinnedToCore(prog_task, "radiko_prog", 6144, NULL, 4, NULL, 0);
    if (ok != pdPASS) ESP_LOGE(TAG, "prog task create failed (low internal RAM)");
}
