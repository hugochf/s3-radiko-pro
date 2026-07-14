#include "crashlog.h"

#include <stdio.h>
#include "esp_core_dump.h"
#include "esp_log.h"

static const char *TAG = "crashlog";

static char s_last[96] = "none";

void crashlog_check(void)
{
    if (esp_core_dump_image_check() != ESP_OK) {
        ESP_LOGI(TAG, "no coredump stored — last shutdown was clean");
        return;
    }

    // Summary decode needs the ELF-format dump (sdkconfig) and gives us the
    // faulting task + PC + raw backtrace without any host tooling.
    static esp_core_dump_summary_t sum;   // ~200 B; static spares main's stack
    if (esp_core_dump_get_summary(&sum) != ESP_OK) {
        snprintf(s_last, sizeof(s_last), "stored (summary unreadable)");
        ESP_LOGW(TAG, "coredump present but summary decode failed");
        return;
    }

    snprintf(s_last, sizeof(s_last), "%s @ 0x%08lx",
             sum.exc_task, (unsigned long)sum.exc_pc);
    ESP_LOGW(TAG, "PREVIOUS BOOT CRASHED: task '%s', PC 0x%08lx",
             sum.exc_task, (unsigned long)sum.exc_pc);

    // Backtrace as one addr2line-ready line:
    //   xtensa-esp32s3-elf-addr2line -pfiaC -e build/s3_radiko_pro.elf <addrs>
    char bt[16 * 11 + 1] = "";
    int  pos = 0;
    for (int i = 0; i < sum.exc_bt_info.depth && i < 16; i++) {
        pos += snprintf(bt + pos, sizeof(bt) - pos, "0x%08lx ",
                        (unsigned long)sum.exc_bt_info.bt[i]);
    }
    ESP_LOGW(TAG, "backtrace%s: %s",
             sum.exc_bt_info.corrupted ? " (CORRUPTED)" : "", bt);
    ESP_LOGW(TAG, "full dump kept in flash — `idf.py coredump-info` to inspect");
}

const char *crashlog_last(void)
{
    return s_last;
}
