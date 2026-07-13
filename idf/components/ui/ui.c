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
#include "led.h"
#include "lvgl.h"
#include "radiko.h"
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
static lv_obj_t *w_logo_card  = NULL;   // white pad behind the player logo
static lv_obj_t *w_logo_img   = NULL;
static lv_obj_t *w_name       = NULL;
static lv_obj_t *w_prog       = NULL;
static lv_obj_t *w_vol_slider = NULL;
static lv_obj_t *w_vol_val    = NULL;
static lv_obj_t *w_play       = NULL;
static lv_obj_t *w_wifi       = NULL;
static lv_obj_t *w_clock      = NULL;
static lv_obj_t *w_dots[NUM_STATIONS];
static lv_obj_t *s_list_prog[NUM_STATIONS];   // per-row programme labels (list)

// =====================================================================
// LVGL <-> display glue
// =====================================================================
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Given by the SPI DMA-done ISR, awaited once per flush in flush_cb.
static SemaphoreHandle_t s_flush_sem = NULL;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, w * h);
    esp_lcd_panel_draw_bitmap(display_panel(), area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    // Block (yielding) until the DMA finishes, then hand the buffer back to LVGL.
    // Exactly one take per flush pairs with one give per completion — no desync,
    // no frame drops. Yielding (vs LVGL's default busy-spin) lets the lower-prio
    // audio decoder on this core run, so playback doesn't stutter. The timeout
    // self-heals a lost completion instead of hanging.
    xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(1000));
    lv_display_flush_ready(disp);
}

static bool color_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &hp);
    return hp == pdTRUE;   // ask the ISR dispatcher to yield if we woke a task
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
static int set_logo(lv_obj_t *img, int station, int box_w, int box_h, int max_scale)
{
    const lv_image_dsc_t *L = STATION_LOGOS[station];
    lv_image_set_src(img, L);
    int sw = box_w * 256 / L->header.w;
    int sh = box_h * 256 / L->header.h;
    int sc = sw < sh ? sw : sh;
    if (sc > max_scale) sc = max_scale;   // 256 = never upscale (list rows)
    // v9 transforms are visual-only AND shrinking the widget crops the source
    // before the transform. So: keep the widget at its natural size, scale
    // around the top-left pivot, and let callers align by the visual box
    // (top-left of the widget + w*sc/256 x h*sc/256).
    lv_image_set_pivot(img, 0, 0);
    lv_image_set_scale(img, sc);
    lv_obj_center(img);
    return sc;
}

// Show the cached "now on air" title for the current station (empty until the
// first program fetch lands). The label is a circular marquee for long titles.
static void set_program_label(void)
{
    char title[256];   // title + " / " + performers (pfm)
    radiko_program_title(STATIONS[s_cur].id, title, sizeof(title));
    lv_label_set_text(w_prog, title[0] ? title : "");
}

void ui_program_updated(void)
{
    ui_lock(-1);
    if (w_prog) set_program_label();
    // Refresh the station-list rows too (Arduino shows the programme per row).
    char buf[256];
    for (int i = 0; i < STATION_COUNT; i++) {
        if (!s_list_prog[i]) continue;
        radiko_program_title(STATIONS[i].id, buf, sizeof(buf));
        lv_label_set_text(s_list_prog[i], buf[0] ? buf : "---");
    }
    ui_unlock();
}

static void refresh_player(void)
{
    const station_t *s = &STATIONS[s_cur];
    const lv_image_dsc_t *L = STATION_LOGOS[s_cur];
    int sc = set_logo(w_logo_img, s_cur, 300, 56, 512);
    lv_obj_set_size(w_logo_card,
                    L->header.w * sc / 256 + 12, L->header.h * sc / 256 + 8);
    lv_obj_center(w_logo_card);
    lv_obj_center(w_logo_img);
    lv_label_set_text(w_name, s->name);
    set_program_label();
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

// ---- Sleep timer (Arduino bell button): cycles OFF/30/60/90 min; when it
// fires, playback stops. Runs as a paused LVGL timer armed on selection.
static lv_obj_t  *s_sleep_lbl  = NULL;
static lv_obj_t  *s_led_lbl    = NULL;
static lv_timer_t *s_sleep_timer = NULL;
static int        s_sleep_idx  = 0;
static const int  SLEEP_MIN[]  = { 0, 30, 60, 90 };

static void sleep_fire_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    s_sleep_idx = 0;
    s_playing = false;
    refresh_player();
    apply_playback();
}
static void ev_sleep_toggle(lv_event_t *e)
{
    s_sleep_idx = (s_sleep_idx + 1) % 4;
    int min = SLEEP_MIN[s_sleep_idx];
    if (s_sleep_timer) {
        if (min == 0) lv_timer_pause(s_sleep_timer);
        else {
            lv_timer_set_period(s_sleep_timer, (uint32_t)min * 60 * 1000);
            lv_timer_reset(s_sleep_timer);
            lv_timer_resume(s_sleep_timer);
        }
    }
    char buf[24];
    if (min) snprintf(buf, sizeof(buf), "Sleep: %d min", min);
    else     snprintf(buf, sizeof(buf), "Sleep: off");
    lv_label_set_text(w_prog, buf);   // transient, like the Arduino build
}

// ---- RGB LED mode cycle (Arduino eye button) ----
static void ev_led_toggle(lv_event_t *e)
{
    const char *name = led_cycle_mode();
    lv_label_set_text(s_led_lbl,
        led_mode() == LED_MODES - 1 ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    lv_label_set_text(w_prog, name);  // transient, like the Arduino build
}

static void ev_open_list(lv_event_t *e) { lv_screen_load(s_scr_list); }

// Swipe/tap gestures on the full-width logo strip (Arduino behaviour):
//   dx >= +25 → prev station, dx <= -25 → next station,
//   |dx| < 8 AND released in the centre region (x 120..200) → open station list,
//   anything else → dead-zone (prevents accidental list opens during near-swipes).
static void ev_prev(lv_event_t *e);
static void ev_next(lv_event_t *e);
static int32_t s_press_x;
static void ev_logo_pressed(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    s_press_x = p.x;
}
static void ev_logo_released(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int dx = p.x - s_press_x;
    if      (dx >= 25) ev_prev(e);   // swipe right → previous station
    else if (dx <= -25) ev_next(e);  // swipe left  → next station
    else if (dx > -8 && dx < 8 && p.x >= 120 && p.x <= 200) ev_open_list(e);
}
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
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI);   // icon only (Arduino bar)
            (void)wifi_get_rssi();
            color = 0x3AD07A;
            break;
        case WIFI_CONNECTING:
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI);
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
    lv_obj_set_size(wbtn, 86, 22);
    lv_obj_align(wbtn, LV_ALIGN_LEFT_MID, -4, 0);
    lv_obj_set_style_bg_opa(wbtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(wbtn, 0, 0);
    lv_obj_add_event_cb(wbtn, ev_open_wifi_setup, LV_EVENT_CLICKED, NULL);
    w_wifi = lv_label_create(wbtn);
    lv_label_set_text(w_wifi, LV_SYMBOL_WIFI " ...");
    lv_obj_set_style_text_color(w_wifi, lv_color_hex(C_DIM), 0);
    lv_obj_align(w_wifi, LV_ALIGN_LEFT_MID, 0, 0);

    // Clock inside the same tappable button, right of the icon (Arduino).
    w_clock = lv_label_create(wbtn);
    lv_label_set_text(w_clock, "--:--");
    lv_obj_set_style_text_color(w_clock, lv_color_hex(C_TEXT), 0);
    lv_obj_align(w_clock, LV_ALIGN_RIGHT_MID, -2, 0);

    lv_obj_t *hdr_lbl = lv_label_create(bar);
    lv_label_set_text(hdr_lbl, "Radiko Radio");
    lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(hdr_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *bat = lv_label_create(bar);
    lv_label_set_text(bat, LV_SYMBOL_BATTERY_FULL " 100%");
    lv_obj_set_style_text_color(bat, lv_color_hex(C_TEXT), 0);
    lv_obj_align(bat, LV_ALIGN_RIGHT_MID, -2, 0);

    // ---- Station logo (full width, Arduino layout) ----
    // Swipe left/right anywhere on the strip = next/prev; a small tap in the
    // centre region opens the station list (see ev_logo_pressed/released).
    w_logo_tile = lv_obj_create(s_scr_player);
    lv_obj_set_size(w_logo_tile, 320, 68);
    lv_obj_set_pos(w_logo_tile, 0, 28);
    lv_obj_set_style_bg_opa(w_logo_tile, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w_logo_tile, 0, 0);
    lv_obj_set_style_pad_all(w_logo_tile, 0, 0);
    lv_obj_clear_flag(w_logo_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w_logo_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w_logo_tile, ev_logo_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(w_logo_tile, ev_logo_released, LV_EVENT_RELEASED, NULL);
    // White card behind the logo (small padding); events bubble to the strip
    // so swipes/taps still work anywhere on it.
    w_logo_card = lv_obj_create(w_logo_tile);
    lv_obj_set_style_bg_color(w_logo_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(w_logo_card, 0, 0);
    lv_obj_set_style_radius(w_logo_card, 6, 0);
    lv_obj_set_style_pad_all(w_logo_card, 0, 0);
    lv_obj_clear_flag(w_logo_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w_logo_card, LV_OBJ_FLAG_EVENT_BUBBLE);
    w_logo_img = lv_image_create(w_logo_card);
    lv_obj_add_flag(w_logo_img, LV_OBJ_FLAG_EVENT_BUBBLE);   // taps reach the tile

    // ---- Station name (JP) ----
    w_name = lv_label_create(s_scr_player);
    lv_obj_set_style_text_font(w_name, &lv_font_jp_16, 0);
    lv_obj_set_style_text_color(w_name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_size(w_name, 300, 23);   // one line, DOT if too long
    lv_label_set_long_mode(w_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(w_name, LV_TEXT_ALIGN_CENTER, 0);
    // Full Noto JP line height is ~23 px: name occupies ~94..117, title ~122..145,
    // volume row starts at 146 — no overlap (name at 98 used to collide with the
    // title at 120, which clipped/hid the title's upper part).
    lv_obj_align(w_name, LV_ALIGN_TOP_MID, 0, 91);

    // ---- Program title ("now on air") ----
    // Circular marquee for long titles (LVGL only animates when the text
    // overflows). Affordable only because LVGL owns core 1 outright — when the
    // audio decoder shared this core, the per-frame marquee re-render starved it
    // and playback died. If audio ever stutters again, suspect this first.
    w_prog = lv_label_create(s_scr_player);
    lv_obj_set_style_text_font(w_prog, &lv_font_jp_16, 0);
    lv_obj_set_style_text_color(w_prog, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(w_prog, 300);
    lv_label_set_long_mode(w_prog, LV_LABEL_LONG_SCROLL_CIRCULAR);
    // Slow crawl (Arduino used anim_speed 8 px/s): one full scroll cycle takes
    // this long regardless of text length. Also keeps the render load tiny.
    lv_obj_set_style_anim_duration(w_prog, 30000, 0);
    lv_obj_set_style_text_align(w_prog, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w_prog, LV_ALIGN_TOP_MID, 0, 112);

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
    lv_obj_t *bp = mk_circle_btn(s_scr_player, 44, 16,  176, C_ACCENT, ev_prev);
    lv_obj_t *lp = lv_label_create(bp); lv_label_set_text(lp, LV_SYMBOL_PREV); lv_obj_center(lp);

    lv_obj_t *bplay = mk_circle_btn(s_scr_player, 56, 90, 170, C_HL, ev_play);
    w_play = lv_label_create(bplay); lv_obj_center(w_play);

    lv_obj_t *bn = mk_circle_btn(s_scr_player, 44, 176, 176, C_ACCENT, ev_next);
    lv_obj_t *ln = lv_label_create(bn); lv_label_set_text(ln, LV_SYMBOL_NEXT); lv_obj_center(ln);

    lv_obj_t *bs = mk_circle_btn(s_scr_player, 34, 244, 181, C_ACCENT, ev_sleep_toggle);
    s_sleep_lbl = lv_label_create(bs);
    lv_label_set_text(s_sleep_lbl, LV_SYMBOL_BELL);
    lv_obj_center(s_sleep_lbl);

    lv_obj_t *bl2 = mk_circle_btn(s_scr_player, 34, 282, 181, C_ACCENT, ev_led_toggle);
    s_led_lbl = lv_label_create(bl2);
    lv_label_set_text(s_led_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(s_led_lbl);

    // ---- Position dots ----
    int dot_w = STATION_COUNT * 8 + (STATION_COUNT - 1) * 4;
    int dx = (320 - dot_w) / 2;
    for (int i = 0; i < STATION_COUNT; i++) {
        w_dots[i] = lv_obj_create(s_scr_player);
        lv_obj_set_size(w_dots[i], 8, 8);
        lv_obj_set_pos(w_dots[i], dx + i * 12, 228);
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
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ev_select_station, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Arduino row layout: logo at left; name top-right; programme bottom-right.
        lv_obj_t *logo = lv_image_create(row);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_EVENT_BUBBLE);   // taps reach the row
        int lsc = set_logo(logo, i, 138, 38, 256);   // height-bound: ~38px rows
        int vh  = STATION_LOGOS[i]->header.h * lsc / 256;
        // Visual pixels hang off the widget's top-left (pivot 0,0), so align
        // the top-left and centre the *visual* height in the 54px row.
        lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 4, (54 - vh) / 2);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s->name);
        lv_obj_set_style_text_font(nm, &lv_font_jp_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
        lv_obj_set_size(nm, 168, 22);   // fixed height: DOT, not wrap
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_RIGHT, -6, 4);

        s_list_prog[i] = lv_label_create(row);
        lv_label_set_text(s_list_prog[i], "---");
        lv_obj_set_style_text_font(s_list_prog[i], &lv_font_jp_16, 0);
        lv_obj_set_style_text_color(s_list_prog[i], lv_color_hex(C_DIM), 0);
        lv_obj_set_size(s_list_prog[i], 168, 22);
        lv_label_set_long_mode(s_list_prog[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(s_list_prog[i], 30000, 0);   // slow crawl
        lv_obj_set_style_text_align(s_list_prog[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_list_prog[i], LV_ALIGN_BOTTOM_RIGHT, -6, -4);
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
    lv_display_set_flush_cb(s_disp, flush_cb);   // flush_cb blocks on the DMA itself
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    display_register_flush_ready_cb(color_done_cb, NULL);

    build_player_screen();
    build_list_screen();
    lv_screen_load(s_scr_player);

    lv_timer_create(status_timer_cb, 2000, NULL);  // WiFi/status bar refresh

    // One-shot debounce timer for prev/next (created paused; armed on each press).
    s_commit_timer = lv_timer_create(commit_cb, 450, NULL);
    lv_timer_pause(s_commit_timer);

    s_sleep_timer = lv_timer_create(sleep_fire_cb, 60000, NULL);
    lv_timer_pause(s_sleep_timer);

    // 16 KB stack: LVGL v9's image draw/decode path (Phase 13 logos) is much
    // deeper than the label/tile path, and 8 KB could overflow while rendering
    // the 15-logo station list.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "LVGL v%d.%d.%d, %d stations, player UI up",
             lv_version_major(), lv_version_minor(), lv_version_patch(), STATION_COUNT);
    return ESP_OK;
}
