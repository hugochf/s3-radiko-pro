#include "stream.h"

#include <stdio.h>
#include <string.h>
#include "aacdec.h"
#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "httpc.h"
#include "radiko.h"

static const char *TAG = "stream";

#define STREAM_INFO_FMT "https://radiko.jp/v3/station/stream/pc_html5/%s.xml"
#define SEG_BUF_BYTES   (128 * 1024)   // one AAC segment (PSRAM)
#define PLIST_BYTES     4096

static TaskHandle_t     s_task = NULL;
static volatile bool    s_stop = false;
static char             s_station[16];

// ---- helpers ----
static void make_lsid(char *out /* [33] */)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", rnd[i]);
    out[32] = '\0';
}

static int fetch(const char *url, const char *token, char *buf, size_t cap)
{
    http_header_t h[1];
    int nh = 0;
    if (token && token[0]) { h[0].name = "X-Radiko-AuthToken"; h[0].value = token; nh = 1; }
    http_req_t r = {
        .url = url, .method = HTTP_METHOD_GET,
        .req_headers = h, .n_req_headers = nh,
        .body = buf, .body_cap = cap,
    };
    if (httpc_do(&r) != ESP_OK) return -1;
    return r.status == 200 ? (int)r.body_len : -r.status;
}

// First non-'#', non-empty line of an m3u8 (a URL). Copies into out.
static bool first_url_line(const char *body, char *out, size_t cap)
{
    const char *p = body;
    while (*p) {
        const char *nl = strpbrk(p, "\r\n");
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[0] != '#') {
            if (len >= cap) len = cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

// Resolve the media URL, playing new segments. Returns false if it should be
// re-resolved (session expired). last_seg tracks what we've already played.
static bool play_media(const char *media_url, const char *token,
                       AACFrameInfo *fi_out, char *seg_buf, char *plist,
                       char *last_seg, size_t last_seg_cap, HAACDecoder dec)
{
    int len = fetch(media_url, token, plist, PLIST_BYTES);
    if (len <= 0) {
        ESP_LOGW(TAG, "media playlist fetch -> %d", len);
        return false;
    }

    // Walk segment URLs; play those after the last one we played.
    bool seen_last = (last_seg[0] == '\0');
    int played = 0;
    char *save = NULL;
    char *dup = plist;
    for (char *line = strtok_r(dup, "\r\n", &save); line && !s_stop;
         line = strtok_r(NULL, "\r\n", &save)) {
        if (!line[0] || line[0] == '#') continue;

        if (!seen_last) {
            if (strcmp(line, last_seg) == 0) seen_last = true;
            continue;   // skip up to and including the last played
        }

        // fetch segment
        int slen = fetch(line, token, seg_buf, SEG_BUF_BYTES);
        if (slen <= 0) { ESP_LOGW(TAG, "seg fetch -> %d", slen); continue; }

        // decode ADTS AAC -> PCM -> audio
        unsigned char *inp = (unsigned char *)seg_buf;
        int left = slen;
        static int16_t pcm[2048 * 2];
        int frames = 0;
        while (left > 0 && !s_stop) {
            int off = AACFindSyncWord(inp, left);
            if (off < 0) break;
            inp += off; left -= off;
            int err = AACDecode(dec, &inp, &left, pcm);
            if (err) { inp++; left--; continue; }   // resync
            AACGetLastFrameInfo(dec, fi_out);
            if (frames == 0 && played == 0) {
                ESP_LOGI(TAG, "decode: %d Hz, %d ch, %d samp/frame",
                         fi_out->sampRateOut, fi_out->nChans, fi_out->outputSamps);
            }
            audio_write(pcm, fi_out->outputSamps * sizeof(int16_t), NULL);
            frames++;
        }
        strncpy(last_seg, line, last_seg_cap - 1);
        last_seg[last_seg_cap - 1] = '\0';
        played++;
    }
    if (!seen_last) last_seg[0] = '\0';   // fell off the window — resync next time
    return true;
}

static void stream_task(void *arg)
{
    char *seg_buf = heap_caps_malloc(SEG_BUF_BYTES, MALLOC_CAP_SPIRAM);
    char *plist   = heap_caps_malloc(PLIST_BYTES, MALLOC_CAP_SPIRAM);
    HAACDecoder dec = AACInitDecoder();
    if (!seg_buf || !plist || !dec) { ESP_LOGE(TAG, "alloc/decoder failed"); goto done; }

    const char *token = radiko_token();
    char base[160] = {0}, media_url[400] = {0}, last_seg[256] = {0};
    AACFrameInfo fi = {0};

    // stream-info -> playlist base URL
    char info_url[160];
    snprintf(info_url, sizeof(info_url), STREAM_INFO_FMT, s_station);
    if (fetch(info_url, NULL, plist, PLIST_BYTES) <= 0) { ESP_LOGE(TAG, "stream-info failed"); goto done; }
    {
        char *p = strstr(plist, "<playlist_create_url>");
        char *e = p ? strstr(p, "</playlist_create_url>") : NULL;
        if (!p || !e) { ESP_LOGE(TAG, "no playlist_create_url"); goto done; }
        p += strlen("<playlist_create_url>");
        size_t n = (size_t)(e - p); if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, p, n); base[n] = '\0';
    }
    ESP_LOGI(TAG, "streaming %s (base %s)", s_station, base);

    while (!s_stop) {
        if (media_url[0] == '\0') {
            // master playlist -> media playlist URL
            char lsid[33]; make_lsid(lsid);
            char purl[320];
            snprintf(purl, sizeof(purl), "%s?station_id=%s&l=15&lsid=%s&type=b",
                     base, s_station, lsid);
            if (fetch(purl, token, plist, PLIST_BYTES) <= 0 ||
                !first_url_line(plist, media_url, sizeof(media_url))) {
                ESP_LOGW(TAG, "master playlist failed; retry");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            last_seg[0] = '\0';
        }

        if (!play_media(media_url, token, &fi, seg_buf, plist,
                        last_seg, sizeof(last_seg), dec)) {
            media_url[0] = '\0';   // re-resolve
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));   // ~half the target duration
    }

done:
    if (dec) AACFreeDecoder(dec);
    free(seg_buf); free(plist);
    ESP_LOGI(TAG, "stream task exit");
    s_task = NULL;
    vTaskDelete(NULL);
}

void stream_play(const char *station_id)
{
    stream_stop();
    strncpy(s_station, station_id, sizeof(s_station) - 1);
    s_station[sizeof(s_station) - 1] = '\0';
    s_stop = false;
    xTaskCreatePinnedToCore(stream_task, "stream", 20480, NULL, 5, &s_task, 0);
}

void stream_stop(void)
{
    s_stop = true;
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}
