#include "ui.h"

#include <string.h>
#include "app_watchdog.h"
#include "audio.h"
#include "battery.h"
#include "crashlog.h"
#include "display.h"
#include "logos.h"
#include "esp_app_desc.h"
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
#include "ota.h"
#include "radiko.h"
#include "settings.h"
#include "stations.h"
#include "stream.h"
#include "timesync.h"
#include "viz.h"
#include "wifi.h"

static const char *TAG = "ui";

#define LVGL_BUF_LINES 20

// Spill pool for LVGL's allocator, in PSRAM. See ui_init() for why.
#define LVGL_PSRAM_POOL_BYTES (256 * 1024)

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
static lv_obj_t *w_bat        = NULL;
static lv_obj_t *w_hdr        = NULL;   // centre title: "Radiko - <area>"
static lv_obj_t *w_dots[MAX_STATIONS];
static lv_obj_t *s_list_prog[MAX_STATIONS];   // per-row programme labels (list)
static lv_obj_t *s_list_cont = NULL;          // scroll container, rows rebuilt per area

// =====================================================================
// LVGL <-> display glue
// =====================================================================
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Given by the SPI DMA-done ISR, awaited once per flush in flush_cb.
static SemaphoreHandle_t s_flush_sem = NULL;

void ui_apply_brightness(void);   // defined with the settings code below

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, w * h);
    esp_err_t derr = esp_lcd_panel_draw_bitmap(display_panel(), area->x1, area->y1,
                                               area->x2 + 1, area->y2 + 1, px_map);
    // Block (yielding) until the DMA finishes, then hand the buffer back to LVGL.
    // Exactly one take per flush pairs with one give per completion — no desync,
    // no frame drops. Yielding (vs LVGL's default busy-spin) lets the lower-prio
    // audio decoder on this core run, so playback doesn't stutter. The timeout
    // self-heals a lost completion instead of hanging.
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        // Track the failure shape: a one-off lost completion self-heals on the
        // next give; a run of these with draw errors means the SPI queue is
        // wedged (seen rarely after station switches — under investigation).
        static int timeouts = 0;
        ESP_LOGE(TAG, "flush DMA-done timeout #%d (draw=%s, area %dx%d)",
                 ++timeouts, esp_err_to_name(derr), w, h);
    }
    bool first_frame_done = lv_display_flush_is_last(disp);
    lv_display_flush_ready(disp);

    // Backlight comes on the moment the FIRST complete frame (the splash) is
    // on the glass: no dark pause at power-up, and no garbage frame either
    // (previously app_main forced full duty much later in boot).
    static bool s_bl_on = false;
    if (!s_bl_on && first_frame_done) {
        s_bl_on = true;
        ui_apply_brightness();
    }
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
    int i = (s_cur >= 0 && s_cur < stations_count()) ? s_cur : 0;
    return station_id(i);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    // Watchdog: a wedged render pass (Phase 17: lv_timer_handler never
    // returned) must panic+reboot into a working radio, not freeze forever.
    app_watchdog_add();
    while (true) {
        app_watchdog_feed();
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
// Player logo: a pre-scaled asset from the logos partition (fit 300x56),
// blitted 1:1 — no runtime image transform anywhere in the UI (Phase 17). The
// white card is sized to the logo plus a little padding.
static void set_player_logo(void)
{
    const lv_image_dsc_t *L = station_logo_big(s_cur);
    if (!L) return;
    lv_image_set_src(w_logo_img, L);
    lv_obj_set_size(w_logo_card, L->header.w + 12, L->header.h + 8);
    lv_obj_center(w_logo_card);
    lv_obj_center(w_logo_img);
}

// Show the cached "now on air" title for the current station (empty until the
// first program fetch lands). The label is a circular marquee for long titles.
static void set_program_label(void)
{
    char title[256];   // title + " / " + performers (pfm)
    radiko_program_title(station_id(s_cur), title, sizeof(title));
    lv_label_set_text(w_prog, title[0] ? title : "");
}

void ui_program_updated(void)
{
    ui_lock(-1);
    if (w_prog) set_program_label();
    // Refresh the station-list rows too (Arduino shows the programme per row).
    char buf[256];
    for (int i = 0; i < stations_count(); i++) {
        if (!s_list_prog[i]) continue;
        radiko_program_title(station_id(i), buf, sizeof(buf));
        lv_label_set_text(s_list_prog[i], buf[0] ? buf : "---");
    }
    ui_unlock();
}

// Position the active dots centred; hide the unused ones (count is per-area).
static void layout_dots(void)
{
    int n = stations_count();
    int dot_w = n > 0 ? n * 8 + (n - 1) * 4 : 0;
    int dx = (320 - dot_w) / 2;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!w_dots[i]) continue;
        if (i < n) {
            lv_obj_clear_flag(w_dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(w_dots[i], dx + i * 12, 228);
        } else {
            lv_obj_add_flag(w_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void refresh_player(void)
{
    if (s_cur >= stations_count()) s_cur = 0;
    set_player_logo();
    lv_label_set_text(w_name, station_name(s_cur));
    set_program_label();
    lv_label_set_text(w_play, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_slider_set_value(w_vol_slider, s_vol, LV_ANIM_OFF);
    lv_label_set_text_fmt(w_vol_val, "%d", s_vol);
    for (int i = 0; i < stations_count(); i++) {
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
    if (s_playing) stream_play(station_id(s_cur));
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

// ---- Screen dim/off state machine (Arduino screenState): 0=on 1=dimmed 2=off.
// Driven by a 1 s LVGL timer off lv_display_get_inactive_time(); any touch
// resets LVGL's inactivity clock, which the timer detects to restore the
// backlight. Note the waking touch also reaches whatever widget it lands on
// (same as the Arduino build).
static int s_screen_state = 0;
#define DIM_DUTY 18   // ~7% brightness when dimmed

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
    display_backlight_set(0);   // Arduino: sleep also turns the screen off
    s_screen_state = 2;
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
    settings_get()->led_mode = (uint8_t)led_mode();
    settings_save();
}

static void ev_open_list(lv_event_t *e) { lv_screen_load(s_scr_list); }

// ---- Settings / Info screen (Phase 15, Arduino port) ----
static lv_obj_t *s_scr_set = NULL;
static lv_obj_t *s_set_info[6];          // RSSI/area, IP, uptime, heap, battery, last crash
static lv_obj_t *s_set_bl_sl  = NULL;    // brightness slider (synced on bat-tap)
static lv_obj_t *s_set_bl_val = NULL;
static lv_obj_t *s_set_sl_val = NULL;
static lv_obj_t *s_set_dim_sl  = NULL;   // dim/off rows: greyed out when the
static lv_obj_t *s_set_dim_val = NULL;   // screen saver replaces them
static lv_obj_t *s_set_off_sl  = NULL;
static lv_obj_t *s_set_off_val = NULL;
static const uint8_t BL_DUTY[4] = { 64, 128, 192, 255 };
static const char   *BL_NAMES[4] = { "Low", "Mid", "High", "Max" };
static const char   *SLEEP_NAMES[4] = { "Off", "30 min", "60 min", "90 min" };
static const uint32_t DIM_MS[5]   = { 60000, 180000, 300000, 600000, 900000 };
static const char    *DIM_NAMES[5] = { "1 min", "3 min", "5 min", "10 min", "15 min" };
static const uint32_t OFF_MS[5]   = { 180000, 300000, 600000, 1800000, 0 };  // 0=never
static const char    *OFF_NAMES[5] = { "3 min", "5 min", "10 min", "30 min", "Never" };

static int opt_index(const uint32_t *opts, int n, uint32_t v, int dflt)
{
    for (int i = 0; i < n; i++) if (opts[i] == v) return i;
    return dflt;
}

static void ev_set_back(lv_event_t *e) { lv_screen_load(s_scr_player); }

static void apply_brightness(void)
{
    settings_t *st = settings_get();
    display_backlight_set(BL_DUTY[st->brightness]);
    s_screen_state = 0;
    if (s_set_bl_val) lv_label_set_text(s_set_bl_val, BL_NAMES[st->brightness]);
    if (s_set_bl_sl)  lv_slider_set_value(s_set_bl_sl, st->brightness, LV_ANIM_OFF);
    // Flash the new level in the status bar (the 2 s status tick restores the
    // battery reading) — same transient feedback as the Arduino build.
    if (w_bat) lv_label_set_text_fmt(w_bat, LV_SYMBOL_SETTINGS " %s",
                                     BL_NAMES[st->brightness]);
}

static void ev_set_bl(lv_event_t *e)
{
    int idx = (int)lv_slider_get_value(lv_event_get_target(e));
    settings_get()->brightness = idx;
    apply_brightness();
    settings_save();
}

void ui_apply_brightness(void)
{
    display_backlight_set(BL_DUTY[settings_get()->brightness]);
}

// Arduino bat_btn: tapping the battery cycles the brightness level.
static void ev_bat_cycle(lv_event_t *e)
{
    settings_t *st = settings_get();
    st->brightness = (st->brightness + 1) % 4;
    apply_brightness();
    settings_save();
}

static void ev_set_sleep(lv_event_t *e)
{
    int idx = (int)lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text(s_set_sl_val, SLEEP_NAMES[idx]);
    s_sleep_idx = idx;
    int min = SLEEP_MIN[idx];
    if (s_sleep_timer) {
        if (min == 0) lv_timer_pause(s_sleep_timer);
        else {
            lv_timer_set_period(s_sleep_timer, (uint32_t)min * 60 * 1000);
            lv_timer_reset(s_sleep_timer);
            lv_timer_resume(s_sleep_timer);
        }
    }
    settings_get()->sleep_mins = (uint8_t)min;
    settings_save();
}

static void ev_set_dim(lv_event_t *e)
{
    int idx = (int)lv_slider_get_value(lv_event_get_target(e));
    settings_get()->dim_ms = DIM_MS[idx];
    lv_label_set_text(s_set_dim_val, DIM_NAMES[idx]);
    settings_save();
}

static void ev_set_off(lv_event_t *e)
{
    int idx = (int)lv_slider_get_value(lv_event_get_target(e));
    settings_get()->off_ms = OFF_MS[idx];
    lv_label_set_text(s_set_off_val, OFF_NAMES[idx]);
    settings_save();
}

// With the saver on it replaces dim/off entirely (Arduino behaviour), so grey
// out those sliders. The slider's title label is stashed as its user_data.
static void update_dim_off_state(void)
{
    // With the saver on, "Screen Dim" still controls WHEN the clock appears, so
    // it stays active; only "Screen Off" is unused (the saver never blanks).
    bool saver = settings_get()->saver;
    bool grey[2] = { false, saver };   // {dim, off}
    lv_obj_t *rows[2][2] = { { s_set_dim_sl, s_set_dim_val },
                             { s_set_off_sl, s_set_off_val } };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *sl = rows[i][0];
        if (!sl) continue;
        if (grey[i]) lv_obj_add_state(sl, LV_STATE_DISABLED);
        else         lv_obj_remove_state(sl, LV_STATE_DISABLED);
        lv_obj_t *title = lv_obj_get_user_data(sl);
        lv_obj_set_style_text_color(title,
            lv_color_hex(grey[i] ? 0x555577 : C_HL), 0);
        lv_obj_set_style_text_color(rows[i][1],
            lv_color_hex(grey[i] ? 0x666688 : C_TEXT), 0);
    }
}

static void ev_set_saver(lv_event_t *e)
{
    settings_get()->saver =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
    update_dim_off_state();
}

static void ev_set_rot(lv_event_t *e)
{
    bool flip = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_get()->rotation = flip ? 1 : 3;   // 3 = default upside-down mount
    settings_save();
    display_set_flipped(flip);                 // touch remaps via display_flipped()
    lv_obj_invalidate(lv_screen_active());     // repaint everything the new way up
}

// ---- Listen-area picker (Phase 30): which prefecture to authenticate AS ----
static void rebuild_list_rows(void);   // fwd (defined with the list screen)

// Re-auth as the newly chosen area and restart the stream. Blocking TLS +
// program fetch, so it runs in its own task off the LVGL thread.
static void area_apply_task(void *arg)
{
    radiko_auth_t a;
    if (radiko_authenticate(&a) == ESP_OK) {
        ui_lock(-1);
        if (w_hdr) lv_label_set_text_fmt(w_hdr, "Radiko - %s",
                                         radiko_area_name(radiko_area_num()));
        ui_unlock();
        stream_play(station_id(s_cur));
        radiko_program_refresh();   // the new area's titles now, not in 5 min
    }
    vTaskDelete(NULL);
}

static void ev_area_changed(lv_event_t *e)
{
    int jp = (int)lv_dropdown_get_selected(lv_event_get_target(e)) + 1;  // JP1..47
    settings_get()->area    = (uint8_t)jp;
    settings_get()->station = 0;   // active list changes; start at its first
    settings_save();
    radiko_set_area(jp);
    stations_set_area(jp);
    s_cur = 0;
    s_playing = true;
    // Rebuild the per-area UI now; re-auth + restream happens in the task.
    layout_dots();
    rebuild_list_rows();
    refresh_player();
    ui_program_updated();
    stream_stop();
    xTaskCreate(area_apply_task, "area", 8192, NULL, 5, NULL);
}

// Fill the System Info labels; called each time the screen opens.
static void refresh_settings_info(void)
{
    char b[64];
    snprintf(b, sizeof(b), LV_SYMBOL_WIFI " %d dBm    Area: %s",
             wifi_get_rssi(), radiko_area()[0] ? radiko_area() : "-");
    lv_label_set_text(s_set_info[0], b);
    snprintf(b, sizeof(b), "IP: %s", wifi_get_ip());
    lv_label_set_text(s_set_info[1], b);
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(b, sizeof(b), "Uptime: %lld:%02lld:%02lld", up / 3600, (up / 60) % 60, up % 60);
    lv_label_set_text(s_set_info[2], b);
    snprintf(b, sizeof(b), "Free: %u KB internal, %u KB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(s_set_info[3], b);
    int mv = battery_mv();
    if (mv >= 0) snprintf(b, sizeof(b), "Battery: %d mV (%d%%)", mv, battery_pct());
    else         snprintf(b, sizeof(b), "Battery: n/a");
    lv_label_set_text(s_set_info[4], b);
    snprintf(b, sizeof(b), "Last crash: %s", crashlog_last());
    lv_label_set_text(s_set_info[5], b);
}

static void ev_open_settings(lv_event_t *e)
{
    refresh_settings_info();
    lv_screen_load(s_scr_set);
}

// ---- OTA check/update from Settings (Phase 22) ----
static lv_obj_t *s_ota_btn = NULL;
static lv_obj_t *s_ota_lbl = NULL;

// Progress marshalled from the OTA task into LVGL under the lock.
static void ota_cb(const char *status, int pct)
{
    ui_lock(-1);
    if (pct >= 0) lv_label_set_text_fmt(s_ota_lbl, "%s  %d%%", status, pct);
    else          lv_label_set_text(s_ota_lbl, status);
    ui_unlock();
}

static void ota_task(void *arg)
{
    char tag[32], url[256];
    bool newer = false;
    esp_err_t err = ota_check(tag, sizeof(tag), url, sizeof(url), &newer);
    if (err != ESP_OK) {
        ota_cb(err == ESP_ERR_NOT_FOUND ? "No release with a .bin found"
                                        : "Check failed - network?", -1);
    } else if (!newer) {
        char b[48];
        snprintf(b, sizeof(b), "Up to date (v%s)",
                 esp_app_get_description()->version);
        ota_cb(b, -1);
    } else {
        // Playback stops for the download: a 3 MB TLS transfer wants the
        // bandwidth and the CPU. ota_update() reboots on success, so the
        // line below it only runs on failure.
        char b[48];
        snprintf(b, sizeof(b), "Updating to %s...", tag);
        ota_cb(b, -1);
        stream_stop();
        ota_update(url, ota_cb);
        ota_cb("Update failed - press play to resume", -1);
    }
    ui_lock(-1);
    if (s_ota_btn) lv_obj_remove_state(s_ota_btn, LV_STATE_DISABLED);
    ui_unlock();
    vTaskDelete(NULL);
}

static void ev_ota_check(lv_event_t *e)
{
    lv_obj_add_state(s_ota_btn, LV_STATE_DISABLED);   // no re-entry
    lv_label_set_text(s_ota_lbl, "Checking...");
    // 8 KB stack: TLS + esp_https_ota run inside this task.
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}

static lv_obj_t *set_slider_row(lv_obj_t *cont, const char *title, int max_idx,
                                int cur, const char *cur_name,
                                lv_event_cb_t cb, lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, 300, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(row);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, lv_color_hex(C_HL), 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 4, 0);

    *out_val = lv_label_create(row);
    lv_label_set_text(*out_val, cur_name);
    lv_obj_set_style_text_color(*out_val, lv_color_hex(C_TEXT), 0);
    lv_obj_align(*out_val, LV_ALIGN_TOP_RIGHT, -4, 0);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_size(sl, 264, 8);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_slider_set_range(sl, 0, max_idx);
    lv_slider_set_value(sl, cur, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_HL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_HL), LV_PART_KNOB);
    // Disabled look (dim/off rows grey out while the saver owns the timeouts)
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x222238), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x444466), LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x555577), LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_user_data(sl, tl);   // update_dim_off_state greys the title via this
    return sl;
}

static lv_obj_t *set_switch_row(lv_obj_t *cont, const char *title, bool on,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, 300, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(row);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, lv_color_hex(C_HL), 0);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_HL), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void build_settings_screen(void)
{
    s_scr_set = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_set, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scr_set, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(s_scr_set);
    lv_obj_set_size(hdr, 320, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_size(back, 72, 24);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, ev_set_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, "Settings / Info");
    lv_obj_set_style_text_color(ttl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 24, 0);

    lv_obj_t *cont = lv_obj_create(s_scr_set);
    lv_obj_set_size(cont, 320, 212);
    lv_obj_set_pos(cont, 0, 28);
    lv_obj_set_style_bg_color(cont, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_style_pad_row(cont, 6, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    settings_t *st = settings_get();

    // ---- Listen Area (Phase 30): authenticate as any prefecture, VPN-free ----
    {
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_size(row, 300, 40);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *tl = lv_label_create(row);
        lv_label_set_text(tl, "Listen Area");
        lv_obj_set_style_text_color(tl, lv_color_hex(C_HL), 0);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

        static char opts[640];   // 47 JP names, \n-joined; static: dropdown keeps the ptr
        opts[0] = '\0';
        for (int i = 1; i <= 47; i++) {
            strcat(opts, radiko_area_name_jp(i));
            if (i < 47) strcat(opts, "\n");
        }
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options_static(dd, opts);
        lv_dropdown_set_selected(dd, (st->area >= 1 && st->area <= 47) ? st->area - 1 : 12);
        // The default LVGL arrow symbol isn't in our JP font (tofu), but the
        // font does carry U+25BC ▼ (geometric-shapes range) — use that.
        lv_dropdown_set_symbol(dd, "\xE2\x96\xBC");
        lv_obj_set_width(dd, 118);
        lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -4, 0);
        // Match the app's dark palette — the stock LVGL dropdown theme is light
        // and clashes. Button: accent tile like our other buttons.
        lv_obj_set_style_text_font(dd, &lv_font_jp_16, 0);
        lv_obj_set_style_bg_color(dd, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_text_color(dd, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_border_width(dd, 0, 0);
        lv_obj_set_style_radius(dd, 6, 0);
        lv_obj_set_style_shadow_width(dd, 0, 0);
        // Open list: dark panel, tight rows, accent highlight on the selection.
        lv_obj_t *list = lv_dropdown_get_list(dd);
        lv_obj_set_style_text_font(list, &lv_font_jp_16, 0);
        lv_obj_set_style_text_line_space(list, 2, 0);
        lv_obj_set_style_pad_ver(list, 2, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_radius(list, 6, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(C_HL), LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_add_event_cb(dd, ev_area_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    s_set_bl_sl = set_slider_row(cont, "Brightness", 3, st->brightness,
                                 BL_NAMES[st->brightness], ev_set_bl, &s_set_bl_val);
    // Arduino row order: Brightness, Screen Dim, Screen Off, Sleep Timer.
    int didx = opt_index(DIM_MS, 5, st->dim_ms, 2);
    s_set_dim_sl = set_slider_row(cont, "Screen Dim", 4, didx,
                                  DIM_NAMES[didx], ev_set_dim, &s_set_dim_val);
    int oidx = opt_index(OFF_MS, 5, st->off_ms, 2);
    s_set_off_sl = set_slider_row(cont, "Screen Off", 4, oidx,
                                  OFF_NAMES[oidx], ev_set_off, &s_set_off_val);
    int sidx = (st->sleep_mins >= 90) ? 3 : (st->sleep_mins >= 60) ? 2
             : (st->sleep_mins >= 30) ? 1 : 0;
    set_slider_row(cont, "Sleep Timer", 3, sidx,
                   SLEEP_NAMES[sidx], ev_set_sleep, &s_set_sl_val);

    set_switch_row(cont, "Flip Screen 180\xC2\xB0", st->rotation == 1, ev_set_rot);
    set_switch_row(cont, "Screen Saver (Clock)", st->saver, ev_set_saver);

    lv_obj_t *sec = lv_label_create(cont);
    lv_label_set_text(sec, "System Info");
    lv_obj_set_style_text_color(sec, lv_color_hex(C_HL), 0);
    for (int i = 0; i < 6; i++) {
        s_set_info[i] = lv_label_create(cont);
        lv_obj_set_style_text_color(s_set_info[i], lv_color_hex(C_TEXT), 0);
        lv_label_set_text(s_set_info[i], "...");
    }

    lv_obj_t *sec2 = lv_label_create(cont);
    lv_label_set_text(sec2, "Firmware");
    lv_obj_set_style_text_color(sec2, lv_color_hex(C_HL), 0);
    const esp_app_desc_t *app = esp_app_get_description();
    char fw[72];
    lv_obj_t *f1 = lv_label_create(cont);
    lv_obj_set_style_text_color(f1, lv_color_hex(C_TEXT), 0);
    snprintf(fw, sizeof(fw), "v%s (%s %s)", app->version, app->date, app->time);
    lv_label_set_text(f1, fw);
    lv_obj_t *f2 = lv_label_create(cont);
    lv_obj_set_style_text_color(f2, lv_color_hex(C_TEXT), 0);
    snprintf(fw, sizeof(fw), "IDF %s", app->idf_ver);
    lv_label_set_text(f2, fw);

    // OTA (Phase 22): check GitHub releases, update in place.
    s_ota_btn = lv_button_create(cont);
    lv_obj_set_size(s_ota_btn, 288, 30);
    lv_obj_set_style_bg_color(s_ota_btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_color(s_ota_btn, lv_color_hex(C_HL), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_ota_btn, 0, 0);
    lv_obj_add_event_cb(s_ota_btn, ev_ota_check, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(s_ota_btn);
    lv_label_set_text(ol, LV_SYMBOL_DOWNLOAD "  Check for Update");
    lv_obj_center(ol);

    s_ota_lbl = lv_label_create(cont);
    lv_obj_set_style_text_color(s_ota_lbl, lv_color_hex(C_DIM), 0);
    lv_label_set_text(s_ota_lbl, "");

    update_dim_off_state();   // grey dim/off if the saver owns the timeouts
}

// ---- Screen saver (Phase 16, Arduino port): DVD-style bouncing clock ----
// Entered from the player screen after the dim timeout when the saver switch is
// on; the off timeout then dims the clock instead of blanking (never fully off).
// Audio keeps playing throughout. Any touch returns to the player.
static lv_obj_t   *s_scr_saver   = NULL;
static lv_obj_t   *s_saver_box   = NULL;
static lv_obj_t   *s_saver_clock = NULL;
static lv_obj_t   *s_saver_date  = NULL;
static lv_timer_t *s_saver_timer = NULL;   // 60 ms bounce tick, paused unless shown
#define SAVER_BOX_W 168
#define SAVER_BOX_H 80

static void saver_update_clock(void)
{
    if (!timesync_valid()) {
        lv_label_set_text(s_saver_clock, "--:--");
        lv_label_set_text(s_saver_date, "");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_saver_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    static const char *WDAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    lv_label_set_text_fmt(s_saver_date, "%s  %04d-%02d-%02d",
                          WDAYS[tm.tm_wday % 7],
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

// One bounce step; the clock changes colour on each wall hit.
static void saver_tick_cb(lv_timer_t *t)
{
    static int px = 80, py = 90, vx = 2, vy = 1;
    static uint16_t hue = 0;
    static int clock_div = 0;
    px += vx;
    py += vy;
    bool bounced = false;
    if (px <= 0)                             { px = 0;                             vx = -vx; bounced = true; }
    if (px + SAVER_BOX_W >= DISPLAY_H_RES)   { px = DISPLAY_H_RES - SAVER_BOX_W;   vx = -vx; bounced = true; }
    if (py <= 0)                             { py = 0;                             vy = -vy; bounced = true; }
    if (py + SAVER_BOX_H >= DISPLAY_V_RES)   { py = DISPLAY_V_RES - SAVER_BOX_H;   vy = -vy; bounced = true; }
    lv_obj_set_pos(s_saver_box, px, py);
    if (bounced) {
        hue = (hue + 47) % 360;   // 47° step hits every colour before repeating
        lv_obj_set_style_text_color(s_saver_clock, lv_color_hsv_to_rgb(hue, 100, 100), 0);
    }
    if (++clock_div >= 167) {     // ~10 s at 60 ms/tick
        clock_div = 0;
        saver_update_clock();
    }
}

// Idempotent: reached both from the tap handler and the idle timer's wake path.
static void saver_exit(void)
{
    if (s_saver_timer) lv_timer_pause(s_saver_timer);
    s_screen_state = 0;
    display_backlight_set(BL_DUTY[settings_get()->brightness]);
    if (lv_screen_active() == s_scr_saver) lv_screen_load(s_scr_player);
}

static void ev_saver_touched(lv_event_t *e) { saver_exit(); }

static void saver_enter(void)
{
    saver_update_clock();
    lv_timer_resume(s_saver_timer);
    lv_screen_load(s_scr_saver);
}

static void build_saver_screen(void)
{
    s_scr_saver = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_saver, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(s_scr_saver, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr_saver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr_saver, ev_saver_touched, LV_EVENT_CLICKED, NULL);

    // Bouncing container holding the clock + date.
    s_saver_box = lv_obj_create(s_scr_saver);
    lv_obj_set_size(s_saver_box, SAVER_BOX_W, SAVER_BOX_H);
    lv_obj_set_pos(s_saver_box, 80, 90);
    lv_obj_set_style_bg_opa(s_saver_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_saver_box, 0, 0);
    lv_obj_set_style_pad_all(s_saver_box, 0, 0);
    lv_obj_clear_flag(s_saver_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_saver_box, LV_OBJ_FLAG_CLICKABLE);   // taps fall through to the screen

    s_saver_clock = lv_label_create(s_saver_box);
    lv_label_set_text(s_saver_clock, "--:--");
    lv_obj_set_style_text_color(s_saver_clock, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_saver_clock, &lv_font_montserrat_48, 0);
    lv_obj_align(s_saver_clock, LV_ALIGN_TOP_MID, 0, 0);

    s_saver_date = lv_label_create(s_saver_box);
    lv_label_set_text(s_saver_date, "");
    lv_obj_set_style_text_color(s_saver_date, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(s_saver_date, &lv_font_montserrat_16, 0);
    lv_obj_align(s_saver_date, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ---- Boot splash: logo + status line until audio actually starts ----
// Event-driven, not a fixed timer: audio_write's first real PCM triggers
// ui_splash_done(), so the splash covers exactly the not-ready period however
// long boot takes. A minimum stops a fast boot from blinking it; a failsafe
// stops a boot problem from trapping the user on it. If WiFi setup is needed,
// that screen simply replaces the splash and all of this stands down.
static lv_obj_t *s_scr_splash = NULL;
static lv_obj_t *s_splash_lbl = NULL;
#define SPLASH_MIN_MS      2500
// Measured boot-to-first-PCM is 22-27 s (WiFi + auth + HLS chain + first
// segment); the failsafe must clear that comfortably or it, not the audio,
// ends the splash on ordinary boots.
#define SPLASH_FAILSAFE_MS 35000

static void splash_finish(void)
{
    if (s_scr_splash && lv_screen_active() == s_scr_splash)
        lv_screen_load(s_scr_player);
}

static void splash_timer_cb(lv_timer_t *t) { splash_finish(); }

static void build_splash_screen(void)
{
    s_scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_splash, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(s_scr_splash);
    lv_image_set_src(img, &SPLASH_IMG);   // pre-composited on C_BG, 1:1
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -24);

    s_splash_lbl = lv_label_create(s_scr_splash);
    lv_label_set_text(s_splash_lbl, "Starting...");
    lv_obj_set_style_text_color(s_splash_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(s_splash_lbl, LV_ALIGN_CENTER, 0, 64);
}

void ui_splash_status(const char *text)
{
    ui_lock(-1);
    if (s_splash_lbl && lv_screen_active() == s_scr_splash)
        lv_label_set_text(s_splash_lbl, text);
    ui_unlock();
}

void ui_splash_done(void)
{
    ui_lock(-1);
    if (s_scr_splash && lv_screen_active() == s_scr_splash) {
        int64_t up_ms = esp_timer_get_time() / 1000;
        if (up_ms >= SPLASH_MIN_MS) {
            splash_finish();
        } else {   // one-shot; LVGL auto-deletes when the repeat count runs out
            lv_timer_t *t = lv_timer_create(splash_timer_cb,
                                            (uint32_t)(SPLASH_MIN_MS - up_ms), NULL);
            lv_timer_set_repeat_count(t, 1);
        }
    }
    ui_unlock();
}

// ---- Audio visualiser (Phase 32) -----------------------------------------
// Lives INSIDE w_logo_tile as a sibling of the logo card, so the tile's existing
// swipe/tap gestures keep working over the bars for free.
//
// Two heights, because the screen is full: in logo mode the tile is 68 px and
// the programme line sits at y=112. In visualiser mode the programme line hides
// and the station name slides down into its slot, letting the bars run
// 28..110 = 82 px. That's the entire budget — status bar caps the top, the
// volume row at y=146 caps the bottom.
#define VIZ_H_LOGO   68
#define VIZ_H_TALL   82
#define NAME_Y_LOGO  91
#define NAME_Y_TALL  112         // the programme line's slot
#define VIZ_BAR_W    15
#define VIZ_BAR_PIT  19          // 15 px bar + 4 px gap
// Centre the strip so bars don't kiss the screen edges (they used to start 2 px
// from each side, which read as touching). Symmetric ~10 px margin.
#define VIZ_X0       ((320 - ((VIZ_BANDS - 1) * VIZ_BAR_PIT + VIZ_BAR_W)) / 2)
#define VIZ_SEGS     11          // grille lines for the LED style
#define VIZ_SEG_PIT  7

static lv_obj_t   *w_viz_rb;                    // rainbow container
static lv_obj_t   *w_viz_rb_bar[VIZ_BANDS];
static lv_obj_t   *w_viz_led;                   // LED/car container
// Three fixed-colour zones per band (green / amber / red), each sized to the
// part of the level that falls inside it. Colour therefore tracks ABSOLUTE
// height — a quiet band is all green — without the full-height gradient + shutter
// the first version used. That drew every pixel ~3x (gradient, then shutter over
// it, then caps) with per-pixel interpolation: 165-213 ms a frame, and it dragged
// the decoder back under real-time. Here we paint only the lit pixels, once.
#define LED_ZONES  3
#define LED_G_TOP  (VIZ_H_TALL * 60 / 100)   // green up to 60% of full scale
#define LED_A_TOP  (VIZ_H_TALL * 85 / 100)   // amber to 85%, red above
static lv_obj_t   *w_viz_led_zone[VIZ_BANDS][LED_ZONES];
static lv_obj_t   *w_viz_led_cap[VIZ_BANDS];    // peak-hold caps
// LED_RB: the LED/car layout (single solid bar + peak cap + grille) but each bar
// keeps its rainbow band-hue instead of the green/amber/red zones.
static lv_obj_t   *w_viz_lr;                    // rainbow-LED container
static lv_obj_t   *w_viz_lr_bar[VIZ_BANDS];
static lv_obj_t   *w_viz_lr_cap[VIZ_BANDS];
static uint8_t     s_peak[VIZ_BANDS];
static lv_timer_t *s_viz_timer;

// The container that hosts a given visualiser mode (NULL for logo).
static lv_obj_t *viz_container_for(uint8_t mode)
{
    switch (mode) {
        case VIZ_MODE_RAINBOW: return w_viz_rb;
        case VIZ_MODE_LED:     return w_viz_led;
        case VIZ_MODE_LED_RB:  return w_viz_lr;
        default:               return NULL;
    }
}

// A transparent, full-tile, non-interactive layer. EVENT_BUBBLE everywhere so
// swipe/tap/long-press still land on w_logo_tile through the bars.
static lv_obj_t *viz_container(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 320, VIZ_H_TALL);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

static lv_obj_t *viz_rect(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

// AUDIO WINS. The visualiser is not free: at 25 fps it cost the decoder enough
// throughput to drop it BELOW real-time (measured: PCM ring +36 KB/s with the
// logo showing, -17 KB/s with bars). The ring then bled out over ~40 s and the
// radio cracked — which is why the fault looked like "cracks a while after
// switching" rather than an instant glitch. It is a cross-core effect: the bars
// run on core 1, the decoder on core 0, and they contend for PSRAM/cache, not
// CPU (i2s_wr at prio 6 already preempts LVGL at 4).
//
// 10 fps + the single-invalidate render fix put the slope firmly positive (the
// ring stays flat with bars running), so the visualiser barely loads the system
// now. This gate is the backstop for when it isn't — a slow CDN, a station
// change, the first seconds after boot. Below PAUSE we stop drawing; we resume
// only once RESUME is buffered, and the hysteresis keeps it from flapping.
// Nothing is lost visually: a thin buffer means there's no music to show yet.
//
// The RESUME threshold trades startup cosmetics against startup audio. Dropping
// it to 1.5 s to start the bars sooner brought back an audible crackle in the
// first seconds — because the decoder is still catching up to the live edge then
// (a low absolute buffer doesn't mean it's running ahead yet), and the extra
// render load tips it into underrun. 3.0 s waits until the decoder genuinely has
// slack. The ~3-5 s of still bars at boot is the price of clean startup audio,
// and audio wins. Do NOT lower this without re-checking startup crackle by ear.
#define VIZ_RING_PAUSE  (192000 / 2 * 3)   // 1.5 s buffered: stop drawing
#define VIZ_RING_RESUME (192000 * 3)       // 3.0 s buffered: safe to draw again

static int s_bar_h[VIZ_BANDS];   // current on-screen bar height, px — retained so
                                // the idle fall can decay from the last real level

// Draw one band at height h (2..VIZ_H_TALL).
static void viz_render_bar(int i, int h, uint8_t mode)
{
    if (mode == VIZ_MODE_RAINBOW) {
        lv_obj_set_height(w_viz_rb_bar[i], h);
        lv_obj_set_y(w_viz_rb_bar[i], VIZ_H_TALL - h);   // grow up from the baseline
        return;
    }
    if (mode == VIZ_MODE_LED_RB) {
        lv_obj_set_height(w_viz_lr_bar[i], h);           // one solid hue bar
        lv_obj_set_y(w_viz_lr_bar[i], VIZ_H_TALL - h);
    } else {
        // Size each colour zone to the slice of the level inside it, so we paint
        // only lit pixels. Zone heights sum to h, and each keeps its own fixed
        // colour => colour tracks absolute height, with no overdraw.
        int g = h < LED_G_TOP ? h : LED_G_TOP;
        int a = h <= LED_G_TOP ? 0 : (h < LED_A_TOP ? h : LED_A_TOP) - LED_G_TOP;
        int r = h <= LED_A_TOP ? 0 : h - LED_A_TOP;
        lv_obj_set_height(w_viz_led_zone[i][0], g);
        lv_obj_set_y(w_viz_led_zone[i][0], VIZ_H_TALL - g);
        lv_obj_set_height(w_viz_led_zone[i][1], a);
        lv_obj_set_y(w_viz_led_zone[i][1], VIZ_H_TALL - LED_G_TOP - a);
        lv_obj_set_height(w_viz_led_zone[i][2], r);
        lv_obj_set_y(w_viz_led_zone[i][2], VIZ_H_TALL - LED_A_TOP - r);
    }
    // Shared peak cap (LED + LED_RB): jump up instantly, sink a pixel per frame.
    // The falling cap is the detail that reads as a car head unit — and it also
    // makes the idle fall look natural, the cap trailing the bars down.
    if (h > s_peak[i]) s_peak[i] = h;
    else if (s_peak[i] > 2) s_peak[i]--;
    lv_obj_t *cap = (mode == VIZ_MODE_LED_RB) ? w_viz_lr_cap[i] : w_viz_led_cap[i];
    lv_obj_set_y(cap, VIZ_H_TALL - s_peak[i] - 2);
}

// One frame of the idle fall: ease every bar toward the baseline. Returns true
// while anything is still falling. When the buffer goes thin (boot, station
// change) we run this instead of snapping flat — a hard jump to a flat line read
// as odd; a smooth drop reads as "settling". Just resizes existing objects, no
// FFT, so it's cheap enough to run during the fragile buffer-refill window.
static bool viz_decay(uint8_t mode)
{
    bool moving = false;
    for (int i = 0; i < VIZ_BANDS; i++) {
        if (s_bar_h[i] > 2) {
            s_bar_h[i] -= (s_bar_h[i] - 2) * 2 / 5 + 1;   // ~40%/frame, min 1 px
            if (s_bar_h[i] < 2) s_bar_h[i] = 2;
            moving = true;
        }
        // Drop the LED peak cap WITH the bar during the idle fall. Its normal
        // 1 px/frame sink (the car-stereo hold) is far slower than the bars, so
        // it would otherwise hang near the top for ~8 s — reading as a frozen
        // "top level bar" until the buffer refilled.
        s_peak[i] = s_bar_h[i];
        viz_render_bar(i, s_bar_h[i], mode);
    }
    lv_obj_invalidate(viz_container_for(mode));
    return moving;
}

static void viz_tick(lv_timer_t *t)
{
    uint8_t lvl[VIZ_BANDS];
    static bool s_starved, s_falling;
    uint8_t mode = settings_get()->viz;
    size_t ring = audio_ring_bytes();
    if (s_starved) {
        if (ring < VIZ_RING_RESUME) {
            if (s_falling) s_falling = viz_decay(mode);   // ease down, then rest
            return;
        }
        s_starved = false;
    } else if (ring < VIZ_RING_PAUSE) {
        s_starved = true;
        s_falling = true;    // start the idle fall from the current heights
        return;
    }

    // false = silent AND fully decayed: skip the LVGL writes entirely so a
    // paused radio costs zero invalidation/redraw.
    if (!viz_read(lvl, VIZ_BANDS)) return;

    for (int i = 0; i < VIZ_BANDS; i++) {
        int h = 2 + (lvl[i] * (VIZ_H_TALL - 2)) / 255;
        s_bar_h[i] = h;
        viz_render_bar(i, h, mode);
    }

    // Invalidate the whole strip ON PURPOSE, even though every bar already
    // invalidated itself. The bars are disjoint (gaps between them), so LVGL
    // cannot merge their dirty areas and emits ~15 tiny flushes per frame — each
    // paying address-window commands, a DMA start, an ISR and a semaphore
    // round-trip to paint ~800 px. That per-flush overhead, NOT the pixel count,
    // cost 100-155 ms a frame. One big area collapses it to ~5 buffer-sized
    // flushes. Repainting more pixels is faster here — the logo screen already
    // proved it: 160k px/s in 45 flushes, perfectly smooth, versus the bars'
    // 128k px/s in 153 flushes and a visibly frozen UI.
    lv_obj_invalidate(viz_container_for(mode));
}

// Visual only — no persistence, so it can also be called at init from settings.
static void viz_apply(uint8_t mode)
{
    if (!w_viz_rb || !w_viz_led || !w_viz_lr || !w_logo_card) return;
    bool on = (mode != VIZ_MODE_LOGO);

    if (on) lv_obj_add_flag(w_logo_card, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_clear_flag(w_logo_card, LV_OBJ_FLAG_HIDDEN);

    // Show only the active mode's container.
    lv_obj_t *want = viz_container_for(mode);
    lv_obj_t *all[] = { w_viz_rb, w_viz_led, w_viz_lr };
    for (int i = 0; i < 3; i++) {
        if (all[i] == want) lv_obj_clear_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Reclaim the programme line's 21 px for the bars, and move the name into it.
    lv_obj_set_height(w_logo_tile, on ? VIZ_H_TALL : VIZ_H_LOGO);
    if (w_prog) {
        if (on) lv_obj_add_flag(w_prog, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(w_prog, LV_OBJ_FLAG_HIDDEN);
    }
    if (w_name) lv_obj_align(w_name, LV_ALIGN_TOP_MID, 0, on ? NAME_Y_TALL : NAME_Y_LOGO);

    if (s_viz_timer) {
        if (on) lv_timer_resume(s_viz_timer);
        else    lv_timer_pause(s_viz_timer);
    }
}

static void viz_cycle(void)
{
    settings_t *st = settings_get();
    st->viz = (st->viz + 1) % VIZ_MODE_COUNT;   // logo -> rainbow -> LED -> logo
    settings_save();
    viz_apply(st->viz);
    static const char *names[] = { "logo", "rainbow", "LED", "LED-rainbow" };
    ESP_LOGI(TAG, "player tile: %s", names[st->viz]);
}

// Swipe/tap gestures on the full-width logo strip (Arduino behaviour):
//   dx >= +25 → prev station, dx <= -25 → next station,
//   |dx| < 8 AND released in the centre region (x 120..200) → open station list,
//   anything else → dead-zone (prevents accidental list opens during near-swipes).
//   long press (held still) → toggle logo <-> visualiser.
static void ev_prev(lv_event_t *e);
static void ev_next(lv_event_t *e);
static int32_t s_press_x;
static bool s_long_fired;   // see ev_logo_released
static void ev_logo_pressed(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    s_press_x = p.x;
    s_long_fired = false;
}
// LVGL fires LONG_PRESSED at ~400 ms while the finger is STILL DOWN; the
// |dx| guard means a slow swipe changes station instead of toggling.
static void ev_logo_long(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int dx = p.x - s_press_x;
    if (dx > -8 && dx < 8) {
        viz_cycle();
        s_long_fired = true;
    }
}
static void ev_logo_released(lv_event_t *e)
{
    // RELEASED still fires after LONG_PRESSED. Without this latch a long press
    // would toggle the visualiser AND (dx≈0, centre region) open the station
    // list — one press, two actions.
    if (s_long_fired) { s_long_fired = false; return; }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int dx = p.x - s_press_x;
    if      (dx >= 25) ev_prev(e);   // swipe right → previous station
    else if (dx <= -25) ev_next(e);  // swipe left  → next station
    else if (dx > -8 && dx < 8 && p.x >= 120 && p.x <= 200 && p.y >= 40)
        ev_open_list(e);   // y-guard: don't steal near-miss taps on the title
}
static void ev_back_to_player(lv_event_t *e) { lv_screen_load(s_scr_player); }
// Audio keeps playing during WiFi setup (unlike the Arduino, which had to stop
// its audio library): a scan only blips the connection and the 30 s PCM buffer
// rides through it. Actually changing networks interrupts audio naturally.
static void ev_open_wifi_setup(lv_event_t *e) { ui_show_wifi_setup(); }

static void ev_prev(lv_event_t *e)
{
    s_cur = (s_cur + stations_count() - 1) % stations_count();
    refresh_player();
    schedule_commit();   // debounced: persist + switch stream once user settles
}
static void ev_next(lv_event_t *e)
{
    s_cur = (s_cur + 1) % stations_count();
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

    // Battery icon + % on the same 2 s tick (this cadence also feeds the
    // charging-trend window in battery_charging()).
    if (w_bat) {
        int pct = battery_pct();
        if (pct >= 0) {
            const char *icon = battery_charging() ? LV_SYMBOL_CHARGE
                             : pct > 75 ? LV_SYMBOL_BATTERY_FULL
                             : pct > 50 ? LV_SYMBOL_BATTERY_3
                             : pct > 25 ? LV_SYMBOL_BATTERY_2
                             : LV_SYMBOL_BATTERY_EMPTY;
            lv_label_set_text_fmt(w_bat, "%s %d%%", icon, pct > 100 ? 100 : pct);
        }
    }
}

// Fast tick (300 ms, so a wake touch restores the backlight without a visible
// lag): dim then switch off the backlight after inactivity; restore on the
// next touch (LVGL's inactivity clock resets on any input event).
static void idle_timer_cb(lv_timer_t *t)
{
    static uint32_t last_idle = 0;
    settings_t *st = settings_get();
    uint32_t idle = lv_display_get_inactive_time(NULL);
    bool woke = idle < last_idle;   // clock reset since last tick = user touched
    last_idle = idle;

    if (woke) {
        if (lv_screen_active() == s_scr_saver) {
            saver_exit();   // backstop for touches the tap handler missed
        } else if (s_screen_state != 0) {
            s_screen_state = 0;
            display_backlight_set(BL_DUTY[st->brightness]);
        }
        return;
    }
    if (st->saver) {
        // Saver fully replaces dim/off: after the dim timeout, show the
        // bouncing clock at full brightness and leave it there — no dimming
        // (the moving clock avoids burn-in, and dimming a screensaver is what
        // the user explicitly does NOT want). Any touch exits via the wake path.
        if (s_screen_state == 0 && idle > st->dim_ms &&
            lv_screen_active() == s_scr_player) {
            s_screen_state = 1;
            saver_enter();
        }
        return;
    }
    if (s_screen_state == 0 && idle > st->dim_ms) {
        s_screen_state = 1;
        display_backlight_set(DIM_DUTY);
    } else if (s_screen_state == 1 && st->off_ms && idle > st->off_ms) {
        s_screen_state = 2;
        display_backlight_set(0);
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
    lv_obj_set_ext_click_area(wbtn, 16);   // small target at the screen edge
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

    // Real button (Arduino hdr_btn): the whole centre area opens Settings.
    // OVERFLOW_VISIBLE lets the title label spill past the 140 px button so a
    // long "Radiko - <area>" (e.g. Kanagawa) shows in full instead of clipping;
    // the centre gap to the wifi/battery buttons has room for it.
    lv_obj_t *hdr_btn = lv_button_create(bar);
    lv_obj_set_size(hdr_btn, 140, 24);
    lv_obj_add_flag(hdr_btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(hdr_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(hdr_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(hdr_btn, lv_color_hex(C_HL), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(hdr_btn, 0, 0);
    lv_obj_add_event_cb(hdr_btn, ev_open_settings, LV_EVENT_CLICKED, NULL);
    w_hdr = lv_label_create(hdr_btn);   // English, default 14 pt (overflows the button)
    lv_label_set_text_fmt(w_hdr, "Radiko - %s", radiko_area_name(radiko_area_num()));
    lv_obj_set_style_text_color(w_hdr, lv_color_hex(C_TEXT), 0);
    lv_obj_center(w_hdr);

    // Battery (real reading via ADC); tapping it cycles brightness (Arduino).
    lv_obj_t *bbtn = lv_button_create(bar);
    lv_obj_set_size(bbtn, 86, 22);
    lv_obj_align(bbtn, LV_ALIGN_RIGHT_MID, 4, 0);
    lv_obj_set_style_bg_opa(bbtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(bbtn, 0, 0);
    lv_obj_set_ext_click_area(bbtn, 16);
    lv_obj_add_event_cb(bbtn, ev_bat_cycle, LV_EVENT_CLICKED, NULL);
    w_bat = lv_label_create(bbtn);
    lv_label_set_text(w_bat, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(w_bat, lv_color_hex(C_TEXT), 0);
    lv_obj_align(w_bat, LV_ALIGN_RIGHT_MID, -2, 0);

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
    lv_obj_add_event_cb(w_logo_tile, ev_logo_long, LV_EVENT_LONG_PRESSED, NULL);
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

    // ---- Visualisers: the logo card's siblings, cycled by long-press ---------
    // Plain rects, not an lv_canvas: a 320x82 RGB565 canvas would be 52 KB of
    // LVGL heap, and LVGL draws rects for us anyway.
    w_viz_rb  = viz_container(w_logo_tile);
    w_viz_led = viz_container(w_logo_tile);
    w_viz_lr  = viz_container(w_logo_tile);

    // Rainbow: hue by BAND INDEX, fixed per bar, computed once here. Bass red ->
    // treble violet, so the bars dance while the spectrum stays legible. Hue
    // driven by amplitude instead makes the whole strip strobe as one and reads
    // as noise.
    for (int i = 0; i < VIZ_BANDS; i++) {
        lv_obj_t *b = viz_rect(w_viz_rb, VIZ_X0 + i * VIZ_BAR_PIT, VIZ_H_TALL - 2,
                               VIZ_BAR_W, 2);
        lv_obj_set_style_bg_color(b, lv_color_hsv_to_rgb(i * 360 / VIZ_BANDS, 90, 100), 0);
        lv_obj_set_style_radius(b, 2, 0);
        w_viz_rb_bar[i] = b;
    }

    // LED/car: three solid colour zones per band, resized each frame to the slice
    // of the level that falls inside them. The obvious build — one full-height
    // green->amber->red gradient per band with a shutter masking the unlit top —
    // looks identical and is what I wrote first, but it paints every pixel ~3x
    // (gradient, shutter over it, caps) with per-pixel interpolation: 165-213 ms
    // a frame, and it pulled the decoder back under real-time. Solid zones paint
    // lit pixels only, once, and keep colour tied to ABSOLUTE height.
    static const uint32_t LED_COL[LED_ZONES] = { 0x22C55E, 0xF59E0B, 0xEF4444 };
    for (int i = 0; i < VIZ_BANDS; i++) {
        int x = VIZ_X0 + i * VIZ_BAR_PIT;
        for (int z = 0; z < LED_ZONES; z++) {
            lv_obj_t *o = viz_rect(w_viz_led, x, VIZ_H_TALL, VIZ_BAR_W, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(LED_COL[z]), 0);
            w_viz_led_zone[i][z] = o;
        }
        // Peak cap last per band, so it paints over the zones.
        lv_obj_t *cap = viz_rect(w_viz_led, x, VIZ_H_TALL - 2, VIZ_BAR_W, 2);
        lv_obj_set_style_bg_color(cap, lv_color_hex(0xEAEAEA), 0);
        w_viz_led_cap[i] = cap;
    }
    // Grille last => on top of everything, turning the solid bars into a ladder
    // of discrete segments. Static: never touched again after this loop.
    for (int r = 1; r < VIZ_SEGS; r++) {
        lv_obj_t *g = viz_rect(w_viz_led, 0, VIZ_H_TALL - r * VIZ_SEG_PIT, 320, 2);
        lv_obj_set_style_bg_color(g, lv_color_hex(C_BG), 0);
    }

    // LED_RB: the same car-stereo layout (single solid bar + peak cap + grille)
    // but each bar wears its rainbow band-hue instead of the green/amber/red
    // zones. One bar object per band (not three), grown from the baseline.
    for (int i = 0; i < VIZ_BANDS; i++) {
        int x = VIZ_X0 + i * VIZ_BAR_PIT;
        lv_obj_t *b = viz_rect(w_viz_lr, x, VIZ_H_TALL, VIZ_BAR_W, 0);
        lv_obj_set_style_bg_color(b, lv_color_hsv_to_rgb(i * 360 / VIZ_BANDS, 90, 100), 0);
        w_viz_lr_bar[i] = b;
        lv_obj_t *cap = viz_rect(w_viz_lr, x, VIZ_H_TALL - 2, VIZ_BAR_W, 2);
        lv_obj_set_style_bg_color(cap, lv_color_hex(0xEAEAEA), 0);
        w_viz_lr_cap[i] = cap;
    }
    for (int r = 1; r < VIZ_SEGS; r++) {
        lv_obj_t *g = viz_rect(w_viz_lr, 0, VIZ_H_TALL - r * VIZ_SEG_PIT, 320, 2);
        lv_obj_set_style_bg_color(g, lv_color_hex(C_BG), 0);
    }

    s_viz_timer = lv_timer_create(viz_tick, 100, NULL);   // 10 fps — see viz_tick
    lv_timer_pause(s_viz_timer);                          // viz_apply() resumes it
    // NOTE: viz_apply() is deliberately NOT called here — it moves w_name and
    // hides w_prog, which do not exist yet. It runs at the END of this function.

    // Extend the swipe/tap/long-press area over the station name + programme
    // block below the logo tile. A transparent CLICKABLE layer wired to the same
    // handlers; the name/prog labels are created AFTER it (higher z) and are not
    // clickable, so touches on them fall through to this layer. Ends above the
    // volume row (y=146) so it never steals the slider.
    lv_obj_t *gext = lv_obj_create(s_scr_player);
    lv_obj_set_size(gext, 320, 56);
    lv_obj_set_pos(gext, 0, 88);
    lv_obj_set_style_bg_opa(gext, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gext, 0, 0);
    lv_obj_set_style_pad_all(gext, 0, 0);
    lv_obj_clear_flag(gext, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gext, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gext, ev_logo_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(gext, ev_logo_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(gext, ev_logo_long, LV_EVENT_LONG_PRESSED, NULL);

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

    // ---- Position dots ---- (created for the max; layout_dots shows the
    // active per-area subset, repositioned when the area changes)
    for (int i = 0; i < MAX_STATIONS; i++) {
        w_dots[i] = lv_obj_create(s_scr_player);
        lv_obj_set_size(w_dots[i], 8, 8);
        lv_obj_set_pos(w_dots[i], 0, 228);
        lv_obj_set_style_radius(w_dots[i], 4, 0);
        lv_obj_set_style_border_width(w_dots[i], 0, 0);
        lv_obj_clear_flag(w_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    layout_dots();

    // Last: viz_apply() repositions w_name and hides w_prog, so every widget it
    // touches must already exist. Called earlier (next to the bars, where it
    // reads more naturally) the NULL guards silently skipped that half of the
    // layout, and a radio booting into a visualiser drew the station name on
    // top of the bars with the programme line still showing.
    viz_apply(settings_get()->viz);

    refresh_player();
}

// =====================================================================
// Station list screen
// =====================================================================
static void rebuild_list_rows(void);   // repopulates rows for the current area

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

    // Scrollable rows (populated per-area by rebuild_list_rows)
    s_list_cont = lv_obj_create(s_scr_list);
    lv_obj_set_size(s_list_cont, 320, 210);
    lv_obj_set_pos(s_list_cont, 0, 30);
    lv_obj_set_style_bg_color(s_list_cont, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_list_cont, 0, 0);
    lv_obj_set_style_radius(s_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_list_cont, 0, 0);
    lv_obj_set_style_pad_row(s_list_cont, 0, 0);
    lv_obj_set_flex_flow(s_list_cont, LV_FLEX_FLOW_COLUMN);
    rebuild_list_rows();
}

// (Re)build the station-list rows for the current area. Called at boot and
// whenever the area changes (the active station set changes with it).
static void rebuild_list_rows(void)
{
    if (!s_list_cont) return;
    lv_obj_clean(s_list_cont);   // frees old rows + their labels
    for (int i = 0; i < MAX_STATIONS; i++) s_list_prog[i] = NULL;

    for (int i = 0; i < stations_count(); i++) {
        lv_obj_t *row = lv_obj_create(s_list_cont);
        lv_obj_set_size(row, 320, 54);
        lv_obj_set_style_bg_color(row, lv_color_hex(i & 1 ? C_PANEL : C_BG), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ev_select_station, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Arduino row layout: logo at left; name top-right; programme bottom-right.
        // Pre-scaled asset, blitted 1:1 — no runtime lv_image scale anywhere in
        // the UI (the sw-transform path wedged the LVGL task; see Phase 17).
        lv_obj_t *logo = lv_image_create(row);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_EVENT_BUBBLE);   // taps reach the row
        lv_image_set_src(logo, station_logo_small(i));
        lv_obj_align(logo, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, station_name(i));
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
        lv_label_set_text_fmt(l, "%s %s%s  (%d)", LV_SYMBOL_WIFI,
                              s_aps[i].secure ? "" : "(open) ",
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

    // LVGL's built-in allocator gets ONE fixed pool, carved out of internal RAM
    // at link time (LV_MEM_SIZE_KILOBYTES=64). That pool is the entire UI's
    // budget: every object, style and draw mask comes from it. The player +
    // station list + settings + 47-area dropdown already sit at ~77% of it, so
    // opening WiFi setup and filling a 20-AP scan list pushed it to exactly
    // 100%. lv_malloc then returns NULL — and LVGL's default assert handler is
    // `while(1);`, so the LVGL task froze until the task watchdog rebooted the
    // radio (or a NULL write panicked first, StoreProhibited). The reboot looked
    // instant and the backtrace pointed at innocent draw code; the real fault
    // was simply "out of memory".
    //
    // Growing the internal pool is NOT an option: idf.py size claims ~109 KB
    // spare, but that's static link headroom — at runtime WiFi/LWIP own it and
    // only ~30 KB of internal RAM is actually free, largest block ~10 KB. PSRAM
    // has >2 MB idle, so hand TLSF a second pool there and let it spill. Draw
    // buffers stay internal + DMA-capable (see below); this pool only ever holds
    // object/style/mask metadata, which is small and cache-friendly.
    // NOTE: check the return. lv_mem_add_pool() reports failure only via
    // LV_LOG_WARN, which is compiled out here (LV_USE_LOG off) — so a rejected
    // pool is otherwise SILENT, and the UI just goes back to OOMing under a
    // busy scan. This bit once: TLSF refused a 256 KB pool because
    // LV_MEM_POOL_EXPAND_SIZE was 0, and it looked like it had worked.
    void *spill = heap_caps_malloc(LVGL_PSRAM_POOL_BYTES, MALLOC_CAP_SPIRAM);
    if (spill && lv_mem_add_pool(spill, LVGL_PSRAM_POOL_BYTES)) {
        ESP_LOGI(TAG, "LVGL heap: %u KB internal + %u KB PSRAM spill",
                 (unsigned)(CONFIG_LV_MEM_SIZE_KILOBYTES),
                 (unsigned)(LVGL_PSRAM_POOL_BYTES / 1024));
    } else {
        ESP_LOGE(TAG, "LVGL PSRAM spill pool FAILED (malloc=%p) — UI may OOM "
                      "and hang; check CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES", spill);
        heap_caps_free(spill);
    }

    // Start from persisted settings (Phase 7).
    settings_t *st = settings_get();
    s_cur = (st->station >= 0 && st->station < stations_count()) ? st->station : 0;
    s_vol = st->volume;
    display_set_flipped(st->rotation == 1);   // apply persisted orientation

    // Draw buffers MUST be internal DMA-capable RAM. They lived in PSRAM for a
    // while (sparing internal RAM for TLS, pre-Phase-14): GPSPI can't DMA from
    // PSRAM on the S3, so spi_master silently bounced EVERY flush through a
    // freshly-malloc'd MALLOC_CAP_DMA buffer + a 25 KB memcpy — hidden per-flush
    // cost, and once heap fragmentation dropped the largest internal block below
    // the chunk size the alloc failed and the SCREEN FROZE (tx_color NO_MEM,
    // which the ili9341 driver swallows). With mbedtls in PSRAM since Phase 14
    // there's headroom to do this right: 2×20 lines internal = zero-copy DMA,
    // no allocation in the flush path at all.
    size_t buf_bytes = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t);
    uint8_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    uint8_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    if (!buf1 || !buf2) return ESP_ERR_NO_MEM;

    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) return ESP_ERR_NO_MEM;

    s_disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);   // flush_cb blocks on the DMA itself
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    display_register_flush_ready_cb(color_done_cb, NULL);

    // Visualiser DSP before the player screen builds its bars. A failure here is
    // not fatal: viz_read() then always returns false, the bars stay flat, and
    // the radio plays on — audio never depends on this path.
    if (viz_init() != ESP_OK) ESP_LOGW(TAG, "visualiser disabled (init failed)");

    build_player_screen();
    build_list_screen();
    build_settings_screen();
    build_saver_screen();
    build_splash_screen();
    lv_screen_load(s_scr_splash);   // player follows via ui_splash_done()

    // Failsafe: whatever happens to the boot pipeline, never trap the user
    // on the splash (one-shot, auto-deleted).
    lv_timer_t *fs = lv_timer_create(splash_timer_cb, SPLASH_FAILSAFE_MS, NULL);
    lv_timer_set_repeat_count(fs, 1);

    lv_timer_create(status_timer_cb, 2000, NULL);  // WiFi/status bar refresh
    lv_timer_create(idle_timer_cb, 300, NULL);     // screen dim/off after idle

    // One-shot debounce timer for prev/next (created paused; armed on each press).
    s_commit_timer = lv_timer_create(commit_cb, 450, NULL);
    lv_timer_pause(s_commit_timer);

    s_sleep_timer = lv_timer_create(sleep_fire_cb, 60000, NULL);
    lv_timer_pause(s_sleep_timer);

    s_saver_timer = lv_timer_create(saver_tick_cb, 60, NULL);   // bounce step
    lv_timer_pause(s_saver_timer);

    // 16 KB stack: LVGL v9's image draw/decode path (Phase 13 logos) is much
    // deeper than the label/tile path, and 8 KB could overflow while rendering
    // the 15-logo station list.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "LVGL v%d.%d.%d, %d stations, player UI up",
             lv_version_major(), lv_version_minor(), lv_version_patch(), stations_count());
    return ESP_OK;
}
