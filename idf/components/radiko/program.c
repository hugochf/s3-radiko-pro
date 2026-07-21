/*
 * Radiko "now on air" program info.
 *
 * radiko.jp/v3/program/now/{area}.xml returns the current + upcoming programmes
 * for every station in the area, in one XML (~14 KB gzipped, ~68 KB raw), which
 * we inflate with puff. Radiko does NOT reliably gzip unasked — a whole-day
 * schedule came back as 90-120 KB of plain XML — so we send Accept-Encoding and
 * still size the buffers for the uncompressed case; the server picks. Fetched
 * over plain HTTP on purpose: the data is public and this avoids a second TLS
 * session competing with the stream's for the scarce ~40 KB of contiguous
 * internal RAM that mbedtls needs.
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
#include "radiko_parse.h"
#include "httpc.h"
#include "stations.h"

static const char *TAG = "radiko_prog";

// Radiko does NOT always gzip: without an explicit Accept-Encoding it serves a
// whole-day schedule as plain XML (90-120 KB). The old 48 KB response buffer then
// silently kept the first 48 KB and the day's guide simply ended mid-afternoon.
// So: ask for gzip (keeping transfers ~18 KB), but still size the buffer for the
// uncompressed worst case, because the server decides the encoding, not us.
#define GZ_CAP   (192 * 1024)   // response as received: gzip (~18 KB) OR plain XML
#define XML_CAP  (256 * 1024)   // decompressed XML — a busy day is ~120 KB raw
#define PROG_MAX 256            // programme title + performers (pfm), UTF-8

// Current-programme titles, parallel to the active station list. Guarded by s_lock.
static char             s_prog[MAX_STATIONS][PROG_MAX];
static SemaphoreHandle_t s_lock;
static void (*s_on_update)(void);

// Pull each station's current programme out of the flat XML. The extraction
// itself (title + 出演者, entity decoding, block bounding) is pure and lives
// in radiko_parse.c — host-unit-tested in test/host.
static void parse_all(const char *xml)
{
    for (int i = 0; i < stations_count(); i++) {
        char text[PROG_MAX];
        radiko_parse_now(xml, station_id(i), text, sizeof(text));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strcpy(s_prog[i], text);
        xSemaphoreGive(s_lock);
    }
}

// Fetch a gzip'd Radiko programme-XML endpoint over plain HTTP (public data, and
// plain HTTP keeps a second TLS session off the scarce internal RAM) and inflate
// it into `xml` (NUL-terminated). Returns the XML length, or 0 on any failure.
static size_t fetch_gz_xml(const char *url, char *xml, size_t xml_cap)
{
    uint8_t *gz = heap_caps_malloc(GZ_CAP, MALLOC_CAP_SPIRAM);
    if (!gz) return 0;
    // Ask for gzip: unasked, Radiko serves a whole day as ~120 KB of plain XML.
    static const http_header_t hdrs[] = {{ "Accept-Encoding", "gzip" }};
    http_req_t req = {
        .url = url, .method = HTTP_METHOD_GET,
        .req_headers = hdrs, .n_req_headers = 1,
        .body = (char *)gz, .body_cap = GZ_CAP,
    };
    esp_err_t err = httpc_do(&req);
    if (err != ESP_OK || req.status != 200 || req.body_len < 18) {
        ESP_LOGW(TAG, "fetch failed (err=%s status=%d len=%u)",
                 esp_err_to_name(err), req.status, (unsigned)req.body_len);
        free(gz);
        return 0;
    }
    // httpc fills at most body_cap-1, so this means the response was cut off. It
    // used to pass silently as a short-but-valid schedule (the guide just ended
    // mid-afternoon) — say so instead of quietly serving partial data.
    if (req.body_len >= GZ_CAP - 1)
        ESP_LOGE(TAG, "response filled the %u KB buffer — truncated", GZ_CAP / 1024);

    size_t xlen = 0, dlen = 0;
    const uint8_t *deflate = radiko_gzip_body(gz, req.body_len, &dlen);
    if (deflate) {
        // puff inflates raw DEFLATE using the output buffer itself as the history
        // window and only ~2 KB of stack — no multi-KB decompressor object. That
        // matters here: the internal heap is fragmented to ~15 KB blocks while the
        // stream's TLS holds its ~16 KB buffers, so anything bigger can't allocate.
        unsigned long outlen = xml_cap - 1, srclen = dlen;
        int pr = puff((unsigned char *)xml, &outlen, deflate, &srclen);
        if (pr == 0) xlen = outlen;
        else ESP_LOGW(TAG, "puff=%d body=%u deflate=%u out=%lu", pr,
                      (unsigned)req.body_len, (unsigned)dlen, outlen);
    } else if (req.body_len < xml_cap) {   // not gzip (unexpected) — treat as plain XML
        memcpy(xml, gz, req.body_len);
        xlen = req.body_len;
    }
    free(gz);
    if (xlen) xml[xlen] = '\0';
    return xlen;
}

esp_err_t radiko_program_refresh(void)
{
    if (radiko_area()[0] == '\0') return ESP_ERR_INVALID_STATE;

    char url[96];
    snprintf(url, sizeof(url), "http://radiko.jp/v3/program/now/%s.xml", radiko_area());

    char *xml = heap_caps_malloc(XML_CAP, MALLOC_CAP_SPIRAM);
    if (!xml) return ESP_ERR_NO_MEM;
    size_t xlen = fetch_gz_xml(url, xml, XML_CAP);
    if (xlen == 0) { free(xml); return ESP_FAIL; }

    parse_all(xml);
    free(xml);
    ESP_LOGI(TAG, "program info updated (%u bytes XML)", (unsigned)xlen);
    if (s_on_update) s_on_update();
    return ESP_OK;
}

// Time-free (Phase 29b): one station's whole-day schedule. BLOCKING network call
// — run it off a worker task, never the LVGL thread. `date` is "YYYYMMDD" (JST
// broadcast day). Returns the number of programmes written to `out` (0 on error).
int radiko_guide_fetch(const char *station, const char *date,
                       radiko_prog_t *out, int max)
{
    if (!station || !date || max <= 0) return 0;
    char url[128];
    snprintf(url, sizeof(url),
             "http://radiko.jp/v3/program/station/date/%s/%s.xml", date, station);

    char *xml = heap_caps_malloc(XML_CAP, MALLOC_CAP_SPIRAM);
    if (!xml) return 0;
    size_t xlen = fetch_gz_xml(url, xml, XML_CAP);
    int n = xlen ? radiko_parse_day(xml, station, out, max) : 0;
    free(xml);
    ESP_LOGI(TAG, "guide %s %s: %d programmes (%u bytes)", station, date, n, (unsigned)xlen);
    return n;
}

void radiko_program_title(const char *sid, char *out, size_t out_len)
{
    if (out_len) out[0] = '\0';
    if (!s_lock || !sid) return;
    for (int i = 0; i < stations_count(); i++) {
        if (strcmp(station_id(i), sid) != 0) continue;
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
        // On failure retry in 30 s, not 5 min — one bad fetch at boot would
        // otherwise leave every title blank for the whole first interval.
        esp_err_t err = radiko_program_refresh();
        vTaskDelay(pdMS_TO_TICKS(err == ESP_OK ? 5 * 60 * 1000 : 30 * 1000));
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
