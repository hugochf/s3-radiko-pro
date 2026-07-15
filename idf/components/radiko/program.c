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
#include "radiko_parse.h"
#include "httpc.h"
#include "stations.h"

static const char *TAG = "radiko_prog";

#define GZ_CAP   (48 * 1024)    // compressed response (~14 KB) + headroom
#define XML_CAP  (128 * 1024)   // decompressed XML (~68 KB) + headroom
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
    const uint8_t *deflate = radiko_gzip_body(gz, req.body_len, &dlen);
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
