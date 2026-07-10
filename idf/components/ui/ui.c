#include "ui.h"

#include <string.h>
#include "audio.h"
#include "display.h"
#include "logos.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <time.h>
#include "lvgl.h"
#include "settings.h"
#include "stations.h"
#include "stream.h"
#include "timesync.h"
#include "wifi.h"

static const char *TAG = "ui";

#define LVGL_BUF_LINES 40

// Dark palette (ported from the Arduino build).
#define C_BG     0x1A1A2E
#define C_PANEL  0x16213E
#define C_ACCENT 0x0F3460
#define C_HL     0xE94560
#define C_TEXT   0xEAEAEA
#define C_DIM    0x8888AA
#define C_TRACK  0x2E2E5E

static lv_display_t     *s_disp  = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// ---- Player state (Phase 4: local only, no audio/network) ----
static int  s_cur     = 0;
static int  s_vol     = 20;
static bool s_playing = false;

// ---- Screens + widgets refreshed on state change ----
static lv_obj_t *s_scr_player = NULL;
static lv_obj_t *s_scr_list   = NULL;
static lv_obj_t *w_logo_tile  = NULL;
static lv_obj_t *w_logo_img   = NULL;
static lv_obj_t *w_name       = NULL;
static lv_obj_t *w_prog       = NULL;
static lv_obj_t *w_vol_slider = NULL;
static lv_obj_t *w_vol_val    = NULL;
static lv_obj_t *w_play       = NULL;
static lv_obj_t *w_wifi       = NULL;
static lv_obj_t *w_clock      = NULL;
static lv_obj_t *w_dots[NUM_STATIONS];

// =====================================================================
// LVGL <-> display glue
// =====================================================================
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, w * h);
    esp_lcd_panel_draw_bitmap(display_panel(), area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

// Signalled by the SPI DMA-done ISR; awaited by flush_wait_cb so the LVGL task
// sleeps (yielding the CPU) instead of busy-spinning while the panel transfers.
static SemaphoreHandle_t s_flush_sem = NULL;

static bool color_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t hp = pdFALSE;
    lv_display_flush_ready(s_disp);
    xSemaphoreGiveFromISR(s_flush_sem, &hp);
    return hp == pdTRUE;   // ask the ISR dispatcher to yield if we woke a task
}

// LVGL's default wait is a busy `while(disp->flushing)` spin that never yields —
// on an RTOS that starves the idle task (and the task watchdog) if a flush ever
// stalls. Block on the DMA-done semaphore instead. The timeout self-heals a lost
// completion: force the flag clear and drop the frame rather than hang forever.
static void flush_wait_cb(lv_display_t *disp)
{
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        lv_display_flush_ready(disp);   // completion missed — recover, don't wedge
    }
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

void ui_set_playing(bool playing)
{
    ui_lock(-1);
    s_playing = playing;
    if (w_play) lv_label_set_text(w_play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    ui_unlock();
}

const char *ui_current_station_id(void)
{
    int i = (s_cur >= 0 && s_cur < STATION_COUNT) ? s_cur : 0;
    return STATIONS[i].id;
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

// =====================================================================
// Small helpers
// =====================================================================
// Show a station logo in `img`, scaled to fit within box_w x box_h (never
// upscaled beyond 1x, to keep it crisp). Logos are embedded RGB565 (Phase 13).
static void set_logo(lv_obj_t *img, int station, int box_w, int box_h)
{
    const lv_image_dsc_t *L = STATION_LOGOS[station];
    lv_image_set_src(img, L);
    int sw = box_w * 256 / L->header.w;
    int sh = box_h * 256 / L->header.h;
    int sc = sw < sh ? sw : sh;
    if (sc > 256) sc = 256;
    lv_image_set_scale(img, sc);
    lv_obj_center(img);
}

static void refresh_player(void)
{
    const station_t *s = &STATIONS[s_cur];
    set_logo(w_logo_img, s_cur, 148, 58);
    lv_label_set_text(w_name, s->name);
    lv_label_set_text(w_prog, "- program info (Phase 14) -");
    lv_label_set_text(w_play, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_slider_set_value(w_vol_slider, s_vol, LV_ANIM_OFF);
    lv_label_set_text_fmt(w_vol_val, "%d", s_vol);
    for (int i = 0; i < STATION_COUNT; i++) {
        lv_obj_set_style_bg_color(w_dots[i],
            lv_color_hex(i == s_cur ? C_HL : 0x3A3A5A), 0);
    }
}

// =====================================================================
// Event callbacks
// =====================================================================
// Persist the fields the player can change. NVS writes are debounced to
// meaningful moments (station select, volume release), not every slider tick.
static void persist_station(void) { settings_get()->station = s_cur; settings_save(); }
static void persist_volume(void)  { settings_get()->volume  = s_vol; settings_save(); }

// Reflect s_playing/s_cur onto the actual stream (non-blocking requests).
static void apply_playback(void)
{
    if (s_playing) stream_play(STATIONS[s_cur].id);
    else           stream_stop();
}

// Debounced commit for prev/next. Mashing the buttons should only scrub the
// on-screen station; the flash write (NVS) and the real stream switch fire once,
// a beat after the user stops. This avoids hammering flash and restarting the
// decoder/HTTP pipeline on every press. Runs in the LVGL task (holds the lock).
static lv_timer_t *s_commit_timer = NULL;
static void commit_cb(lv_timer_t *t)
{
    persist_station();
    apply_playback();
    lv_timer_pause(t);
}
static void schedule_commit(void)
{
    if (!s_commit_timer) { persist_station(); apply_playback(); return; }
    lv_timer_reset(s_commit_timer);
    lv_timer_resume(s_commit_timer);
}

static void ev_open_list(lv_event_t *e) { lv_screen_load(s_scr_list); }
static void ev_back_to_player(lv_event_t *e) { lv_screen_load(s_scr_player); }
static void ev_open_wifi_setup(lv_event_t *e) { ui_show_wifi_setup(); }

static void ev_prev(lv_event_t *e)
{
    s_cur = (s_cur + STATION_COUNT - 1) % STATION_COUNT;
    refresh_player();
    schedule_commit();   // debounced: persist + switch stream once user settles
}
static void ev_next(lv_event_t *e)
{
    s_cur = (s_cur + 1) % STATION_COUNT;
    refresh_player();
    schedule_commit();
}
static void ev_play(lv_event_t *e)
{
    s_playing = !s_playing;
    refresh_player();
    apply_playback();
}
static void ev_vol(lv_event_t *e)   // live update while dragging
{
    s_vol = (int)lv_slider_get_value(w_vol_slider);
    lv_label_set_text_fmt(w_vol_val, "%d", s_vol);
    audio_set_volume(s_vol);        // apply to the ES8311 immediately
}
static void ev_vol_released(lv_event_t *e) { persist_volume(); }

static void ev_select_station(lv_event_t *e)
{
    s_cur = (int)(intptr_t)lv_event_get_user_data(e);
    s_playing = true;
    refresh_player();
    persist_station();
    apply_playback();
    lv_screen_load(s_scr_player);
}

// Poll WiFi state and reflect it in the status bar. Runs inside lv_timer_handler
// (LVGL task), so it already holds the LVGL lock. wifi_get_* are thread-safe.
static void status_timer_cb(lv_timer_t *t)
{
    if (!w_wifi) return;
    char buf[24];
    uint32_t color;
    switch (wifi_get_state()) {
        case WIFI_CONNECTED:
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %ddBm", wifi_get_rssi());
            color = 0x3AD07A;
            break;
        case WIFI_CONNECTING:
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " ...");
            color = 0xE0C040;
            break;
        default:
            snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " off");
            color = C_HL;
            break;
    }
    lv_label_set_text(w_wifi, buf);
    lv_obj_set_style_text_color(w_wifi, lv_color_hex(color), 0);

    if (w_clock) {
        if (timesync_valid()) {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            lv_label_set_text_fmt(w_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
        } else {
            lv_label_set_text(w_clock, "--:--");
        }
    }
}

// =====================================================================
// Player screen
// =====================================================================
static lv_obj_t *mk_circle_btn(lv_obj_t *parent, int size, int x, int y,
                               uint32_t bg, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_HL), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_player_screen(void)
{
    s_scr_player = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_player, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scr_player, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Status bar ----
    lv_obj_t *bar = lv_obj_create(s_scr_player);
    lv_obj_set_size(bar, 320, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi status — tappable to open the setup screen.
    lv_obj_t *wbtn = lv_button_create(bar);
    lv_obj_set_size(wbtn, 100, 22);
    lv_obj_align(wbtn, LV_ALIGN_LEFT_MID, -4, 0);
    lv_obj_set_style_bg_opa(wbtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(wbtn, 0, 0);
    lv_obj_add_event_cb(wbtn, ev_open_wifi_setup, LV_EVENT_CLICKED, NULL);
    w_wifi = lv_label_create(wbtn);
    lv_label_set_text(w_wifi, LV_SYMBOL_WIFI " ...");
    lv_obj_set_style_text_color(w_wifi, lv_color_hex(C_DIM), 0);
    lv_obj_align(w_wifi, LV_ALIGN_LEFT_MID, 0, 0);

    w_clock = lv_label_create(bar);
    lv_label_set_text(w_clock, "--:--");
    lv_obj_set_style_text_color(w_clock, lv_color_hex(C_TEXT), 0);
    lv_obj_align(w_clock, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *bat = lv_label_create(bar);
    lv_label_set_text(bat, LV_SYMBOL_BATTERY_FULL " 100%");
    lv_obj_set_style_text_color(bat, lv_color_hex(C_TEXT), 0);
    lv_obj_align(bat, LV_ALIGN_RIGHT_MID, -2, 0);

    // ---- Logo tile (tap to open station list) ----
    w_logo_tile = lv_obj_create(s_scr_player);
    lv_obj_set_size(w_logo_tile, 150, 60);
    lv_obj_align(w_logo_tile, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_clear_flag(w_logo_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w_logo_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w_logo_tile, ev_open_list, LV_EVENT_CLICKED, NULL);
    w_logo_img = lv_image_create(w_logo_tile);
    lv_obj_add_flag(w_logo_img, LV_OBJ_FLAG_EVENT_BUBBLE);   // taps reach the tile

    // ---- Station name (JP) ----
    w_name = lv_label_create(s_scr_player);
    lv_obj_set_style_text_font(w_name, &lv_font_jp_16, 0);
    lv_obj_set_style_text_color(w_name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(w_name, 300);
    lv_obj_set_style_text_align(w_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w_name, LV_ALIGN_TOP_MID, 0, 98);

    // ---- Program title (stub) ----
    w_prog = lv_label_create(s_scr_player);
    lv_obj_set_style_text_font(w_prog, &lv_font_jp_16, 0);
    lv_obj_set_style_text_color(w_prog, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(w_prog, 300);
    lv_label_set_long_mode(w_prog, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(w_prog, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w_prog, LV_ALIGN_TOP_MID, 0, 120);

    // ---- Volume row ----
    lv_obj_t *vlbl = lv_label_create(s_scr_player);
    lv_label_set_text(vlbl, "VOL");
    lv_obj_set_style_text_color(vlbl, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(vlbl, 12, 146);

    w_vol_slider = lv_slider_create(s_scr_player);
    lv_obj_set_size(w_vol_slider, 214, 8);
    lv_obj_set_pos(w_vol_slider, 52, 150);
    lv_slider_set_range(w_vol_slider, 0, 100);
    lv_obj_set_style_bg_color(w_vol_slider, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w_vol_slider, lv_color_hex(C_HL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(w_vol_slider, lv_color_hex(C_HL), LV_PART_KNOB);
    lv_obj_add_event_cb(w_vol_slider, ev_vol, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(w_vol_slider, ev_vol_released, LV_EVENT_RELEASED, NULL);

    w_vol_val = lv_label_create(s_scr_player);
    lv_obj_set_style_text_color(w_vol_val, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(w_vol_val, 286, 146);

    // ---- Transport buttons ----
    lv_obj_t *bp = mk_circle_btn(s_scr_player, 44, 40,  176, C_ACCENT, ev_prev);
    lv_obj_t *lp = lv_label_create(bp); lv_label_set_text(lp, LV_SYMBOL_PREV); lv_obj_center(lp);

    lv_obj_t *bplay = mk_circle_btn(s_scr_player, 56, 132, 170, C_HL, ev_play);
    w_play = lv_label_create(bplay); lv_obj_center(w_play);

    lv_obj_t *bn = mk_circle_btn(s_scr_player, 44, 236, 176, C_ACCENT, ev_next);
    lv_obj_t *ln = lv_label_create(bn); lv_label_set_text(ln, LV_SYMBOL_NEXT); lv_obj_center(ln);

    // ---- Position dots ----
    int dot_w = STATION_COUNT * 8 + (STATION_COUNT - 1) * 4;
    int dx = (320 - dot_w) / 2;
    for (int i = 0; i < STATION_COUNT; i++) {
        w_dots[i] = lv_obj_create(s_scr_player);
        lv_obj_set_size(w_dots[i], 8, 8);
        lv_obj_set_pos(w_dots[i], dx + i * 12, 230);
        lv_obj_set_style_radius(w_dots[i], 4, 0);
        lv_obj_set_style_border_width(w_dots[i], 0, 0);
        lv_obj_clear_flag(w_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    refresh_player();
}

// =====================================================================
// Station list screen
// =====================================================================
static void build_list_screen(void)
{
    s_scr_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_list, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scr_list, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *hdr = lv_obj_create(s_scr_list);
    lv_obj_set_size(hdr, 320, 30);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_size(back, 80, 24);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
    lv_obj_add_event_cb(back, ev_back_to_player, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, "Select Station");
    lv_obj_set_style_text_color(ttl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 24, 0);

    // Scrollable rows
    lv_obj_t *cont = lv_obj_create(s_scr_list);
    lv_obj_set_size(cont, 320, 210);
    lv_obj_set_pos(cont, 0, 30);
    lv_obj_set_style_bg_color(cont, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < STATION_COUNT; i++) {
        const station_t *s = &STATIONS[i];
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_size(row, 320, 54);
        lv_obj_set_style_bg_color(row, lv_color_hex(i & 1 ? C_PANEL : C_BG), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ev_select_station, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *logo = lv_image_create(row);
        lv_obj_align(logo, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_EVENT_BUBBLE);   // taps reach the row
        set_logo(logo, i, 110, 44);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s->name);
        lv_obj_set_style_text_font(nm, &lv_font_jp_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
        lv_obj_align(nm, LV_ALIGN_RIGHT_MID, -6, 0);
    }
}

// =====================================================================
// WiFi setup screen (scan -> select -> password -> connect)
// =====================================================================
static lv_obj_t *s_scr_wifi = NULL;
static lv_obj_t *s_wifi_list = NULL;
static lv_obj_t *s_wifi_ta   = NULL;
static lv_obj_t *s_wifi_kb   = NULL;
static char      s_sel_ssid[33];

static void ev_kb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {              // "OK" pressed
        wifi_connect_creds(s_sel_ssid, lv_textarea_get_text(s_wifi_ta));
        lv_screen_load(s_scr_player);
    } else if (code == LV_EVENT_CANCEL) {      // back to network list
        lv_obj_add_flag(s_wifi_ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ev_pick_ssid(lv_event_t *e)
{
    const wifi_ap_info_t *ap = lv_event_get_user_data(e);
    strncpy(s_sel_ssid, ap->ssid, sizeof(s_sel_ssid) - 1);
    s_sel_ssid[sizeof(s_sel_ssid) - 1] = '\0';
    if (!ap->secure) {                          // open network — connect now
        wifi_connect_creds(s_sel_ssid, "");
        lv_screen_load(s_scr_player);
        return;
    }
    lv_textarea_set_text(s_wifi_ta, "");
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_wifi_kb, s_wifi_ta);
}

// Scan runs OFF the LVGL thread (blocking), then repopulates the list.
static wifi_ap_info_t s_aps[20];   // persists so button user_data stays valid

static void scan_task(void *arg)
{
    int n = wifi_scan(s_aps, 20);

    ui_lock(-1);
    lv_obj_clean(s_wifi_list);
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_wifi_list);
        lv_label_set_text(l, "No networks found");
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_button_create(s_wifi_list);
        lv_obj_set_size(btn, 300, 34);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_PANEL), 0);
        lv_obj_add_event_cb(btn, ev_pick_ssid, LV_EVENT_CLICKED, &s_aps[i]);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text_fmt(l, "%s %s  (%d)", s_aps[i].secure ? LV_SYMBOL_CLOSE : LV_SYMBOL_OK,
                              s_aps[i].ssid, s_aps[i].rssi);
        lv_obj_center(l);
    }
    ui_unlock();
    vTaskDelete(NULL);
}

static void ev_wifi_back(lv_event_t *e) { lv_screen_load(s_scr_player); }

static void build_wifi_setup_screen(void)
{
    s_scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_wifi, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(s_scr_wifi);
    lv_obj_set_size(hdr, 320, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_size(back, 70, 22);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
    lv_obj_add_event_cb(back, ev_wifi_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, "WiFi Setup");
    lv_obj_set_style_text_color(ttl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 20, 0);

    s_wifi_list = lv_obj_create(s_scr_wifi);
    lv_obj_set_size(s_wifi_list, 314, 210);
    lv_obj_set_pos(s_wifi_list, 3, 28);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 4, 0);

    // Password entry (hidden until a secured network is picked)
    s_wifi_ta = lv_textarea_create(s_scr_wifi);
    lv_obj_set_size(s_wifi_ta, 300, 36);
    lv_obj_align(s_wifi_ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_one_line(s_wifi_ta, true);
    lv_textarea_set_placeholder_text(s_wifi_ta, "Password");
    lv_obj_add_flag(s_wifi_ta, LV_OBJ_FLAG_HIDDEN);

    s_wifi_kb = lv_keyboard_create(s_scr_wifi);
    lv_obj_add_event_cb(s_wifi_kb, ev_kb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_wifi_setup(void)
{
    ui_lock(-1);
    if (!s_scr_wifi) build_wifi_setup_screen();
    lv_obj_add_flag(s_wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_wifi_list);
    lv_obj_t *l = lv_label_create(s_wifi_list);
    lv_label_set_text(l, "Scanning...");
    lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
    lv_screen_load(s_scr_wifi);
    ui_unlock();

    xTaskCreate(scan_task, "wifiscan", 4096, NULL, 4, NULL);
}

// =====================================================================
esp_err_t ui_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    lv_init();
    lv_tick_set_cb(tick_cb);

    // Start from persisted settings (Phase 7).
    settings_t *st = settings_get();
    s_cur = (st->station >= 0 && st->station < STATION_COUNT) ? st->station : 0;
    s_vol = st->volume;

    // Draw buffers in PSRAM (not internal DMA RAM) — frees ~50 KB of scarce
    // internal RAM for TLS/WiFi. esp_lcd handles cache sync for PSRAM sources.
    size_t buf_bytes = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t);
    uint8_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    uint8_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) return ESP_ERR_NO_MEM;

    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) return ESP_ERR_NO_MEM;

    s_disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);  // yield, don't busy-spin
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    display_register_flush_ready_cb(color_done_cb, NULL);

    build_player_screen();
    build_list_screen();
    lv_screen_load(s_scr_player);

    lv_timer_create(status_timer_cb, 2000, NULL);  // WiFi/status bar refresh

    // One-shot debounce timer for prev/next (created paused; armed on each press).
    s_commit_timer = lv_timer_create(commit_cb, 450, NULL);
    lv_timer_pause(s_commit_timer);

    // 16 KB stack: LVGL v9's image draw/decode path (Phase 13 logos) is much
    // deeper than the label/tile path, and 8 KB could overflow while rendering
    // the 15-logo station list.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "LVGL v%d.%d.%d, %d stations, player UI up",
             lv_version_major(), lv_version_minor(), lv_version_patch(), STATION_COUNT);
    return ESP_OK;
}
