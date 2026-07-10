#include "timesync.h"

#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "time";
static bool s_synced = false;

static void on_sync(struct timeval *tv)
{
    s_synced = true;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d JST",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void timesync_start(void)
{
    setenv("TZ", "JST-9", 1);   // Japan, no DST
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.nict.jp");     // Japan's national time server
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "time.google.com");
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP started (JST, 3 servers) — will sync when network is up");
}

bool timesync_valid(void)
{
    // Either the callback fired, or the RTC already holds a plausible time.
    return s_synced || time(NULL) > 1700000000;  // > 2023-11
}
