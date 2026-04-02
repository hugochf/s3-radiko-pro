/*
 * S3 Radiko Internet Radio
 * ESP32-S3 2.8" IPS Display (ILI9341 + FT6336G touch + ES8311 audio)
 *
 * Features:
 *   - Modern LVGL dark UI with station logos and Japanese text
 *   - Scrollable station list (15 Kanagawa-area stations)
 *   - Volume slider (ES8311 codec control)
 *   - Song/program title from stream metadata
 *   - Screen auto-dim after 60s, touch to wake
 *   - Battery level indicator
 *
 * Board: esp32:esp32:esp32s3  (ESP32-S3 Dev Module or similar)
 * Partition: Huge APP (3MB)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Audio.h>
#include <SD_MMC.h>
#include <base64.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include "touch.h"
#include "es8311.h"
#include "lv_font_jp_16.h"
#include "station_logos.h"
// Full Japanese font compiled in flash
// Full JP font loaded from SD card at runtime
// Gzip decompression via ESP ROM tinfl
extern "C" {
  size_t tinfl_decompress_mem_to_mem(void *pOut, size_t out_len, const void *pSrc, size_t src_len, int flags);
}
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// ============================================================
// CONFIG
// ============================================================
#define WIFI_SSID     "ASUKA JP"
#define WIFI_PASSWORD "SMM27275458"

// ============================================================
// PINS
// ============================================================
#define PIN_BL      45   // LCD backlight (PWM, HIGH=ON)
#define PIN_AMP_EN   1   // Speaker amp FM8002E (LOW=enable)
#define PIN_I2S_MCK  4   // I2S Master Clock → ES8311
#define PIN_I2S_BCK  5   // I2S Bit Clock
#define PIN_I2S_WS   7   // I2S Word Select (LRC)
#define PIN_I2S_OUT  8   // I2S Data Out → ES8311 DAC
#define PIN_RGB_LED 42   // WS2812B RGB LED

// ============================================================
// RADIKO
// ============================================================
static const char* RADIKO_AUTH_KEY =
  "bcd151073c03b352e1ef2fd66c32209da9ca0afa";

struct Station {
  const char*          id;
  const char*          name;   // UTF-8 Japanese
  const char*          abbr;   // short label fallback
  uint32_t             color;  // logo background (RGB hex)
  const lv_img_dsc_t*  logo;   // station logo image
};

static const Station STATIONS[] = {
  {"TBS",     "TBSラジオ",           "TBS",  0xC62828, &logo_TBS},
  {"QRR",     "文化放送",             "文化", 0x1565C0, &logo_QRR},
  {"LFR",     "ニッポン放送",         "LFR",  0x2E7D32, &logo_LFR},
  {"RN1",     "ラジオNIKKEI第1",     "RN1",  0x0277BD, &logo_RN1},
  {"RN2",     "ラジオNIKKEI第2",     "RN2",  0x558B2F, &logo_RN2},
  {"INT",     "interfm",              "INT",  0xE65100, &logo_INT},
  {"FMT",     "TOKYO FM",             "FM",   0x6A1B9A, &logo_FMT},
  {"FMJ",     "J-WAVE",               "JW",   0x00838F, &logo_FMJ},
  {"JORF",    "ラジオ日本",           "RJ",   0x4E342E, &logo_JORF},
  {"BAYFM78", "BAYFM78",              "BAY",  0x00695C, &logo_BAYFM78},
  {"NACK5",   "NACK5",                "N5",   0xAD1457, &logo_NACK5},
  {"YFM",     "FMヨコハマ",            "YFM",  0x283593, &logo_YFM},
  {"IBS",     "LuckyFM\n茨城放送",    "LFM",  0xBF360C, &logo_IBS},
  {"JOAK",    "NHK AM(東京)",         "NHK1", 0x37474F, &logo_JOAK},
  {"JOAK-FM", "NHK FM(東京)",         "NHKF", 0x00695C, &logo_JOAK_FM},
};
#define NUM_STATIONS 15

// ============================================================
// STATE
// ============================================================
// I2C master bus + device handles (shared between ES8311 and FT6336 via touch.h)
i2c_master_bus_handle_t g_i2c_bus    = nullptr;
i2c_master_dev_handle_t g_es8311_dev = nullptr;

static Audio  audio;
static lv_font_t* font_jp_full = nullptr;  // loaded from SD at runtime
static String radikoToken  = "";
static String radikoArea   = "JP14";
static int    currentStn   = 0;
static int    currentVol   = 20;   // 0–100 (ES8311 percentage)
static bool   isPlaying    = false;
static String songTitle    = "";

// ============================================================
// BACKLIGHT / SCREEN TIMEOUT
// ============================================================
#define DIM_TIMEOUT_MS   300000UL  // 5 min → dim screen
#define OFF_TIMEOUT_MS   600000UL  // 10 min → screen off
#define DIM_DUTY     18            // ~7% brightness when dimmed (0–255)
static unsigned long lastTouch  = 0;
static uint8_t       screenState = 0;  // 0=on, 1=dimmed, 2=off

static void bl_set(int duty) { ledcWrite(PIN_BL, duty); }

// ============================================================
// RGB LED (WS2812B on GPIO42) — rainbow effect
// ============================================================
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t region = h / 60;
  uint8_t rem = (h - region * 60) * 255 / 60;
  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * rem / 255)) / 255;
  uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - rem) / 255)) / 255;
  switch (region) {
    case 0: *r=v; *g=t; *b=p; break;
    case 1: *r=q; *g=v; *b=p; break;
    case 2: *r=p; *g=v; *b=t; break;
    case 3: *r=p; *g=q; *b=v; break;
    case 4: *r=t; *g=p; *b=v; break;
    default:*r=v; *g=p; *b=q; break;
  }
}

static void rgb_update() {
  static uint16_t hue = 0;
  uint8_t r, g, b;
  hsv_to_rgb(hue % 360, 255, 255, &r, &g, &b);  // full brightness
  neopixelWrite(PIN_RGB_LED, r, g, b);
  hue += 1;
  if (hue >= 360) hue = 0;
}

// ============================================================
// BATTERY ADC (ADC1_CHANNEL_8 = GPIO9, voltage divider ×2)
// ============================================================
static adc_oneshot_unit_handle_t s_adc  = nullptr;
static adc_cali_handle_t         s_cali = nullptr;

static void bat_init() {
  adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
  adc_oneshot_new_unit(&u, &s_adc);
  adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
  adc_oneshot_config_channel(s_adc, ADC_CHANNEL_8, &c);
  adc_cali_curve_fitting_config_t k = {
    .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
  adc_cali_create_scheme_curve_fitting(&k, &s_cali);
}

// Smoothed battery reading with exponential moving average
static int s_bat_mv_avg = 0;  // smoothed battery voltage in mV

static int bat_read_mv() {
  if (!s_adc) return -1;
  // Average 8 ADC samples to reduce noise
  long sum = 0;
  int good = 0;
  for (int i = 0; i < 8; i++) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_8, &raw) == ESP_OK &&
        s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
      sum += mv;
      good++;
    }
  }
  if (good == 0) return -1;
  int mv_now = (int)(sum / good) * 2;  // voltage divider ×2

  // Exponential moving average (alpha ~0.1) to smooth across calls
  if (s_bat_mv_avg == 0) s_bat_mv_avg = mv_now;  // first reading
  else s_bat_mv_avg = (s_bat_mv_avg * 9 + mv_now) / 10;
  return s_bat_mv_avg;
}

static int bat_pct() {
  int mv = bat_read_mv();
  if (mv < 0) return -1;
  // Li-ion discharge curve (approximate):
  // 4200mV=100%, 3900mV~75%, 3700mV~50%, 3550mV~25%, 3300mV~5%, 3000mV=0%
  if (mv >= 4200) return 100;
  if (mv >= 3900) return 75 + (mv - 3900) * 25 / 300;
  if (mv >= 3700) return 50 + (mv - 3700) * 25 / 200;
  if (mv >= 3550) return 25 + (mv - 3550) * 25 / 150;
  if (mv >= 3300) return 5  + (mv - 3300) * 20 / 250;
  if (mv >= 3000) return mv > 3000 ? (mv - 3000) * 5 / 300 : 0;
  return 0;
}

static bool bat_is_charging() {
  // No hardware charge status pin (TP4054 CHRG drives LED only).
  // Compare current voltage to 30s ago — if rising, likely charging.
  static int history[15] = {};  // ~30s of history (called every ~2s)
  static int idx = 0;
  static bool filled = false;
  int mv = s_bat_mv_avg;

  // Store current reading
  history[idx] = mv;
  idx = (idx + 1) % 15;
  if (idx == 0) filled = true;

  if (!filled) return mv > 4250;  // not enough history yet

  // Find oldest reading
  int oldest = history[idx];  // next slot to write = oldest
  // Charging if voltage rose by >10mV over 30 seconds
  return (mv > oldest + 10) || mv > 4250;
}

// ============================================================
// LVGL – display + touch drivers
// ============================================================
static TFT_eSPI           tft;
static lv_disp_draw_buf_t lv_draw_buf;
static lv_color_t*        lv_px_buf = nullptr;  // allocated in PSRAM to save internal RAM for SSL

static void lv_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)&px->full, w * h, true);
  lv_disp_flush_ready(drv);
}

static void lv_touch(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (touch_touched()) {
    data->state   = LV_INDEV_STATE_PR;
    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
    lastTouch     = millis();
    if (screenState > 0) { screenState = 0; bl_set(255); }
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ============================================================
// RADIKO AUTH + PLAY
// ============================================================
static bool radiko_auth() {
  HTTPClient h;
  h.begin("https://radiko.jp/v2/api/auth1");
  h.addHeader("X-Radiko-App",         "pc_html5");
  h.addHeader("X-Radiko-App-Version", "0.0.1");
  h.addHeader("X-Radiko-User",        "dummy_user");
  h.addHeader("X-Radiko-Device",      "pc");
  // Must call collectHeaders() before GET() to capture response headers
  const char* keys[] = {"X-Radiko-AuthToken", "X-Radiko-KeyLength", "X-Radiko-KeyOffset"};
  h.collectHeaders(keys, 3);
  if (h.GET() != 200) { h.end(); return false; }
  String tok1 = h.header("X-Radiko-AuthToken");
  int klen    = h.header("X-Radiko-KeyLength").toInt();
  int koff    = h.header("X-Radiko-KeyOffset").toInt();
  h.end();

  String pk = base64::encode((const uint8_t*)RADIKO_AUTH_KEY + koff, klen);
  h.begin("https://radiko.jp/v2/api/auth2");
  h.addHeader("X-Radiko-AuthToken",  tok1);
  h.addHeader("X-Radiko-PartialKey", pk);
  h.addHeader("X-Radiko-User",       "dummy_user");
  h.addHeader("X-Radiko-Device",     "pc");
  if (h.GET() != 200) { h.end(); return false; }
  String auth2body = h.getString();
  h.end();

  // Extract area from auth2 response (e.g. "JP14,神奈川県,kanagawa Japan")
  int comma = auth2body.indexOf(',');
  if (comma > 0) radikoArea = auth2body.substring(0, comma);
  radikoArea.trim();

  radikoToken = tok1;
  return true;
}

// Fetch current program info from Radiko API
static String s_prog_title = "";

static void fetch_program_info(const char* station_id) {
  // Raw HTTPS request — full control, no gzip
  NetworkClientSecure tc;
  tc.setInsecure();
  tc.setTimeout(10);
  if (!tc.connect("radiko.jp", 443)) {
    delay(500);
    if (!tc.connect("radiko.jp", 443)) { songTitle = "ERR:conn"; return; }
  }

  String path = "/v3/program/now/" + radikoArea + ".xml";
  tc.print("GET " + path + " HTTP/1.0\r\n"
           "Host: radiko.jp\r\n"
           "Accept-Encoding: gzip\r\n"
           "Connection: close\r\n"
           "\r\n");

  // Read entire response into PSRAM, then split headers/body
  size_t bufCap = 80000;
  char* buf = (char*)ps_malloc(bufCap);
  if (!buf) { tc.stop(); return; }
  size_t bufLen = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 10000 && bufLen < bufCap - 1) {
    if (tc.available()) {
      int n = tc.readBytes(buf + bufLen, min((int)tc.available(), (int)(bufCap - 1 - bufLen)));
      bufLen += n;
      t0 = millis();
    } else if (!tc.connected()) {
      break;
    } else {
      delay(5);
    }
  }
  tc.stop();
  buf[bufLen] = 0;

  // Find header/body boundary
  char* bodyPtr = (char*)memmem(buf, bufLen, "\r\n\r\n", 4);
  if (!bodyPtr) { free(buf); return; }
  *bodyPtr = 0;  // null-terminate headers for searching
  bodyPtr += 4;
  size_t bodyLen = bufLen - (bodyPtr - buf);

  // Check gzip from headers (case-insensitive search in header area)
  for (char* p = buf; *p; p++) *p = tolower(*p);
  bool isGzip = strstr(buf, "gzip") != NULL;
  if (!isGzip && bodyLen > 2 && (uint8_t)bodyPtr[0] == 0x1F && (uint8_t)bodyPtr[1] == 0x8B)
    isGzip = true;
  if (bodyLen == 0) { free(buf); return; }
  int contentLen = 0;  // for debug

  // Decompress gzip into PSRAM
  char* xml = NULL;
  size_t xmlLen = 0;
  if (isGzip && bodyLen > 10) {
    size_t hdr = 10;
    uint8_t flags = (uint8_t)bodyPtr[3];
    if (flags & 0x04) hdr += 2 + (uint8_t)bodyPtr[hdr] + ((uint8_t)bodyPtr[hdr+1] << 8);
    if (flags & 0x08) { while (hdr < bodyLen && bodyPtr[hdr]) hdr++; hdr++; }
    if (flags & 0x10) { while (hdr < bodyLen && bodyPtr[hdr]) hdr++; hdr++; }
    if (flags & 0x02) hdr += 2;

    size_t alloc = 250000;  // decompressed XML ~150KB+
    xml = (char*)ps_malloc(alloc);
    if (xml) {
      // Run decompression in a task with large stack (tinfl needs ~32KB stack)
      struct { char* out; size_t outCap; const char* in; size_t inLen; size_t result; } dctx;
      dctx.out = xml; dctx.outCap = alloc;
      dctx.in = bodyPtr + hdr; dctx.inLen = bodyLen - hdr - 8;
      dctx.result = TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
      TaskHandle_t th = NULL;
      xTaskCreatePinnedToCore([](void* p) {
        auto* d = (decltype(dctx)*)p;
        d->result = tinfl_decompress_mem_to_mem(d->out, d->outCap, d->in, d->inLen, 0);
        vTaskDelete(NULL);
      }, "gunzip", 40960, &dctx, 1, &th, 1);
      while (eTaskGetState(th) != eDeleted) delay(10);
      xmlLen = dctx.result;
      if (xmlLen == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) { free(xml); xml = NULL; }
      else xml[xmlLen] = 0;
    }
  } else {
    // Not gzip — body is already XML
    xml = (char*)ps_malloc(bodyLen + 1);
    if (xml) { memcpy(xml, bodyPtr, bodyLen); xml[bodyLen] = 0; xmlLen = bodyLen; }
  }
  free(buf);  // done with response data
  if (!xml) return;

  // Search for station and extract program info (all in PSRAM)
  char tag[32];
  snprintf(tag, sizeof tag, "id=\"%s\"", station_id);
  char* stn = strstr(xml, tag);
  if (!stn) {
    free(xml); return;
  }

  char* prog = strstr(stn, "<prog ");
  char* progEnd = prog ? strstr(prog, "</prog>") : NULL;
  if (!prog || !progEnd) { free(xml); return; }
  *progEnd = 0;  // null-terminate prog block

  char* ts = strstr(prog, "<title>");
  char* te = strstr(prog, "</title>");
  char* ps = strstr(prog, "<pfm>");
  char* pe = strstr(prog, "</pfm>");

  if (ts && te && te > ts) {
    ts += 7;
    *te = 0;
    songTitle = ts;
    if (ps && pe && pe > ps) {
      ps += 5;
      *pe = 0;
      songTitle += "  ";
      songTitle += ps;
    }
  }
  free(xml);
}

// Forward declarations
static void show_status(const char* msg);
static void hide_status();
static void refresh_playing();
static lv_obj_t *scr_play  = nullptr;
static lv_obj_t *wi_title  = nullptr;

// Debounced station selection
static uint32_t s_pending_connect_ms = 0;
static int      s_pending_stn        = -1;

static void play_stn(int idx) {
  audio.stopSong();  // stop immediately for smooth UI navigation
  isPlaying = false;
  currentStn = idx;
  songTitle = STATIONS[idx].name;
  s_pending_stn = idx;
  s_pending_connect_ms = millis();
}

static void do_connect(int idx) {
  currentStn = idx;
  songTitle = "Connecting...";
  refresh_playing();
  show_status("Connecting...");
  lv_task_handler();
  lv_refr_now(NULL);  // force pixels to display before blocking SSL

  audio.stopSong();

  // Fetch program info while audio is stopped (reuses SSL session from auth)
  fetch_program_info(STATIONS[idx].id);

  String hdr = "X-Radiko-AuthToken: " + radikoToken + "\r\n";
  audio.setExtraHeaders(hdr.c_str());
  String url = "https://f-radiko.smartstream.ne.jp/";
  url += STATIONS[idx].id;
  url += "/_definst_/simul-stream.stream/chunklist.m3u8";
  audio.connecttohost(url.c_str());

  hide_status();
  isPlaying = true;
  if (songTitle == "Connecting..." || songTitle.length() == 0)
    songTitle = STATIONS[idx].name;  // fallback if fetch failed
  s_pending_stn = -1;
  s_pending_connect_ms = 0;
}

static void stop_stn() {
  audio.stopSong();
  isPlaying = false;
  s_pending_stn = -1;
  s_pending_connect_ms = 0;
}


// Audio info message from library → main loop for display
static volatile bool     s_audio_msg_fresh = false;
static char              s_audio_msg[128]  = {};

// ESP32-audioI2S weak-function callbacks
void audio_info(const char* info) {
  Serial.printf("[audio] %s\n", info);
  // Mirror every message to screen so we can see what the library reports
  strncpy(s_audio_msg, info, sizeof(s_audio_msg) - 1);
  s_audio_msg[sizeof(s_audio_msg) - 1] = '\0';
  s_audio_msg_fresh = true;

  String s(info);
  int colon = s.indexOf(':');
  if (colon < 0) return;
  String key = s.substring(0, colon);
  String val = s.substring(colon + 1); val.trim();
  if (key == "StreamTitle" || key == "Title" || key == "icy-name") {
    songTitle = val;
  }
}
void audio_showstation(const char* info)    { Serial.printf("[station] %s\n", info); }
void audio_showstreamtitle(const char* info){ Serial.printf("[title] %s\n", info); }
void audio_eof_stream(const char* info)     { Serial.printf("[eof] %s\n", info); }

// ============================================================
// LVGL UI – widget handles
// ============================================================
static lv_obj_t *scr_list  = nullptr;

// Playing screen widgets
static lv_obj_t *wi_wifi   = nullptr;  // status bar: WiFi icon
static lv_obj_t *wi_bat    = nullptr;  // status bar: battery
static lv_obj_t *wi_logo   = nullptr;  // station logo box
static lv_obj_t *wi_logo_img = nullptr; // station logo image
static lv_obj_t *wi_slider = nullptr;  // volume slider
static lv_obj_t *wi_vol    = nullptr;  // volume value label
static lv_obj_t *wi_play   = nullptr;  // play/pause button label
static lv_obj_t *wi_dots[NUM_STATIONS] = {};

// Colour palette (dark modern)
#define C_BG        0x1A1A2E
#define C_PANEL     0x16213E
#define C_ACCENT    0x0F3460
#define C_HL        0xE94560
#define C_TEXT      0xEAEAEA
#define C_DIM       0x8888AA
#define C_TRACK     0x2E2E5E

// ============================================================
// EVENT CALLBACKS
// ============================================================
static void refresh_playing();

static void ev_prev(lv_event_t*) {
  currentStn = (currentStn + NUM_STATIONS - 1) % NUM_STATIONS;
  play_stn(currentStn);
  refresh_playing();
}
static void ev_next(lv_event_t*) {
  currentStn = (currentStn + 1) % NUM_STATIONS;
  play_stn(currentStn);
  refresh_playing();
}
static void ev_play(lv_event_t*) {
  if (isPlaying) stop_stn();
  else           play_stn(currentStn);
  lv_label_set_text(wi_play, isPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}
static void ev_vol_changed(lv_event_t*) {
  currentVol = (int)lv_slider_get_value(wi_slider);
  char b[8]; snprintf(b, sizeof b, "%d", currentVol);
  lv_label_set_text(wi_vol, b);
  es8311_voice_volume_set(g_es8311_dev, currentVol, nullptr);
}
static void ev_show_list(lv_event_t*) {
  audio.stopSong();
  isPlaying = false;
  s_pending_stn = -1;       // cancel any pending connect
  s_pending_connect_ms = 0;
  lv_scr_load(scr_list);
}
static void ev_back(lv_event_t*) {
  lv_scr_load(scr_play);
  // Resume playing current station if audio was stopped when opening list
  if (!isPlaying) {
    play_stn(currentStn);
    s_pending_connect_ms = millis() - 2000;  // connect immediately
  }
}
static void ev_select_stn(lv_event_t *e) {
  int idx = (int)(uintptr_t)lv_event_get_user_data(e);
  currentStn = idx;
  play_stn(idx);
  refresh_playing();
  lv_scr_load(scr_play);
}

// ============================================================
// REFRESH PLAYING SCREEN
// ============================================================
static void refresh_playing() {
  if (!scr_play) return;
  const Station& s = STATIONS[currentStn];
  if (wi_logo_img && s.logo) lv_img_set_src(wi_logo_img, s.logo);
  lv_label_set_text(wi_title, songTitle.isEmpty() ? "---" : songTitle.c_str());
  lv_label_set_text(wi_play, isPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  lv_slider_set_value(wi_slider, currentVol, LV_ANIM_OFF);
  char b[8]; snprintf(b, sizeof b, "%d", currentVol);
  lv_label_set_text(wi_vol, b);
  for (int i = 0; i < NUM_STATIONS; i++) {
    lv_obj_set_style_bg_color(wi_dots[i],
      (i == currentStn) ? lv_color_hex(C_HL) : lv_color_hex(0x3A3A5A), 0);
  }
}

static void update_status() {
  if (!wi_wifi) return;
  lv_label_set_text(wi_wifi,
    WiFi.status() == WL_CONNECTED ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
  int pct = bat_pct();
  if (pct >= 0) {
    bool charging = bat_is_charging();
    const char* icon;
    if (charging) {
      icon = LV_SYMBOL_CHARGE;
    } else {
      icon = pct > 75 ? LV_SYMBOL_BATTERY_FULL :
             pct > 50 ? LV_SYMBOL_BATTERY_3    :
             pct > 25 ? LV_SYMBOL_BATTERY_2    :
                        LV_SYMBOL_BATTERY_EMPTY;
    }
    char b[16]; snprintf(b, sizeof b, "%s%d%%", icon, pct > 100 ? 100 : pct);
    lv_label_set_text(wi_bat, b);
  }
}

// ============================================================
// LVGL – status popup (shown during init)
// ============================================================
static lv_obj_t *popup     = nullptr;
static lv_obj_t *popup_lbl = nullptr;

static void show_status(const char* msg) {
  if (!popup) {
    popup = lv_obj_create(scr_play);
    lv_obj_set_size(popup, 250, 44);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(popup, 10, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    popup_lbl = lv_label_create(popup);
    lv_obj_set_style_text_color(popup_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(popup_lbl);
  }
  lv_label_set_text(popup_lbl, msg);
  lv_obj_move_foreground(popup);
  lv_task_handler();
}

static void hide_status() {
  if (popup) { lv_obj_del(popup); popup = nullptr; popup_lbl = nullptr; }
}

// ============================================================
// BUILD PLAYING SCREEN
// ============================================================
static void build_playing_screen() {
  scr_play = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_play, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr_play, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr_play, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Status bar ----
  lv_obj_t *bar = lv_obj_create(scr_play);
  lv_obj_set_size(bar, 320, 24);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 2, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  wi_wifi = lv_label_create(bar);
  lv_label_set_text(wi_wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wi_wifi, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_wifi, &lv_font_montserrat_12, 0);
  lv_obj_align(wi_wifi, LV_ALIGN_LEFT_MID, 4, 0);

  lv_obj_t *hdr_lbl = lv_label_create(bar);
  lv_label_set_text(hdr_lbl, "Radiko Radio");
  lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(hdr_lbl, &lv_font_montserrat_12, 0);
  lv_obj_align(hdr_lbl, LV_ALIGN_CENTER, 0, 0);

  wi_bat = lv_label_create(bar);
  lv_label_set_text(wi_bat, LV_SYMBOL_BATTERY_FULL);
  lv_obj_set_style_text_color(wi_bat, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_bat, &lv_font_montserrat_12, 0);
  lv_obj_align(wi_bat, LV_ALIGN_RIGHT_MID, -4, 0);

  // ---- Station logo (216×54, centered, tappable → list) ----
  wi_logo = lv_obj_create(scr_play);
  lv_obj_set_size(wi_logo, 220, 58);
  lv_obj_align(wi_logo, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_style_bg_opa(wi_logo, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wi_logo, 0, 0);
  lv_obj_set_style_pad_all(wi_logo, 0, 0);
  lv_obj_set_style_shadow_width(wi_logo, 0, 0);
  lv_obj_clear_flag(wi_logo, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(wi_logo, ev_show_list, LV_EVENT_CLICKED, NULL);

  wi_logo_img = lv_img_create(wi_logo);
  lv_img_set_src(wi_logo_img, STATIONS[0].logo);
  lv_obj_center(wi_logo_img);

  // ---- Play title (station name / program title) ----
  wi_title = lv_label_create(scr_play);
  lv_label_set_text(wi_title, STATIONS[0].name);
  lv_obj_set_style_text_color(wi_title, lv_color_hex(C_DIM), 0);
  // Original font with SD kanji fallback (set after SD loads in setup)
  lv_obj_set_style_text_font(wi_title, &lv_font_jp_16, 0);
  lv_obj_set_width(wi_title, 300);
  lv_label_set_long_mode(wi_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_anim_speed(wi_title, 30, 0);
  lv_obj_set_style_text_align(wi_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(wi_title, LV_ALIGN_TOP_MID, 0, 98);

  // ---- Volume row ----
  lv_obj_t *vrow = lv_obj_create(scr_play);
  lv_obj_set_size(vrow, 320, 30);
  lv_obj_set_pos(vrow, 0, 130);
  lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(vrow, 0, 0);
  lv_obj_set_style_pad_all(vrow, 0, 0);
  lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *vlbl = lv_label_create(vrow);
  lv_label_set_text(vlbl, "VOL");
  lv_obj_set_style_text_color(vlbl, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_text_font(vlbl, &lv_font_montserrat_12, 0);
  lv_obj_align(vlbl, LV_ALIGN_LEFT_MID, 8, 0);

  wi_slider = lv_slider_create(vrow);
  lv_obj_set_size(wi_slider, 214, 8);
  lv_obj_align(wi_slider, LV_ALIGN_LEFT_MID, 46, 0);
  lv_slider_set_range(wi_slider, 0, 100);
  lv_slider_set_value(wi_slider, currentVol, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(wi_slider, lv_color_hex(C_TRACK),   LV_PART_MAIN);
  lv_obj_set_style_bg_color(wi_slider, lv_color_hex(C_HL),      LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(wi_slider, lv_color_hex(C_HL),      LV_PART_KNOB);
  lv_obj_set_style_pad_all (wi_slider, 6,                       LV_PART_KNOB);
  lv_obj_add_event_cb(wi_slider, ev_vol_changed, LV_EVENT_VALUE_CHANGED, NULL);

  wi_vol = lv_label_create(vrow);
  char vb[8]; snprintf(vb, sizeof vb, "%d", currentVol);
  lv_label_set_text(wi_vol, vb);
  lv_obj_set_style_text_color(wi_vol, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_vol, &lv_font_montserrat_12, 0);
  lv_obj_align(wi_vol, LV_ALIGN_RIGHT_MID, -8, 0);

  // ---- Circle control buttons ----
  int btn_y = 170;  // vertical center for buttons

  // Prev (small circle)
  lv_obj_t *btn_prev = lv_btn_create(scr_play);
  lv_obj_set_size(btn_prev, 44, 44);
  lv_obj_set_pos(btn_prev, 50, btn_y);
  lv_obj_set_style_radius(btn_prev, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_prev, lv_color_hex(C_ACCENT), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_prev, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_prev, 0, 0);
  lv_obj_add_event_cb(btn_prev, ev_prev, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_prev = lv_label_create(btn_prev);
  lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
  lv_obj_set_style_text_color(lbl_prev, lv_color_hex(C_TEXT), 0);
  lv_obj_center(lbl_prev);

  // Play/Pause (big circle)
  lv_obj_t *btn_play = lv_btn_create(scr_play);
  lv_obj_set_size(btn_play, 56, 56);
  lv_obj_set_pos(btn_play, 132, btn_y - 6);
  lv_obj_set_style_radius(btn_play, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(C_HL), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(0xC03050), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_play, 0, 0);
  lv_obj_add_event_cb(btn_play, ev_play, LV_EVENT_CLICKED, NULL);
  wi_play = lv_label_create(btn_play);
  lv_label_set_text(wi_play, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(wi_play, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_play, &lv_font_montserrat_16, 0);
  lv_obj_center(wi_play);

  // Next (small circle)
  lv_obj_t *btn_next = lv_btn_create(scr_play);
  lv_obj_set_size(btn_next, 44, 44);
  lv_obj_set_pos(btn_next, 226, btn_y);
  lv_obj_set_style_radius(btn_next, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_next, lv_color_hex(C_ACCENT), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_next, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_next, 0, 0);
  lv_obj_add_event_cb(btn_next, ev_next, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_next = lv_label_create(btn_next);
  lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
  lv_obj_set_style_text_color(lbl_next, lv_color_hex(C_TEXT), 0);
  lv_obj_center(lbl_next);

  // ---- Station position dots ----
  lv_obj_t *drow = lv_obj_create(scr_play);
  int dot_w = NUM_STATIONS * 8 + (NUM_STATIONS - 1) * 4;
  lv_obj_set_size(drow, 320, 12);
  lv_obj_set_pos(drow, 0, 224);
  lv_obj_set_style_bg_opa(drow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(drow, 0, 0);
  lv_obj_set_style_pad_all(drow, 0, 0);
  lv_obj_clear_flag(drow, LV_OBJ_FLAG_SCROLLABLE);
  int dx = (320 - dot_w) / 2;
  for (int i = 0; i < NUM_STATIONS; i++) {
    wi_dots[i] = lv_obj_create(drow);
    lv_obj_set_size(wi_dots[i], 8, 8);
    lv_obj_set_pos(wi_dots[i], dx + i * 12, 2);
    lv_obj_set_style_radius(wi_dots[i], 4, 0);
    lv_obj_set_style_bg_color(wi_dots[i],
      (i == 0) ? lv_color_hex(C_HL) : lv_color_hex(0x3A3A5A), 0);
    lv_obj_set_style_bg_opa(wi_dots[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wi_dots[i], 0, 0);
  }
}

// ============================================================
// BUILD STATION LIST SCREEN
// ============================================================
static void build_list_screen() {
  scr_list = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_list, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr_list, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr_list, LV_OBJ_FLAG_SCROLLABLE);

  // Header bar
  lv_obj_t *hdr = lv_obj_create(scr_list);
  lv_obj_set_size(hdr, 320, 30);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 4, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(hdr);
  lv_obj_set_size(back, 80, 26);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, -2, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_add_event_cb(back, ev_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *blbl = lv_label_create(back);
  lv_label_set_text(blbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(blbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(blbl, &lv_font_montserrat_14, 0);
  lv_obj_center(blbl);

  lv_obj_t *ttl = lv_label_create(hdr);
  lv_label_set_text(ttl, "Select Station");
  lv_obj_set_style_text_color(ttl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, 0);
  lv_obj_align(ttl, LV_ALIGN_CENTER, 24, 0);

  // Scrollable list container
  lv_obj_t *cont = lv_obj_create(scr_list);
  lv_obj_set_size(cont, 320, 210);
  lv_obj_set_pos(cont, 0, 30);
  lv_obj_set_style_bg_color(cont, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_style_pad_row(cont, 0, 0);
  lv_obj_set_style_pad_column(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  for (int i = 0; i < NUM_STATIONS; i++) {
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, 320, 54);
    lv_obj_set_style_bg_color(row,
      (i % 2 == 0) ? lv_color_hex(C_BG) : lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, ev_select_stn, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    // Station logo (scaled to fit row)
    lv_obj_t *logo = lv_img_create(row);
    lv_img_set_src(logo, STATIONS[i].logo);
    lv_img_set_zoom(logo, 200);  // scale 216x54 → ~169x42
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, -6, 0);

    // Station full name, right-aligned
    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, STATIONS[i].name);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_jp_16, 0);
    lv_obj_set_width(name, 140);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(name, LV_ALIGN_RIGHT_MID, -6, 0);
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // I2C master bus for ES8311 + FT6336G (shared bus, SDA=16, SCL=15)
  {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port              = I2C_NUM_0;
    bus_cfg.sda_io_num            = (gpio_num_t)16;
    bus_cfg.scl_io_num            = (gpio_num_t)15;
    bus_cfg.clk_source            = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt     = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &g_i2c_bus));

    i2c_device_config_t es_cfg = {};
    es_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    es_cfg.device_address  = ES8311_ADDRESS_0;
    es_cfg.scl_speed_hz    = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(g_i2c_bus, &es_cfg, &g_es8311_dev));
  }

  // TFT display – init BEFORE ledcAttach so tft.init()'s pinMode(TFT_BL)
  // does not detach the LEDC channel we attach right after.
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // Backlight PWM – attach AFTER tft.init() which calls pinMode(TFT_BL)
  ledcAttach(PIN_BL, 5000, 8);
  bl_set(255);

  // Capacitive touch
  touch_init(tft.width(), tft.height(), tft.getRotation());

  // Speaker amplifier ON
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, LOW);

  // ES8311 codec – first pass (MCLK not yet running; sets up registers)
  // Audio library outputs at 48 kHz → MCLK = 256 × 48000 = 12,288,000 Hz
  {
    es8311_clock_config_t clk = {};
    clk.mclk_from_mclk_pin = true;
    clk.mclk_frequency     = 12288000;  // 256 * 48000 Hz
    clk.sample_frequency   = 48000;
    es8311_init(g_es8311_dev, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  }
  es8311_voice_volume_set(g_es8311_dev, currentVol, nullptr);
  es8311_voice_mute(g_es8311_dev, false);  // explicit DAC unmute

  // Battery ADC
  bat_init();

  // LVGL — display buffer in PSRAM to keep internal RAM free for SSL
  lv_px_buf = (lv_color_t*)ps_malloc(320 * 30 * sizeof(lv_color_t));
  lv_init();
  lv_disp_draw_buf_init(&lv_draw_buf, lv_px_buf, NULL, 320 * 30);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = 320;
  disp_drv.ver_res  = 240;
  disp_drv.flush_cb = lv_flush;
  disp_drv.draw_buf = &lv_draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lv_touch;
  lv_indev_drv_register(&indev_drv);

  // SD card init moved after WiFi — see below
  // SD status shown via songTitle after UI builds

  // Build UI
  build_playing_screen();
  lv_scr_load(scr_play);
  lv_task_handler();

  // WiFi
  show_status("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    lv_task_handler(); delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) { show_status("WiFi failed!"); return; }

  // Radiko auth
  show_status("Authenticating Radiko...");
  lv_task_handler();
  if (!radiko_auth()) { show_status("Auth failed!"); return; }

  // Load full Japanese font from SD card
  SD_MMC.setPins(38, 40, 39);  // CLK, CMD, D0
  if (SD_MMC.begin("/sdcard", true)) {  // 1-bit SDIO
    if (SD_MMC.exists("/lv_font_jp_full.bin")) {
      font_jp_full = lv_font_load("S:lv_font_jp_full.bin");
    }
    show_status(font_jp_full ? "SD Font: OK" : "SD Font: FAIL");
    if (font_jp_full) {
      ((lv_font_t*)&lv_font_jp_16)->fallback = font_jp_full;
    }
  } else {
    show_status("No SD card");
  }
  lv_task_handler();
  delay(1000);
  hide_status();

  // ESP32-audioI2S setup – PSRAM is mandatory for this library.
  // In Arduino IDE: Tools → PSRAM → "OPI PSRAM"  (for ESP32-S3 with 8MB OPI PSRAM)
  if (!psramFound()) {
    show_status("No PSRAM! Tools->PSRAM->OPI PSRAM");
    for (;;) { lv_task_handler(); delay(10); }
  }
  audio.setPinout(PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_OUT, PIN_I2S_MCK);
  audio.setVolume(21);           // max soft vol; ES8311 handles actual level
  audio.setOutput48KHz(true);    // lock I2S at 48 kHz so MCLK stays 12,288,000 Hz
  audio.setConnectionTimeout(250, 10000); // 10s SSL timeout (ESP32-S3 SSL is slow)
  audio.setAudioTaskCore(1);     // PeriodicTask on Core 1 (same as audio.loop task)

  // ES8311 second pass – MCLK is now running at 12,288,000 Hz (256×48000);
  // re-init so the clock state machine locks to the actual MCLK signal.
  delay(50);

  {
    es8311_clock_config_t clk = {};
    clk.mclk_from_mclk_pin = true;
    clk.mclk_frequency     = 12288000;  // 256 * 48000 Hz
    clk.sample_frequency   = 48000;
    es8311_init(g_es8311_dev, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  }
  es8311_voice_volume_set(g_es8311_dev, currentVol, nullptr);
  es8311_voice_mute(g_es8311_dev, false);

  // Build list screen
  build_list_screen();

  // Connect to first station
  hide_status();
  refresh_playing();
  do_connect(currentStn);
  refresh_playing();

  lastTouch = millis();
}

// ============================================================
// LOOP (single-core: audio + LVGL on same core)
// ============================================================
void loop() {
  audio.loop();
  lv_task_handler();

  // Debounced station connect: 1s after last button press
  if (s_pending_stn >= 0 && s_pending_connect_ms > 0 &&
      millis() - s_pending_connect_ms >= 1500) {
    do_connect(s_pending_stn);
    refresh_playing();
  }

  // Screen timeout
  // Screen timeout: 5min→dim, 10min→off
  uint32_t idle = millis() - lastTouch;
  if (screenState == 0 && idle > DIM_TIMEOUT_MS) {
    screenState = 1;
    bl_set(DIM_DUTY);
  } else if (screenState == 1 && idle > OFF_TIMEOUT_MS) {
    screenState = 2;
    bl_set(0);
  }

  // Rainbow RGB LED
  static uint32_t last_rgb = 0;
  if (millis() - last_rgb > 30) {  // ~33 updates/sec for smooth rainbow
    last_rgb = millis();
    rgb_update();
  }

  // Periodic status bar + song title (~2s)
  static uint32_t last_sbar = 0;
  if (millis() - last_sbar > 2000) {
    last_sbar = millis();
    if (lv_scr_act() == scr_play) {
      update_status();
      if (wi_title) {
        if (songTitle.length() > 0)
          lv_label_set_text(wi_title, songTitle.c_str());
        else if (isPlaying)
          lv_label_set_text(wi_title, STATIONS[currentStn].name);
      }
    }
  }
}
