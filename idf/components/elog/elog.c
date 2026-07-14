#include "elog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "elog";

#define RAM_CAP        2048   // staged lines waiting for the next flash flush
#define FLUSH_MS       10000  // flush cadence when anything is pending
#define ELOG_LINE_MAX       200    // per-line cap (long lines truncated)
#define SECTOR         4096

static const esp_partition_t *s_part;
static vprintf_like_t         s_orig;
static SemaphoreHandle_t      s_mux;      // guards the RAM buffer only
static char                   s_ram[RAM_CAP];
static size_t                 s_ram_len;
static size_t                 s_head;     // next byte to write in the ring

// ---- capture side (runs in whichever task logged) ----
static void buffer_line(const char *line, size_t len)
{
    if (len > ELOG_LINE_MAX) len = ELOG_LINE_MAX;
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_ram_len + len + 1 <= RAM_CAP) {   // drop when staging is full: the
        memcpy(s_ram + s_ram_len, line, len);   // log must never block or grow
        s_ram_len += len;
        if (s_ram[s_ram_len - 1] != '\n') s_ram[s_ram_len++] = '\n';
    }
    xSemaphoreGive(s_mux);
}

static int log_hook(const char *fmt, va_list ap)
{
    // Format privately first — the original vprintf consumes the va_list.
    char line[ELOG_LINE_MAX + 1];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);

    int r = s_orig ? s_orig(fmt, ap) : n;   // console sees everything, always

    // Keep only warnings/errors. Skip the ANSI colour prefix esp_log adds.
    const char *p = line;
    if (p[0] == '\033') {
        const char *m = strchr(p, 'm');
        if (m) p = m + 1;
    }
    if ((p[0] == 'W' || p[0] == 'E') && p[1] == ' ' && p[2] == '(') {
        size_t len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' ||
                       (p[len - 1] == '\033') || p[len - 1] == 'm')) {
            // strip trailing newline and the ANSI reset suffix "\033[0m"
            if (p[len - 1] == 'm' && len >= 4 && p[len - 4] == '\033') len -= 4;
            else if (p[len - 1] == '\033') len -= 1;
            else len -= 1;
        }
        if (len) buffer_line(p, len);
    }
    return r;
}

// ---- flash side (flush task only) ----
static void ring_write(const char *data, size_t len)
{
    while (len) {
        if (s_head >= s_part->size) s_head = 0;   // wrap
        // Entering a fresh sector: erase it first (this is the ring's only
        // erase — one per 4 KB of logged text, so wear is a non-issue).
        if (s_head % SECTOR == 0) {
            esp_partition_erase_range(s_part, s_head, SECTOR);
        }
        size_t space = SECTOR - (s_head % SECTOR);
        size_t n = len < space ? len : space;
        esp_partition_write(s_part, s_head, data, n);
        s_head += n;
        data += n;
        len -= n;
    }
}

static void flush_task(void *arg)
{
    static char out[RAM_CAP];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));
        size_t n = 0;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_ram_len) {
            n = s_ram_len;
            memcpy(out, s_ram, n);
            s_ram_len = 0;
        }
        xSemaphoreGive(s_mux);
        if (n) ring_write(out, n);   // flash I/O outside the capture lock
    }
}

// Head = first 0xFF after written data. Scanning the raw 64 KB takes ~10 ms
// once at boot and avoids any index metadata that could itself corrupt.
// Scratch comes from the heap and is freed — internal RAM is too scarce for
// a permanent 4 KB .bss buffer that's used once.
static void find_head(void)
{
    char *sect = malloc(SECTOR);
    s_head = 0;
    if (!sect) return;
    for (size_t off = 0; off < s_part->size; off += SECTOR) {
        esp_partition_read(s_part, off, sect, SECTOR);
        for (size_t i = 0; i < SECTOR; i++) {
            if ((unsigned char)sect[i] == 0xFF) {
                if (off == 0 && i == 0) { free(sect); s_head = 0; return; }
                s_head = off + i;
                free(sect);
                return;
            }
        }
    }
    free(sect);
    s_head = 0;   // completely full: continue at 0 (oldest sector erased next)
}

esp_err_t elog_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "elog");
    if (!s_part) {
        ESP_LOGW(TAG, "no 'elog' partition — persistent log disabled");
        return ESP_ERR_NOT_FOUND;
    }
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;

    find_head();

    // Boot marker straight into the staging buffer (flushed with the rest).
    const esp_app_desc_t *app = esp_app_get_description();
    char b[128];
    int n = snprintf(b, sizeof(b), "B (boot) reset=%d ver=%s built=%s %s",
                     (int)esp_reset_reason(), app->version, app->date, app->time);
    buffer_line(b, (size_t)n);

    xTaskCreate(flush_task, "elog", 3072, NULL, 1, NULL);
    s_orig = esp_log_set_vprintf(log_hook);   // returns the previous vprintf

    ESP_LOGI(TAG, "ring ready: %u KB at 0x%06x, head=%u",
             (unsigned)(s_part->size / 1024), (unsigned)s_part->address,
             (unsigned)s_head);
    return ESP_OK;
}
