#include "storage.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

// Dedicated SDMMC pins for the ES3C28P slot (ngttai BSP; verified on-device).
#define PIN_CLK 38
#define PIN_CMD 40
#define PIN_D0  39
#define PIN_D1  41
#define PIN_D2  48
#define PIN_D3  47

#define MOUNT "/sdcard"

// The SD card is mounted LAZILY, only while recording or browsing recordings —
// NOT during normal streaming. Reason: the SDMMC/FATFS mount holds ~12 KB of
// internal RAM, and internal RAM is so tight that keeping it mounted starved the
// TLS handshake (esp-tls write error, no audio). Unmounted, the radio has room
// to (re-)authenticate; recording uses the already-warm keep-alive connection.
// Refcounted so an overlapping record + browse don't unmount each other.
static sdmmc_card_t     *s_card;
static int               s_refs;
static bool              s_present;    // a card was detected at boot probe
static uint32_t          s_total_mb, s_free_mb;
static SemaphoreHandle_t s_lock;

static esp_err_t do_mount_inner(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_CLK;  slot.cmd = PIN_CMD;
    slot.d0  = PIN_D0;   slot.d1  = PIN_D1;
    slot.d2  = PIN_D2;   slot.d3  = PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) { s_card = NULL; return err; }

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &freeb) == ESP_OK) {
        s_total_mb = (uint32_t)(total / (1024 * 1024));
        s_free_mb  = (uint32_t)(freeb / (1024 * 1024));
    }
    return ESP_OK;
}

static void do_unmount_inner(void)
{
    if (s_card) { esp_vfs_fat_sdcard_unmount(MOUNT, s_card); s_card = NULL; }
}

// BOTH the SDMMC/FATFS mount AND unmount are DEEP call chains that overflowed the
// 3 KB stream_ctl stack (Phase 29 crashes — a corrupted backtrace in the
// scheduler). Rather than permanently enlarging the audio tasks' stacks (which
// starved the TLS handshake of internal RAM → "no sound"), run mount/unmount on a
// transient helper task with a generous stack, deleted the instant it returns.
// Its stack is reclaimed immediately, so the only lasting cost is the ~12 KB the
// mounted volume holds. op: 0 = mount, 1 = unmount.
static struct { esp_err_t err; int op; SemaphoreHandle_t done; } s_io;

static void sd_helper(void *arg)
{
    if (s_io.op == 0) s_io.err = do_mount_inner();
    else              { do_unmount_inner(); s_io.err = ESP_OK; }
    xSemaphoreGive(s_io.done);
    vTaskDelete(NULL);
}

static esp_err_t sd_run(int op)
{
    s_io.op   = op;
    s_io.done = xSemaphoreCreateBinary();
    if (!s_io.done ||
        xTaskCreate(sd_helper, "sd_io", 6144, NULL, 5, NULL) != pdPASS) {
        if (s_io.done) vSemaphoreDelete(s_io.done);
        if (op == 0) return do_mount_inner();   // fallback: run inline
        do_unmount_inner();
        return ESP_OK;
    }
    xSemaphoreTake(s_io.done, portMAX_DELAY);
    vSemaphoreDelete(s_io.done);
    return s_io.err;
}

static esp_err_t do_mount(void) { return sd_run(0); }

esp_err_t storage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    // Probe once: mount to confirm a card is present and read its size, then
    // UNMOUNT so streaming has the internal RAM back.
    esp_err_t err = do_mount();
    if (err == ESP_OK) {
        s_present = true;
        ESP_LOGI(TAG, "SD present: %s, %lu MB (%lu MB free) — mounted on demand",
                 s_card->cid.name, (unsigned long)s_total_mb, (unsigned long)s_free_mb);
        sd_run(1);   // unmount off this stack (deep FATFS call chain)
    } else {
        ESP_LOGW(TAG, "no SD storage: %s. Recording unavailable.", esp_err_to_name(err));
    }
    return err;
}

bool storage_acquire(void)
{
    if (!s_present || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = true;
    if (s_refs == 0) ok = (do_mount() == ESP_OK);
    if (ok) s_refs++;
    xSemaphoreGive(s_lock);
    if (!ok) ESP_LOGW(TAG, "SD remount failed");
    return ok;
}

void storage_release(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refs > 0 && --s_refs == 0 && s_card) {
        sd_run(1);   // unmount off the caller's stack (deep FATFS call chain)
    }
    xSemaphoreGive(s_lock);
}

bool        storage_ready(void)     { return s_present; }   // card detected at boot
const char *storage_root(void)      { return MOUNT; }
uint32_t    storage_total_mb(void)  { return s_total_mb; }
uint32_t    storage_free_mb(void)   { return s_free_mb; }
