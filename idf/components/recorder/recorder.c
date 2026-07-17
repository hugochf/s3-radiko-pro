#include "recorder.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "storage.h"

static const char *TAG = "recorder";

#define Q_DEPTH   8              // segments buffered if the SD write stalls
#define REC_DIR   "/rec"         // under storage_root()
#define FSYNC_EVERY 10           // ~40 s: bound data loss on a power cut

// One command queue serialises ALL file I/O on the writer task, so the file is
// never touched from the UI task that calls start/stop.
typedef enum { CMD_OPEN, CMD_DATA, CMD_CLOSE } cmd_type_t;
typedef struct {
    cmd_type_t type;
    uint8_t   *buf;              // CMD_DATA: malloc'd copy the task frees
    int        len;
    char       station[24];      // CMD_OPEN
} rec_cmd_t;

static QueueHandle_t    s_q;
static volatile bool    s_active;
static volatile uint32_t s_bytes;
static time_t           s_start;

// Strip a leading ID3v2 tag; return offset of the ADTS payload (0 if none).
static int id3_skip(const uint8_t *b, int len)
{
    if (len > 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3') {
        int sz = (b[6] << 21) | (b[7] << 14) | (b[8] << 7) | b[9];  // syncsafe
        int off = 10 + sz;
        if (off > 0 && off < len) return off;
    }
    return 0;
}

static void writer_task(void *arg)
{
    FILE *f = NULL;
    bool mounted = false;
    int since_sync = 0;
    rec_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) != pdTRUE) continue;

        switch (cmd.type) {
        case CMD_OPEN: {
            mounted = storage_acquire();   // mount SD for the duration of this recording
            if (!mounted) { ESP_LOGE(TAG, "SD mount failed — recording aborted"); break; }
            char dir[64], path[128];
            snprintf(dir, sizeof(dir), "%s%s", storage_root(), REC_DIR);
            mkdir(dir, 0777);   // ignore EEXIST
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);   // JST (TZ set at boot)
            snprintf(path, sizeof(path), "%s/%s_%04d%02d%02d_%02d%02d%02d.aac",
                     dir, cmd.station, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
            if (f) fclose(f);
            f = fopen(path, "wb");
            since_sync = 0;
            if (f) ESP_LOGI(TAG, "recording -> %s", path);
            else   ESP_LOGE(TAG, "fopen failed: %s", path);
            break;
        }
        case CMD_DATA:
            if (f) {
                // Bounce through INTERNAL RAM. SDMMC DMA straight from a PSRAM
                // source is pathologically slow (~2.8 s for 31 KB, and it stalls
                // the bus enough to freeze the UI) — the bring-up hit 1 MB/s only
                // because it wrote from a static internal buffer. Copying 4 KB at
                // a time keeps the fwrite source internal and restores that speed.
                static uint8_t bounce[4096];   // .bss = internal RAM
                const uint8_t *src = cmd.buf + id3_skip(cmd.buf, cmd.len);
                int remain = cmd.len - id3_skip(cmd.buf, cmd.len);
                while (remain > 0) {
                    int chunk = remain < (int)sizeof(bounce) ? remain : (int)sizeof(bounce);
                    memcpy(bounce, src, chunk);
                    s_bytes += fwrite(bounce, 1, chunk, f);
                    src += chunk; remain -= chunk;
                }
                if (++since_sync >= FSYNC_EVERY) { fflush(f); fsync(fileno(f)); since_sync = 0; }
            }
            free(cmd.buf);
            break;
        case CMD_CLOSE:
            if (f) { fclose(f); f = NULL; ESP_LOGI(TAG, "recording closed (%u KB)",
                                                   (unsigned)(s_bytes / 1024)); }
            if (mounted) { storage_release(); mounted = false; }   // free SD RAM back to TLS
            break;
        }
    }
}

esp_err_t recorder_init(void)
{
    s_q = xQueueCreate(Q_DEPTH, sizeof(rec_cmd_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    // Core 0 (with the network/decoder); low priority — audio always wins, and
    // an SD stall here only backs up this queue.
    xTaskCreatePinnedToCore(writer_task, "rec_wr", 4096, NULL, 3, NULL, 0);
    return ESP_OK;
}

esp_err_t recorder_start(const char *station_id)
{
    if (!storage_ready()) { ESP_LOGW(TAG, "no SD — can't record"); return ESP_ERR_INVALID_STATE; }
    if (s_active) return ESP_OK;
    s_bytes = 0;
    s_start = time(NULL);
    s_active = true;   // set BEFORE enqueue so the fetcher starts feeding; the
                       // OPEN command is FIFO-ordered ahead of any DATA
    rec_cmd_t c = { .type = CMD_OPEN };
    strncpy(c.station, station_id ? station_id : "REC", sizeof(c.station) - 1);
    xQueueSend(s_q, &c, portMAX_DELAY);
    return ESP_OK;
}

void recorder_stop(void)
{
    if (!s_active) return;
    s_active = false;   // fetcher stops feeding; queued DATA still flushes first
    rec_cmd_t c = { .type = CMD_CLOSE };
    xQueueSend(s_q, &c, portMAX_DELAY);
}

bool     recorder_active(void) { return s_active; }
uint32_t recorder_secs(void)   { return s_active ? (uint32_t)(time(NULL) - s_start) : 0; }
uint32_t recorder_kb(void)     { return s_bytes / 1024; }

void recorder_feed(const void *seg, int len)
{
    if (!s_active || len <= 0) return;
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) return;
    memcpy(copy, seg, len);
    rec_cmd_t c = { .type = CMD_DATA, .buf = copy, .len = len };
    if (xQueueSend(s_q, &c, 0) != pdTRUE) {   // never block the fetcher
        free(copy);
        ESP_LOGW(TAG, "SD write behind — dropped a segment (recording gap)");
    }
}

// --- Recordings library ---
static bool is_aac(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcmp(n + l - 4, ".aac") == 0;
}

static int cmp_desc(const void *a, const void *b)   // newest first (timestamped names)
{
    return strcmp((const char *)b, (const char *)a);
}

void recorder_path(const char *name, char *out, int out_sz)
{
    snprintf(out, out_sz, "%s%s/%s", storage_root(), REC_DIR, name);
}

int recorder_list(char (*names)[REC_NAME_MAX], int max)
{
    if (!storage_acquire()) return 0;
    char dir[64];
    snprintf(dir, sizeof(dir), "%s%s", storage_root(), REC_DIR);
    DIR *d = opendir(dir);
    int n = 0;
    if (d) {
        struct dirent *e;
        while (n < max && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || !is_aac(e->d_name)) continue;
            strncpy(names[n], e->d_name, REC_NAME_MAX - 1);
            names[n][REC_NAME_MAX - 1] = '\0';
            n++;
        }
        closedir(d);
        qsort(names, n, REC_NAME_MAX, cmp_desc);
    }
    storage_release();
    return n;
}

int recorder_count(void)
{
    if (!storage_acquire()) return 0;
    char dir[64];
    snprintf(dir, sizeof(dir), "%s%s", storage_root(), REC_DIR);
    DIR *d = opendir(dir);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (e->d_name[0] != '.' && is_aac(e->d_name)) n++;
        closedir(d);
    }
    storage_release();
    return n;
}
