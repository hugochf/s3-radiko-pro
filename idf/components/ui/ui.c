#include "ui.h"

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "ui";

// Partial-render buffer height (lines). Two buffers -> render while one flushes.
#define LVGL_BUF_LINES 40

static lv_display_t     *s_disp  = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// LVGL tick source — milliseconds since boot from the high-res timer.
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Push a rendered area to the panel. LVGL emits native little-endian RGB565;
// this ILI9341 wants big-endian, so swap in place first (paired with BGR order
// set in display_init). flush-ready is signalled by color_done_cb below.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, w * h);
    esp_lcd_panel_draw_bitmap(display_panel(), area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

// Fires (in ISR context) when the SPI transfer for a flush completes.
static bool color_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    lv_display_flush_ready(s_disp);
    return false;
}

bool ui_lock(int timeout_ms)
{
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_mutex, ticks) == pdTRUE;
}

void ui_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    while (true) {
        uint32_t next_ms = 5;
        if (ui_lock(-1)) {
            next_ms = lv_timer_handler();
            ui_unlock();
        }
        if (next_ms < 5)   next_ms = 5;
        if (next_ms > 500) next_ms = 500;
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

// Phase 3 touch-test screen: shows the coordinates of each tap so we can verify
// touch registers and the orientation mapping is correct (tap top-left -> small
// x,y). Replaced by the real player UI in Phase 4.
static lv_obj_t *s_coord_lbl = NULL;
static int       s_taps      = 0;

// Live readout: updates continuously while pressed/dragged (LV_EVENT_PRESSING),
// so corners can be read precisely. Tap count bumps on each new press.
static void scr_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) s_taps++;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_label_set_text_fmt(s_coord_lbl, "x: %d   y: %d\n(taps: %d)",
                          (int)p.x, (int)p.y, s_taps);
}

static void build_demo_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A2E), 0);

    lv_obj_t *title = lv_label_create(scr);
    // Plain ASCII only — the built-in Montserrat font has no em-dash or CJK
    // glyphs (those render as tofu boxes). Custom fonts arrive in Phase 4.
    lv_label_set_text(title, "S3 Radiko - Touch test");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE94560), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Tap anywhere");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8888AA), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 60);

    s_coord_lbl = lv_label_create(scr);
    lv_label_set_text(s_coord_lbl, "x: --   y: --\n(taps: 0)");
    lv_obj_set_style_text_color(s_coord_lbl, lv_color_hex(0xEAEAEA), 0);
    lv_obj_set_style_text_align(s_coord_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_coord_lbl);

    lv_obj_add_event_cb(scr, scr_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, scr_touch_cb, LV_EVENT_PRESSING, NULL);
}

esp_err_t ui_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    lv_init();
    lv_tick_set_cb(tick_cb);

    size_t buf_bytes = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t);
    uint8_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    uint8_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    if (!buf1 || !buf2) return ESP_ERR_NO_MEM;

    s_disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Route the panel's transfer-done signal to LVGL flush-ready.
    display_register_flush_ready_cb(color_done_cb, NULL);

    build_demo_screen();

    // LVGL + display on core 1, leaving core 0 for network/audio later.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 6144, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "LVGL v%d.%d.%d up, %dx%d",
             lv_version_major(), lv_version_minor(), lv_version_patch(),
             DISPLAY_H_RES, DISPLAY_V_RES);
    return ESP_OK;
}
