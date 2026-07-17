#include "recorder.h"
#include "stream.h"

#include <stdio.h>
#include <string.h>
#include "aacdec.h"
#include "app_watchdog.h"
#include "audio.h"
#include "hls_parse.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radiko.h"
#include "storage.h"

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
static volatile bool    s_stop       = true;   // idle both persistent tasks until a stream starts
static volatile bool    s_live       = false;  // fetcher pulls live segments (vs file playback)
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

// Backoff / auth / playlist-line helpers live in hls_parse.c (pure libc,
// host-unit-tested — see test/host).

// ---- Fetcher: resolve the HLS chain, push new segments onto the queue ----
// PERSISTENT (like the decoder): created ONCE at boot so its 16 KB stack is
// claimed before the SD card fragments internal RAM (Phase 29). Idles between
// live sessions; s_live gates it so file playback (fileplayer_task feeds the
// same queue) can run with the fetcher parked.
static void fetcher_task(void *arg)
{
    // Watchdog: a wedged HTTP client = silent radio with no other symptom.
    // Feeds are placed so the longest un-fed stretch is ONE fetch() (10 s HTTP
    // timeout) — every retry path feeds before its backoff sleep.
    app_watchdog_add();

    char *plist = heap_caps_malloc(PLIST_BYTES, MALLOC_CAP_SPIRAM);
    // base (playlist_create_url) is station-independent — resolve it once and
    // reuse across station changes to skip the stream-info fetch.
    static char base[160] = {0};

  while (true) {
    // Idle until a LIVE session is requested (file playback keeps us parked).
    while (s_stop || !s_live) { app_watchdog_feed(); vTaskDelay(pdMS_TO_TICKS(50)); }
    if (!plist || !s_client) { ESP_LOGE(TAG, "fetcher init failed"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

    const char *token = radiko_token();
    char media_url[400] = {0}, last_seg[256] = {0};

    ESP_LOGI(TAG, "streaming %s", s_station);

    int backoff = 0;    // grows on repeated failures at any stage (don't hammer Radiko)
    int empty = 0;      // consecutive live-playlist polls that yielded no new segment
    int auth_fail = 0;  // consecutive 401/403s — the token has expired/been revoked
    while (!s_stop && s_live) {
        app_watchdog_feed();

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
                backoff = hls_next_backoff(backoff);
                ESP_LOGW(TAG, "re-auth failed; retry in %d ms", backoff);
                app_watchdog_feed();
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
                backoff = hls_next_backoff(backoff);
                ESP_LOGW(TAG, "stream-info failed; retry in %d ms", backoff);
                app_watchdog_feed();
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
            if (r <= 0 || !hls_first_url_line(plist, media_url, sizeof(media_url))) {
                if (hls_auth_rejected(r)) auth_fail++;
                backoff = hls_next_backoff(backoff);
                ESP_LOGW(TAG, "master playlist failed (%d); retry in %d ms", r, backoff);
                app_watchdog_feed();
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
            if (hls_auth_rejected(r)) auth_fail++;
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
            app_watchdog_feed();   // once per segment (each fetch can take 10 s)

            char *buf = heap_caps_malloc(SEG_BUF_BYTES, MALLOC_CAP_SPIRAM);
            if (!buf) { ESP_LOGW(TAG, "seg buf alloc fail"); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            int slen = fetch(line, token, buf, SEG_BUF_BYTES);
            if (slen <= 0) {
                if (hls_auth_rejected(slen)) auth_fail++;
                ESP_LOGW(TAG, "seg fetch failed (%d)", slen);
                free(buf);
                continue;
            }

            // Phase 29: fork a copy to the recorder if capturing. Non-blocking;
            // the recorder never stalls the fetcher (audio always wins).
            recorder_feed(buf, slen);

            seg_t seg = { buf, slen };
            // Blocks when the queue is full -> paces the fetcher to real time
            // (legitimately minutes at the live edge, so feed while waiting).
            while (!s_stop && xQueueSend(s_q, &seg, pdMS_TO_TICKS(200)) != pdTRUE) {
                app_watchdog_feed();
            }
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
    // Live session ended (s_stop or a switch to file playback). Loop to idle;
    // plist/base persist. The task is never deleted (persistent, see header).
  }
}

// ---- Decoder: drain the queue, decode ADTS AAC, feed audio ----
// PERSISTENT: created ONCE at boot (stream_control_start) and never deleted, so
// its 20 KB stack is claimed while internal RAM is unfragmented — before the SD
// card mounts (Phase 29). Re-creating it per stream failed once SDMMC had split
// the largest free block below 20 KB: xTaskCreate returned NULL and the radio
// went silent. s_stop gates a decode session; between sessions the task idles.
static void decoder_task(void *arg)
{
    app_watchdog_add();   // a wedged decode loop = silent radio; panic+reboot

    HAACDecoder dec = AACInitDecoder();
    if (!dec) ESP_LOGE(TAG, "AAC decoder alloc failed — audio disabled");
    static int16_t pcm[2048 * 2];
    AACFrameInfo fi;

    while (true) {
        // Idle between sessions (stopped / a station switch in progress).
        while (s_stop) { app_watchdog_feed(); vTaskDelay(pdMS_TO_TICKS(50)); }

        bool logged = false;
        while (!s_stop) {
            app_watchdog_feed();
            seg_t seg;
            if (xQueueReceive(s_q, &seg, pdMS_TO_TICKS(300)) != pdTRUE) continue;

            unsigned char *inp = (unsigned char *)seg.buf;
            int left = seg.len;
            int frames = 0;
            while (dec && left > 0 && !s_stop) {
                int off = AACFindSyncWord(inp, left);
                if (off < 0) break;
                inp += off; left -= off;
                // Bound the decoder to the bytes we actually hold. libhelix trusts
                // the ADTS frame-length field and reads that many bytes with no
                // internal bounds check, so a FALSE sync near the buffer end (its
                // garbage length exceeds what's left) makes it walk past the
                // allocation -> LoadProhibited. This happens after any desync in a
                // recorded file (Phase 29 crash in seg_dec). Reject a frame whose
                // declared length overruns the buffer and resync one byte on.
                if (left < 7) break;   // ADTS header incomplete -> carry / stop
                int flen = ((inp[3] & 3) << 11) | (inp[4] << 3) | ((inp[5] >> 5) & 7);
                if (flen < 7 || flen > left) { inp++; left--; continue; }
                if (AACDecode(dec, &inp, &left, pcm)) { inp++; left--; continue; }
                AACGetLastFrameInfo(dec, &fi);
                if (!logged) {
                    logged = true;
                    ESP_LOGI(TAG, "decode: %d Hz, %d ch", fi.sampRateOut, fi.nChans);
                }
                audio_write(pcm, fi.outputSamps * sizeof(int16_t), NULL);
                // Yield every ~32 frames (~0.7 s of audio) so this core's idle task
                // feeds the watchdog even mid-segment; a catch-up burst of several
                // back-to-back segments otherwise held the CPU >5 s (IDLE WDT).
                if ((++frames & 31) == 0) vTaskDelay(1);
            }
            free(seg.buf);
            vTaskDelay(1);   // idle/watchdog breather between segments too
        }
        // Session stopped: loop back to idle. stop_stream() drains s_q.
    }
}

// ---- File playback (Phase 29): a recorded .aac -> the SAME decoder/queue ----
// The recorded file is concatenated ADTS AAC. We feed it into s_q exactly like
// the fetcher feeds live segments, so the decoder / PCM ring / I2S path is
// entirely reused — file playback is a second SOURCE, not a second pipeline.
static char          s_file[192];
static TaskHandle_t  s_file_task = NULL;
static void        (*s_end_cb)(void) = NULL;

// Walk ADTS frames in [buf, buf+avail) and return the length of the longest
// prefix that ends exactly on a frame boundary (so a seg never splits a frame,
// which the decoder would drop). 0 if no complete frame yet.
static int adts_framed_len(const uint8_t *b, int avail)
{
    int pos = 0, framed = 0;
    while (pos + 7 <= avail) {
        if (b[pos] != 0xFF || (b[pos + 1] & 0xF0) != 0xF0) { pos++; continue; }  // resync
        int flen = ((b[pos + 3] & 3) << 11) | (b[pos + 4] << 3) | ((b[pos + 5] >> 5) & 7);
        if (flen < 7) { pos++; continue; }
        if (pos + flen > avail) break;   // partial frame at the end -> carry it
        pos += flen; framed = pos;
    }
    return framed;
}

static void fileplayer_task(void *arg)
{
    FILE *f = fopen(s_file, "rb");
    if (!f) { ESP_LOGE(TAG, "playback: can't open %s", s_file); goto done; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "playback: %s (%ld KB)", s_file, fsz / 1024);

    uint8_t *rbuf = heap_caps_malloc(SEG_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!rbuf) { fclose(f); goto done; }

    int carry = 0;
    while (!s_stop) {
        int n = fread(rbuf + carry, 1, SEG_BUF_BYTES - carry, f);
        int avail = carry + n;
        if (avail <= 0) break;   // EOF, nothing left

        int framed = adts_framed_len(rbuf, avail);
        if (framed == 0) {        // no whole frame in a full buffer -> junk; bail
            if (n <= 0) break;
            carry = 0; continue;
        }
        char *seg = heap_caps_malloc(framed, MALLOC_CAP_SPIRAM);
        if (seg) {
            memcpy(seg, rbuf, framed);
            seg_t s = { seg, framed };
            // Blocks while the queue is full -> the PCM ring backpressures the
            // decoder which backpressures us == real-time playback pacing.
            while (!s_stop && xQueueSend(s_q, &s, pdMS_TO_TICKS(200)) != pdTRUE) { }
            if (s_stop) { free(seg); break; }
        }
        carry = avail - framed;
        memmove(rbuf, rbuf + framed, carry);
        if (n <= 0) break;        // EOF: trailing carry is a partial frame, drop it
    }
    fclose(f);
    free(rbuf);

    // The file is fully READ, but a short recording fits entirely in the decode
    // queue + the 30 s PCM ring, so audio is still playing. Wait for the decoder
    // to drain the queue AND the ring to empty, so "end" means the speaker
    // actually finished — otherwise the UI clears "now playing" ~30 s early.
    while (!s_stop && (uxQueueMessagesWaiting(s_q) > 0 || audio_ring_bytes() > 4096))
        vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "playback: end");
    if (!s_stop && s_end_cb) s_end_cb();   // finished on its own (not stopped)

done:
    s_file_task = NULL;
    vTaskDelete(NULL);
}

static void start_file(const char *path)
{
    strncpy(s_file, path, sizeof(s_file) - 1);
    s_file[sizeof(s_file) - 1] = '\0';
    audio_resume();
    s_live = false;      // fetcher stays parked; the file player is the source
    s_stop = false;      // wakes the persistent decoder (fetcher idles on !s_live)
    // The file player is small (8 KB) — safe to create on demand even with the
    // SD card mounted, unlike the 16/20 KB fetcher/decoder (now persistent).
    xTaskCreatePinnedToCore(fileplayer_task, "fileplay", 8192, NULL, 5, &s_file_task, 0);
}

// ---- Internal (blocking) start/stop, run only from the control task ----
static void start_stream(const char *station_id)
{
    strncpy(s_station, station_id, sizeof(s_station) - 1);
    s_station[sizeof(s_station) - 1] = '\0';
    audio_resume();     // unmute, start from an empty buffer (fresh/live)
    s_live = true;      // fetcher pulls live segments
    s_stop = false;     // wakes the persistent fetcher + decoder (both boot tasks)
}

static void stop_stream(void)
{
    s_live = false;
    s_stop = true;      // idles the persistent fetcher + decoder
    audio_flush();   // instant silence + unblocks the decoder (so it idles fast)
    // Only the file player is a create/delete task now; wait for it to exit.
    // The fetcher + decoder are persistent (idled by the flags, not deleted).
    for (int i = 0; i < 600 && s_file_task; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (s_q) {
        seg_t seg;
        while (xQueueReceive(s_q, &seg, 0) == pdTRUE) free(seg.buf);  // drain leftovers
    }
}

// ---- Player-control task: serialises play/stop so the UI never blocks ----
// mode: 0 = stop, 1 = live station (arg=id), 2 = file playback (arg=path).
typedef struct { char arg[192]; uint8_t mode; } cmd_t;
static QueueHandle_t s_cmd_q = NULL;

static void ctrl_task(void *arg)
{
    static bool file_session;   // holds an SD mount while playing a recording
    cmd_t c;
    while (true) {
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE) continue;
        stop_stream();                                   // waits for the file player to exit
        if (file_session) { storage_release(); file_session = false; }   // safe: player gone
        if (c.mode == 1 && c.arg[0]) start_stream(c.arg);
        else if (c.mode == 2 && c.arg[0] && storage_acquire()) {
            file_session = true;
            start_file(c.arg);
        }
    }
}

void stream_control_start(void)
{
    if (!s_cmd_q) s_cmd_q = xQueueCreate(1, sizeof(cmd_t));   // depth 1: latest wins
    if (!s_q)     s_q     = xQueueCreate(SEG_QUEUE_DEPTH, sizeof(seg_t));
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
    // Create the fetcher + decoder ONCE, here at boot — their 16 KB / 20 KB
    // stacks are claimed while internal RAM is unfragmented, BEFORE the SD card
    // mounts (Phase 29). Call this before storage_init(). Both idle (s_stop)
    // until the first stream_play. Re-creating them per stream failed once SDMMC
    // had split the largest free block below their stack size.
    xTaskCreatePinnedToCore(fetcher_task, "seg_fetch", 16384, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(decoder_task, "seg_dec",   20480, NULL, 4, NULL, 0);
    xTaskCreate(ctrl_task, "stream_ctl", 3072, NULL, 6, NULL);
}

void stream_play(const char *station_id)
{
    cmd_t c = { .mode = 1 };
    strncpy(c.arg, station_id, sizeof(c.arg) - 1);
    xQueueOverwrite(s_cmd_q, &c);
}

void stream_play_file(const char *path)
{
    cmd_t c = { .mode = 2 };
    strncpy(c.arg, path, sizeof(c.arg) - 1);
    xQueueOverwrite(s_cmd_q, &c);
}

void stream_stop(void)
{
    cmd_t c = { .mode = 0 };
    xQueueOverwrite(s_cmd_q, &c);
}

void stream_on_playback_end(void (*cb)(void)) { s_end_cb = cb; }
