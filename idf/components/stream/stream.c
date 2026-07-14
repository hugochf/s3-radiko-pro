#include "stream.h"

#include <stdio.h>
#include <string.h>
#include "aacdec.h"
#include "audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radiko.h"

static const char *TAG = "stream";

// Plain HTTP on purpose: public data, no token sent, and it skips a ~5 s TLS
// handshake on the boot->first-audio path (verified 200, no redirect).
#define STREAM_INFO_FMT "http://radiko.jp/v3/station/stream/pc_html5/%s.xml"
#define SEG_BUF_BYTES   (64 * 1024)   // one AAC segment (PSRAM)
#define PLIST_BYTES     4096
#define SEG_QUEUE_DEPTH 4             // fetched segments waiting to be decoded

// Two-stage pipeline: the fetcher pulls AAC segments over the network (~4 s each)
// into a queue; the decoder drains the queue -> libhelix -> audio. This overlaps
// network latency with real-time playback so throughput stays >= 1x.
typedef struct { char *buf; int len; } seg_t;

static QueueHandle_t    s_q          = NULL;
static TaskHandle_t     s_fetch_task = NULL;
static TaskHandle_t     s_dec_task   = NULL;
static volatile bool    s_stop       = false;
static char             s_station[16];

// Persistent keep-alive HTTPS client (fetcher task only).
static esp_http_client_handle_t s_client = NULL;
static struct { char *buf; size_t cap; size_t len; } s_ctx;

static void make_lsid(char *out /* [33] */)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", rnd[i]);
    out[32] = '\0';
}

static esp_err_t on_http(esp_http_client_event_t *e)
{
    // Abort an in-flight download the instant a stop/switch is requested, so
    // stopping the old station doesn't wait for its ~5 s segment fetch.
    if (s_stop) return ESP_FAIL;
    if (e->event_id == HTTP_EVENT_ON_DATA && s_ctx.buf && s_ctx.cap) {
        size_t space = s_ctx.cap - 1 - s_ctx.len;
        size_t n = ((size_t)e->data_len < space) ? (size_t)e->data_len : space;
        memcpy(s_ctx.buf + s_ctx.len, e->data, n);
        s_ctx.len += n;
        s_ctx.buf[s_ctx.len] = '\0';
    }
    return ESP_OK;
}

static int fetch(const char *url, const char *token, char *buf, size_t cap)
{
    s_ctx.buf = buf; s_ctx.cap = cap; s_ctx.len = 0;
    if (buf && cap) buf[0] = '\0';
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, HTTP_METHOD_GET);
    if (token && token[0]) esp_http_client_set_header(s_client, "X-Radiko-AuthToken", token);
    if (esp_http_client_perform(s_client) != ESP_OK) return -1;
    int st = esp_http_client_get_status_code(s_client);
    return (st == 200) ? (int)s_ctx.len : -st;
}

// Capped exponential backoff for transient failures at any stage of the HLS
// chain, so a persistent outage never hammers Radiko at 1 Hz forever.
static int next_backoff(int cur)
{
    return cur == 0 ? 1000 : (cur >= 10000 ? 10000 : cur * 2);
}

// 401/403 mean the auth token was rejected — retrying with the same token can
// never succeed (fetch() returns -status for HTTP errors).
static bool auth_rejected(int fetch_ret)
{
    return fetch_ret == -401 || fetch_ret == -403;
}

static bool first_url_line(const char *body, char *out, size_t cap)
{
    for (const char *p = body; *p; ) {
        const char *nl = strpbrk(p, "\r\n");
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[0] != '#') {
            if (len >= cap) len = cap - 1;
            memcpy(out, p, len); out[len] = '\0';
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

// ---- Fetcher: resolve the HLS chain, push new segments onto the queue ----
static void fetcher_task(void *arg)
{
    char *plist = heap_caps_malloc(PLIST_BYTES, MALLOC_CAP_SPIRAM);
    if (!plist || !s_client) { ESP_LOGE(TAG, "fetcher init failed"); goto done; }

    const char *token = radiko_token();
    // base (playlist_create_url) is station-independent — resolve it once and
    // reuse across station changes to skip the stream-info fetch.
    static char base[160] = {0};
    char media_url[400] = {0}, last_seg[256] = {0};

    ESP_LOGI(TAG, "streaming %s", s_station);

    int backoff = 0;    // grows on repeated failures at any stage (don't hammer Radiko)
    int empty = 0;      // consecutive live-playlist polls that yielded no new segment
    int auth_fail = 0;  // consecutive 401/403s — the token has expired/been revoked
    while (!s_stop) {
        // Token rejected twice in a row → it's dead (Radiko can invalidate it at
        // any time). Re-authenticate in place and resync to the live edge; every
        // other recovery path would just loop forever with the same dead token.
        if (auth_fail >= 2) {
            ESP_LOGW(TAG, "auth token rejected; re-authenticating");
            radiko_auth_t a;
            if (radiko_authenticate(&a) == ESP_OK) {
                auth_fail = 0;
                backoff = 0;
                token = radiko_token();
            } else {
                backoff = next_backoff(backoff);
                ESP_LOGW(TAG, "re-auth failed; retry in %d ms", backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff));
            }
            media_url[0] = '\0';
            last_seg[0] = '\0';
            continue;
        }

        // base (playlist_create_url) resolves once and is reused across stations.
        // Retry transient failures here rather than killing the fetcher — a brief
        // TLS/network hiccup at startup must not permanently stop audio.
        if (base[0] == '\0') {
            char info_url[160];
            snprintf(info_url, sizeof(info_url), STREAM_INFO_FMT, s_station);
            char *p = NULL, *e = NULL;
            if (fetch(info_url, NULL, plist, PLIST_BYTES) > 0 &&
                (p = strstr(plist, "<playlist_create_url>")) != NULL &&
                (e = strstr(p, "</playlist_create_url>")) != NULL) {
                p += strlen("<playlist_create_url>");
                size_t n = (size_t)(e - p); if (n >= sizeof(base)) n = sizeof(base) - 1;
                memcpy(base, p, n); base[n] = '\0';
                backoff = 0;
            } else {
                backoff = next_backoff(backoff);
                ESP_LOGW(TAG, "stream-info failed; retry in %d ms", backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff));
                continue;
            }
        }

        if (media_url[0] == '\0') {
            char lsid[33]; make_lsid(lsid);
            char purl[320];
            snprintf(purl, sizeof(purl), "%s?station_id=%s&l=30&lsid=%s&type=b",
                     base, s_station, lsid);
            int r = fetch(purl, token, plist, PLIST_BYTES);
            if (r <= 0 || !first_url_line(plist, media_url, sizeof(media_url))) {
                if (auth_rejected(r)) auth_fail++;
                backoff = next_backoff(backoff);
                ESP_LOGW(TAG, "master playlist failed (%d); retry in %d ms", r, backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff));
                continue;
            }
        }

        int r = fetch(media_url, token, plist, PLIST_BYTES);
        if (r <= 0) {
            // Session gone — re-resolve. Clear last_seg too: the new session has a
            // fresh live window with different segment names, so keeping the old
            // one means seen_last never matches and every segment is skipped
            // forever (audio dies after the first batch). Resync to the live edge.
            if (auth_rejected(r)) auth_fail++;
            media_url[0] = '\0';
            last_seg[0] = '\0';
            continue;
        }
        auth_fail = 0;   // token accepted — any earlier 401/403 was transient
        backoff = 0;

        bool seen_last = (last_seg[0] == '\0');
        int pushed = 0;
        char *save = NULL;
        for (char *line = strtok_r(plist, "\r\n", &save); line && !s_stop;
             line = strtok_r(NULL, "\r\n", &save)) {
            if (!line[0] || line[0] == '#') continue;
            if (!seen_last) { if (!strcmp(line, last_seg)) seen_last = true; continue; }

            char *buf = heap_caps_malloc(SEG_BUF_BYTES, MALLOC_CAP_SPIRAM);
            if (!buf) { ESP_LOGW(TAG, "seg buf alloc fail"); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            int slen = fetch(line, token, buf, SEG_BUF_BYTES);
            if (slen <= 0) {
                if (auth_rejected(slen)) auth_fail++;
                ESP_LOGW(TAG, "seg fetch failed (%d)", slen);
                free(buf);
                continue;
            }

            seg_t seg = { buf, slen };
            // Blocks when the queue is full -> paces the fetcher to real time.
            while (!s_stop && xQueueSend(s_q, &seg, pdMS_TO_TICKS(200)) != pdTRUE) { }
            if (s_stop) { free(buf); break; }
            strncpy(last_seg, line, sizeof(last_seg) - 1);
            last_seg[sizeof(last_seg) - 1] = '\0';
            pushed++;
        }
        if (pushed > 0) {
            empty = 0;
        } else {
            // A few empty polls is normal at the live edge, but many in a row means
            // the medialist session went stale — Radiko expires it after a few
            // minutes and then serves a frozen playlist with HTTP 200, so the fetch
            // never fails and audio silently dies. Force a fresh session.
            if (++empty >= 5) {
                ESP_LOGI(TAG, "live playlist stale; new session");
                empty = 0; media_url[0] = '\0'; last_seg[0] = '\0';
            }
            vTaskDelay(pdMS_TO_TICKS(1000));   // at live edge
        }
    }

done:
    free(plist);   // s_client persists across stations (keep-alive)
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

// ---- Decoder: drain the queue, decode ADTS AAC, feed audio ----
static void decoder_task(void *arg)
{
    HAACDecoder dec = AACInitDecoder();
    if (!dec) {
        // Should never happen (decoder state is a small fixed alloc), but if it
        // does, keep draining the queue so the fetcher doesn't block forever on
        // a full queue with stale segments.
        ESP_LOGE(TAG, "AAC decoder alloc failed — no audio this session");
        while (!s_stop) {
            seg_t seg;
            if (xQueueReceive(s_q, &seg, pdMS_TO_TICKS(300)) == pdTRUE) free(seg.buf);
        }
        s_dec_task = NULL;
        vTaskDelete(NULL);
    }
    static int16_t pcm[2048 * 2];
    AACFrameInfo fi;
    bool logged = false;

    while (!s_stop) {
        seg_t seg;
        if (xQueueReceive(s_q, &seg, pdMS_TO_TICKS(300)) != pdTRUE) continue;

        unsigned char *inp = (unsigned char *)seg.buf;
        int left = seg.len;
        int frames = 0;
        while (left > 0 && !s_stop) {
            int off = AACFindSyncWord(inp, left);
            if (off < 0) break;
            inp += off; left -= off;
            if (AACDecode(dec, &inp, &left, pcm)) { inp++; left--; continue; }
            AACGetLastFrameInfo(dec, &fi);
            if (!logged) {
                logged = true;
                ESP_LOGI(TAG, "decode: %d Hz, %d ch", fi.sampRateOut, fi.nChans);
            }
            audio_write(pcm, fi.outputSamps * sizeof(int16_t), NULL);
            // Yield every ~32 frames (~0.7 s of audio) so this core's idle task
            // feeds the watchdog even mid-segment. Between-segments alone isn't
            // enough during a catch-up burst: several segments decoded
            // back-to-back kept the CPU >5 s and tripped the IDLE watchdog.
            if ((++frames & 31) == 0) vTaskDelay(1);
        }
        free(seg.buf);
        vTaskDelay(1);   // idle/watchdog breather between segments too
    }

    if (dec) AACFreeDecoder(dec);
    s_dec_task = NULL;
    vTaskDelete(NULL);
}

// ---- Internal (blocking) start/stop, run only from the control task ----
static void start_stream(const char *station_id)
{
    strncpy(s_station, station_id, sizeof(s_station) - 1);
    s_station[sizeof(s_station) - 1] = '\0';
    if (!s_q) s_q = xQueueCreate(SEG_QUEUE_DEPTH, sizeof(seg_t));
    s_stop = false;
    audio_resume();     // unmute, start from an empty buffer (fresh/live)
    xTaskCreatePinnedToCore(fetcher_task, "seg_fetch", 16384, NULL, 5, &s_fetch_task, 0);
    // Decoder on core 0 with the fetcher (which is mostly blocked on network I/O),
    // NOT on core 1 with LVGL: the full-CJK font made rendering heavy enough that
    // sharing core 1 starved whichever task lost — stuttering audio AND laggy UI.
    // Now LVGL owns core 1 and the decoder competes only with I/O-bound tasks.
    xTaskCreatePinnedToCore(decoder_task, "seg_dec",   20480, NULL, 4, &s_dec_task,   0);
}

static void stop_stream(void)
{
    s_stop = true;
    audio_flush();   // instant silence + unblocks the decoder (so it exits fast)
    // The fetcher can be mid-download, so wait generously for both tasks to exit.
    for (int i = 0; i < 600 && (s_fetch_task || s_dec_task); i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_q) {
        seg_t seg;
        while (xQueueReceive(s_q, &seg, 0) == pdTRUE) free(seg.buf);  // drain leftovers
    }
}

// ---- Player-control task: serialises play/stop so the UI never blocks ----
typedef struct { char station[16]; bool play; } cmd_t;
static QueueHandle_t s_cmd_q = NULL;

static void ctrl_task(void *arg)
{
    cmd_t c;
    while (true) {
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE) continue;
        stop_stream();                                   // stop whatever's playing
        if (c.play && c.station[0]) start_stream(c.station);
    }
}

void stream_control_start(void)
{
    if (!s_cmd_q) s_cmd_q = xQueueCreate(1, sizeof(cmd_t));   // depth 1: latest wins
    // One persistent keep-alive client, reused across every station change so the
    // CDN connection stays warm (no re-handshake on switch).
    if (!s_client) {
        esp_http_client_config_t hcfg = {
            .url = "https://radiko.jp/", .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true, .timeout_ms = 10000, .event_handler = on_http,
            .buffer_size = 4096,
        };
        s_client = esp_http_client_init(&hcfg);
    }
    xTaskCreate(ctrl_task, "stream_ctl", 3072, NULL, 6, NULL);
}

void stream_play(const char *station_id)
{
    cmd_t c = { .play = true };
    strncpy(c.station, station_id, sizeof(c.station) - 1);
    xQueueOverwrite(s_cmd_q, &c);
}

void stream_stop(void)
{
    cmd_t c = { .play = false };
    xQueueOverwrite(s_cmd_q, &c);
}
