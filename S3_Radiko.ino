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
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Audio.h>
#include <Preferences.h>
#include <base64.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include "touch.h"
#include "es8311.h"
#include "lv_font_jp_16.h"
#include "station_logos.h"
// Full Japanese font compiled in flash
LV_FONT_DECLARE(lv_font_jp_full);
// Gzip decompression via ESP ROM tinfl
extern "C" {
  size_t tinfl_decompress_mem_to_mem(void *pOut, size_t out_len, const void *pSrc, size_t src_len, int flags);
}
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// ============================================================
// CONFIG
// ============================================================
// Default WiFi (used if no saved credentials)
#define WIFI_SSID_DEFAULT     "aio_jp"
#define WIFI_PASSWORD_DEFAULT "Hugo1983522"

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
static const lv_font_t* font_jp_full = &lv_font_jp_full;
static String radikoToken  = "";
static String radikoArea   = "JP14";
static int    currentStn   = 0;
static int    currentVol   = 20;   // 0–100 (ES8311 percentage)
static bool   isPlaying    = false;
static int    ledMode      = 0;     // 0=rainbow breath, 1=ocean, 2=sunset, 3=candle, 4=rainbow, 5=pulse, 6=off
static uint32_t sleepTimer  = 0;    // 0=off, else millis when timer expires
static int      sleepMins   = 0;    // display: 0/30/60/90
static Preferences prefs;
static String wifiSSID     = "";
static String wifiPass     = "";
static String songTitle    = "";
// Cached program titles for all stations (filled by fetch_all_program_info)
static String stationProgs[15];  // NUM_STATIONS, defined later
static volatile bool s_progs_updated = false;  // signal: list UI should refresh

// ============================================================
// BACKLIGHT / SCREEN TIMEOUT
// ============================================================
#define DIM_DUTY     18            // ~7% brightness when dimmed (0–255)
static uint32_t dim_timeout_ms = 300000UL;  // 5 min (overridden from prefs)
static uint32_t off_timeout_ms = 600000UL;  // 10 min (overridden from prefs)
static uint8_t  scr_rotation  = 1;          // 1 = normal landscape, 3 = flipped 180°
static uint8_t  bl_level      = 3;          // 0=Low 1=Mid 2=High 3=Full (overridden from prefs)
static const uint8_t bl_duty_levels[] = {30, 80, 160, 255};
static const char*   bl_names[]       = {"Low", "Mid", "High", "Full"};
#define FW_VERSION "S3 Radiko v1.0"
static unsigned long lastTouch  = 0;
static uint8_t       screenState = 0;  // 0=on, 1=dimmed, 2=off

static void bl_set(int duty) { ledcWrite(PIN_BL, duty); }

// Apply current brightness level to backlight + persist
static void apply_brightness() {
  bl_set(bl_duty_levels[bl_level]);
  prefs.putUChar("bl", bl_level);
}

// ============================================================
// RGB LED (WS2812B on GPIO42) — breathing effect
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

#define LED_MODES 7
static const char* ledModeNames[] = {"Rainbow", "Ocean", "Sunset", "Candle", "Cycle", "Pulse", "OFF"};

static void rgb_update() {
  static uint16_t phase = 0;
  phase++;
  uint8_t r = 0, g = 0, b = 0;
  float breath;

  switch (ledMode) {
    case 0:  // Rainbow Breathing
      breath = (1.0f - cosf(phase * 2.0f * M_PI / 256.0f)) / 2.0f;
      hsv_to_rgb((phase / 3) % 360, 255, (uint8_t)(breath * 255), &r, &g, &b);
      break;
    case 1:  // Ocean — blue/cyan breathing
      breath = (1.0f - cosf(phase * 2.0f * M_PI / 200.0f)) / 2.0f;
      r = 0; g = (uint8_t)(breath * 80); b = (uint8_t)(breath * 255);
      break;
    case 2:  // Sunset — warm red/orange breathing
      breath = (1.0f - cosf(phase * 2.0f * M_PI / 200.0f)) / 2.0f;
      r = (uint8_t)(breath * 255); g = (uint8_t)(breath * 80); b = 0;
      break;
    case 3:  // Candle — warm flicker
      { uint8_t flicker = random(120, 255);
        r = flicker; g = flicker / 3; b = 0; }
      break;
    case 4:  // Rainbow Cycle — smooth, no breathing
      hsv_to_rgb((phase / 2) % 360, 255, 180, &r, &g, &b);
      break;
    case 5:  // Pulse — quick bright flash then fade
      { uint16_t p = phase % 120;
        uint8_t v = p < 10 ? 255 : (p < 60 ? 255 - (p - 10) * 5 : 0);
        r = v; g = v; b = v; }
      break;
    case 6:  // OFF
      break;
  }
  neopixelWrite(PIN_RGB_LED, r, g, b);
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
    if (screenState > 0) { screenState = 0; bl_set(bl_duty_levels[bl_level]); }
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

// Extract program title (and optional pfm) for one station from already-decompressed XML.
// Non-mutating; safe to call repeatedly across stations.
static void parse_one_station(const char* xml, const char* station_id, String& out) {
  char tag[40];
  snprintf(tag, sizeof tag, "id=\"%s\"", station_id);
  const char* stn = strstr(xml, tag);
  if (!stn) { out = ""; return; }

  const char* prog    = strstr(stn,  "<prog ");
  const char* progEnd = prog ? strstr(prog, "</prog>") : NULL;
  if (!prog || !progEnd) { out = ""; return; }

  const char* ts = strstr(prog, "<title>");
  if (!ts || ts >= progEnd) { out = ""; return; }
  ts += 7;
  const char* te = strstr(ts, "</title>");
  if (!te || te >= progEnd) { out = ""; return; }

  // Copy title into temp buffer (avoids mutating shared XML)
  char tmp[220];
  size_t len = te - ts;
  if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
  memcpy(tmp, ts, len);
  tmp[len] = 0;
  out = tmp;

  // Append <pfm> (DJ name) if present within prog block
  const char* ps = strstr(prog, "<pfm>");
  if (ps && ps < progEnd) {
    ps += 5;
    const char* pe = strstr(ps, "</pfm>");
    if (pe && pe < progEnd && pe > ps) {
      size_t plen = pe - ps;
      if (plen >= sizeof(tmp)) plen = sizeof(tmp) - 1;
      memcpy(tmp, ps, plen);
      tmp[plen] = 0;
      if (plen > 0) {
        out += "  ";
        out += tmp;
      }
    }
  }
}

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

  // Parse program info for ALL stations in one pass — XML covers the whole area
  for (int i = 0; i < NUM_STATIONS; i++) {
    parse_one_station(xml, STATIONS[i].id, stationProgs[i]);
  }
  s_progs_updated = true;

  // Update songTitle for the currently-playing station
  if (currentStn >= 0 && currentStn < NUM_STATIONS && stationProgs[currentStn].length() > 0) {
    songTitle = stationProgs[currentStn];
  }

  free(xml);
}

// Forward declarations
static void show_status(const char* msg);
static void hide_status();
static void refresh_playing();
static void build_wifi_screen();
static void build_settings_screen();
static void refresh_settings_info();

// Compute LVGL zoom value (256 = 1.0x) so the image fits target_h tall
// while preserving aspect ratio AND not exceeding max_w wide.
static int compute_fit_zoom(const lv_img_dsc_t* img, int target_h, int max_w) {
  if (!img || img->header.h == 0 || img->header.w == 0) return 256;
  int zh = (target_h * 256) / img->header.h;
  int zw = (max_w   * 256) / img->header.w;
  return zh < zw ? zh : zw;
}
// WiFi setup screen variables (defined later, needed by lambda)
static lv_obj_t *scr_wifi;
static volatile bool wifi_setup_done;
static lv_obj_t *scr_play  = nullptr;
static lv_obj_t *wi_name   = nullptr;
static lv_obj_t *wi_title  = nullptr;

// Debounced station selection
static uint32_t s_pending_connect_ms = 0;
static int      s_pending_stn        = -1;
static int      s_fetch_station      = -1;  // station to fetch program info for

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

  // Connect audio first — user hears radio quickly
  String hdr = "X-Radiko-AuthToken: " + radikoToken + "\r\n";
  audio.setExtraHeaders(hdr.c_str());
  String url = "https://f-radiko.smartstream.ne.jp/";
  url += STATIONS[idx].id;
  url += "/_definst_/simul-stream.stream/chunklist.m3u8";
  audio.connecttohost(url.c_str());

  hide_status();
  isPlaying = true;
  songTitle = STATIONS[idx].name;
  s_pending_stn = -1;
  s_pending_connect_ms = 0;
  s_fetch_station = idx;  // signal loop to fetch program info
  prefs.putInt("stn", idx);
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
static lv_obj_t *scr_settings = nullptr;  // settings/info screen
static lv_obj_t *list_prog_lbls[NUM_STATIONS] = {};  // program info labels per row
// Settings screen dynamic labels (refreshed on open)
static lv_obj_t *set_lbl_wifi   = nullptr;
static lv_obj_t *set_lbl_ip     = nullptr;
static lv_obj_t *set_lbl_area   = nullptr;
static lv_obj_t *set_lbl_bat    = nullptr;
static lv_obj_t *set_lbl_uptime = nullptr;
static lv_obj_t *set_lbl_mem    = nullptr;
static lv_obj_t *set_sl_dim     = nullptr;
static lv_obj_t *set_sl_off     = nullptr;
static lv_obj_t *set_sl_sleep   = nullptr;
static lv_obj_t *set_sl_bl      = nullptr;
static lv_obj_t *set_val_dim    = nullptr;
static lv_obj_t *set_val_off    = nullptr;
static lv_obj_t *set_val_sleep  = nullptr;
static lv_obj_t *set_val_bl     = nullptr;

// Playing screen widgets
static lv_obj_t *wi_wifi   = nullptr;  // status bar: WiFi icon
static lv_obj_t *wi_bat    = nullptr;  // status bar: battery
static lv_obj_t *wi_clock  = nullptr;  // status bar: clock
static lv_obj_t *wi_logo   = nullptr;  // station logo box
static lv_obj_t *wi_logo_img = nullptr; // station logo image
static lv_obj_t *wi_slider = nullptr;  // volume slider
static lv_obj_t *wi_vol    = nullptr;  // volume value label
static lv_obj_t *wi_play   = nullptr;  // play/pause button label
static lv_obj_t *wi_led_btn = nullptr; // LED toggle button label
static lv_obj_t *wi_sleep_btn = nullptr; // sleep timer label
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
static void do_power_off() {
  // Stop everything
  audio.stopSong();
  isPlaying = false;
  neopixelWrite(PIN_RGB_LED, 0, 0, 0);

  // Show power off message
  show_status("Powering off...");
  lv_task_handler();
  delay(1000);

  // Turn off screen
  bl_set(0);

  // Configure wake on GPIO0 (BOOT button) LOW
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
  // Never reaches here — wake = full reboot
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
  prefs.putInt("vol", currentVol);
}
static void ev_sleep_toggle(lv_event_t*) {
  // Cycle: OFF → 30 → 60 → 90 → OFF
  if (sleepMins == 0) sleepMins = 30;
  else if (sleepMins == 30) sleepMins = 60;
  else if (sleepMins == 60) sleepMins = 90;
  else sleepMins = 0;

  if (sleepMins > 0) {
    sleepTimer = millis() + sleepMins * 60000UL;
    char b[8]; snprintf(b, sizeof b, "%dm", sleepMins);
    lv_label_set_text(wi_sleep_btn, b);
  } else {
    sleepTimer = 0;
    lv_label_set_text(wi_sleep_btn, LV_SYMBOL_BELL);
  }
}
static void ev_led_toggle(lv_event_t*) {
  ledMode = (ledMode + 1) % LED_MODES;
  if (ledMode == 6) neopixelWrite(PIN_RGB_LED, 0, 0, 0);
  lv_label_set_text(wi_led_btn, ledMode == 6 ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
  // Show mode name briefly on title
  if (wi_title) lv_label_set_text(wi_title, ledModeNames[ledMode]);
  prefs.putInt("led", ledMode);
}
static void ev_show_list(lv_event_t*) {
  audio.stopSong();
  isPlaying = false;
  s_pending_stn = -1;       // cancel any pending connect
  s_pending_connect_ms = 0;
  lv_scr_load(scr_list);
  // Trigger background fetch so list shows fresh program info
  if (WiFi.status() == WL_CONNECTED) s_fetch_station = currentStn;
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
  if (wi_logo_img && s.logo) {
    lv_img_set_src(wi_logo_img, s.logo);
    lv_img_set_zoom(wi_logo_img, compute_fit_zoom(s.logo, 56, 300));
    lv_obj_center(wi_logo_img);  // re-center after size change
  }
  if (wi_name) lv_label_set_text(wi_name, s.name);
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
  // Clock
  if (wi_clock) {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char t[6]; snprintf(t, sizeof t, "%02d:%02d", ti.tm_hour, ti.tm_min);
      lv_label_set_text(wi_clock, t);
    }
  }
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
    char b[16]; snprintf(b, sizeof b, "%s %d%%", icon, pct > 100 ? 100 : pct);
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

  // WiFi icon — tappable to open WiFi settings
  lv_obj_t *wifi_btn = lv_btn_create(bar);
  lv_obj_set_size(wifi_btn, 86, 24);
  lv_obj_align(wifi_btn, LV_ALIGN_LEFT_MID, -4, 0);
  lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(wifi_btn, 0, 0);
  lv_obj_set_style_radius(wifi_btn, 4, 0);
  lv_obj_add_event_cb(wifi_btn, [](lv_event_t*) {
    // Stop audio, show WiFi setup, reconnect after
    audio.stopSong();
    isPlaying = false;
    WiFi.disconnect();
    if (scr_wifi) { lv_obj_del(scr_wifi); scr_wifi = nullptr; }
    build_wifi_screen();
    lv_scr_load(scr_wifi);
    wifi_setup_done = false;
    // wifi_setup_done will be set by keyboard callback
    // Main loop will handle reconnection — set flag
    wifiSSID = "";  // force setup flow
  }, LV_EVENT_CLICKED, NULL);
  wi_wifi = lv_label_create(wifi_btn);
  lv_label_set_text(wi_wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wi_wifi, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_wifi, &lv_font_montserrat_12, 0);
  lv_obj_align(wi_wifi, LV_ALIGN_LEFT_MID, 2, 0);

  // Clock next to WiFi icon (inside same tappable button)
  wi_clock = lv_label_create(wifi_btn);
  lv_label_set_text(wi_clock, "--:--");
  lv_obj_set_style_text_color(wi_clock, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_clock, &lv_font_montserrat_12, 0);
  lv_obj_align(wi_clock, LV_ALIGN_RIGHT_MID, -2, 0);

  // Center title
  // Title (tap to open Settings/Info)
  lv_obj_t *hdr_btn = lv_btn_create(bar);
  lv_obj_set_size(hdr_btn, 130, 24);
  lv_obj_align(hdr_btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(hdr_btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(hdr_btn, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(hdr_btn, 0, 0);
  lv_obj_set_style_radius(hdr_btn, 4, 0);
  lv_obj_add_event_cb(hdr_btn, [](lv_event_t*) {
    // Stop audio for smooth settings page (resume via Back)
    audio.stopSong();
    isPlaying = false;
    s_pending_stn = -1;
    s_pending_connect_ms = 0;
    if (scr_settings) { lv_obj_del(scr_settings); scr_settings = nullptr; }
    build_settings_screen();
    refresh_settings_info();
    lv_scr_load(scr_settings);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *hdr_lbl = lv_label_create(hdr_btn);
  lv_label_set_text(hdr_lbl, "Radiko Radio");
  lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(hdr_lbl, &lv_font_montserrat_12, 0);
  lv_obj_center(hdr_lbl);

  // Battery + brightness control (tap to cycle brightness)
  lv_obj_t *bat_btn = lv_btn_create(bar);
  lv_obj_set_size(bat_btn, 100, 24);
  lv_obj_align(bat_btn, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_opa(bat_btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
  lv_obj_set_style_shadow_width(bat_btn, 0, 0);
  lv_obj_add_event_cb(bat_btn, [](lv_event_t*) {
    bl_level = (bl_level + 1) % 4;
    apply_brightness();
    char b[16]; snprintf(b, sizeof b, "%s%s", LV_SYMBOL_SETTINGS, bl_names[bl_level]);
    if (wi_bat) lv_label_set_text(wi_bat, b);
    screenState = 0;
    lastTouch = millis();
  }, LV_EVENT_CLICKED, NULL);
  wi_bat = lv_label_create(bat_btn);
  lv_label_set_text(wi_bat, LV_SYMBOL_BATTERY_FULL);
  lv_obj_set_style_text_color(wi_bat, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_bat, &lv_font_montserrat_12, 0);
  lv_obj_align(wi_bat, LV_ALIGN_RIGHT_MID, -2, 0);

  // ---- Station logo (full width for swipe, tap logo to open list) ----
  wi_logo = lv_obj_create(scr_play);
  lv_obj_set_size(wi_logo, 320, 68);
  lv_obj_set_pos(wi_logo, 0, 28);
  lv_obj_set_style_bg_opa(wi_logo, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wi_logo, 0, 0);
  lv_obj_set_style_pad_all(wi_logo, 0, 0);
  lv_obj_set_style_shadow_width(wi_logo, 0, 0);
  lv_obj_clear_flag(wi_logo, LV_OBJ_FLAG_SCROLLABLE);
  // Manual swipe detection. Logic:
  //   |dx| >= 25  → swipe (lower threshold = easier)
  //   |dx| < 8 AND tap inside central region (x 120..200) → open station list
  //   else: ignore (dead-zone, prevents accidental list opens during near-swipes)
  static lv_coord_t press_x = 0;
  lv_obj_add_event_cb(wi_logo, [](lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(indev, &p);
    press_x = p.x;
  }, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(wi_logo, [](lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(indev, &p);
    int dx = p.x - press_x;
    if (dx >= 25)        { ev_prev(e); refresh_playing(); }       // swipe right → prev
    else if (dx <= -25)  { ev_next(e); refresh_playing(); }       // swipe left → next
    else if (abs(dx) < 8 && p.x >= 120 && p.x <= 200) {
      ev_show_list(e);                                            // small tap on center → list
    }
    // else: dead-zone, do nothing
  }, LV_EVENT_RELEASED, NULL);

  wi_logo_img = lv_img_create(wi_logo);
  lv_img_set_size_mode(wi_logo_img, LV_IMG_SIZE_MODE_REAL);
  lv_img_set_antialias(wi_logo_img, true);  // bilinear scaling — smoother
  lv_img_set_src(wi_logo_img, STATIONS[0].logo);
  lv_img_set_zoom(wi_logo_img, compute_fit_zoom(STATIONS[0].logo, 56, 300));
  lv_obj_center(wi_logo_img);

  // ---- Station name (white) ----
  wi_name = lv_label_create(scr_play);
  lv_label_set_text(wi_name, STATIONS[0].name);
  lv_obj_set_style_text_color(wi_name, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_name, &lv_font_jp_16, 0);
  lv_obj_set_width(wi_name, 300);
  lv_label_set_long_mode(wi_name, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(wi_name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(wi_name, LV_ALIGN_TOP_MID, 0, 91);

  // ---- Program title (grey, scrolling) ----
  wi_title = lv_label_create(scr_play);
  lv_label_set_text(wi_title, "---");
  lv_obj_set_style_text_color(wi_title, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_text_font(wi_title, &lv_font_jp_full, 0);
  lv_obj_set_width(wi_title, 300);
  lv_label_set_long_mode(wi_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_anim_speed(wi_title, 8, 0);  // very slow = tiny jumps during audio blocks
  lv_obj_set_style_text_align(wi_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(wi_title, LV_ALIGN_TOP_MID, 0, 109);

  // ---- Volume row ----
  lv_obj_t *vrow = lv_obj_create(scr_play);
  lv_obj_set_size(vrow, 320, 30);
  lv_obj_set_pos(vrow, 0, 134);
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
  int btn_y = 176;  // vertical center for buttons

  // Prev (small circle)
  lv_obj_t *btn_prev = lv_btn_create(scr_play);
  lv_obj_set_size(btn_prev, 44, 44);
  lv_obj_set_pos(btn_prev, 16, btn_y);
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
  lv_obj_set_pos(btn_play, 90, btn_y - 6);
  lv_obj_set_style_radius(btn_play, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(C_HL), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(0xC03050), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_play, 0, 0);
  lv_obj_add_event_cb(btn_play, ev_play, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(btn_play, [](lv_event_t*) { do_power_off(); }, LV_EVENT_LONG_PRESSED, NULL);
  wi_play = lv_label_create(btn_play);
  lv_label_set_text(wi_play, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(wi_play, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_play, &lv_font_montserrat_16, 0);
  lv_obj_center(wi_play);

  // Next (small circle)
  lv_obj_t *btn_next = lv_btn_create(scr_play);
  lv_obj_set_size(btn_next, 44, 44);
  lv_obj_set_pos(btn_next, 176, btn_y);
  lv_obj_set_style_radius(btn_next, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_next, lv_color_hex(C_ACCENT), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_next, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_next, 0, 0);
  lv_obj_add_event_cb(btn_next, ev_next, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_next = lv_label_create(btn_next);
  lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
  lv_obj_set_style_text_color(lbl_next, lv_color_hex(C_TEXT), 0);
  lv_obj_center(lbl_next);

  // Sleep timer (small circle)
  lv_obj_t *btn_sleep = lv_btn_create(scr_play);
  lv_obj_set_size(btn_sleep, 34, 34);
  lv_obj_set_pos(btn_sleep, 244, btn_y + 5);
  lv_obj_set_style_radius(btn_sleep, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_sleep, lv_color_hex(C_ACCENT), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_sleep, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_sleep, 0, 0);
  lv_obj_add_event_cb(btn_sleep, ev_sleep_toggle, LV_EVENT_CLICKED, NULL);
  wi_sleep_btn = lv_label_create(btn_sleep);
  lv_label_set_text(wi_sleep_btn, LV_SYMBOL_BELL);
  lv_obj_set_style_text_color(wi_sleep_btn, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wi_sleep_btn, &lv_font_montserrat_10, 0);
  lv_obj_center(wi_sleep_btn);

  // LED toggle (small circle, right side)
  lv_obj_t *btn_led = lv_btn_create(scr_play);
  lv_obj_set_size(btn_led, 34, 34);
  lv_obj_set_pos(btn_led, 282, btn_y + 5);
  lv_obj_set_style_radius(btn_led, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_led, lv_color_hex(C_ACCENT), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn_led, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn_led, 0, 0);
  lv_obj_add_event_cb(btn_led, ev_led_toggle, LV_EVENT_CLICKED, NULL);
  wi_led_btn = lv_label_create(btn_led);
  lv_label_set_text(wi_led_btn, ledMode == 6 ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
  lv_obj_set_style_text_color(wi_led_btn, lv_color_hex(C_TEXT), 0);
  lv_obj_center(wi_led_btn);

  // ---- Station position dots ----
  lv_obj_t *drow = lv_obj_create(scr_play);
  int dot_w = NUM_STATIONS * 8 + (NUM_STATIONS - 1) * 4;
  lv_obj_set_size(drow, 320, 12);
  lv_obj_set_pos(drow, 0, 228);
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

    // Station logo: max 38px tall, max 138px wide, preserve ratio
    lv_obj_t *logo = lv_img_create(row);
    lv_img_set_src(logo, STATIONS[i].logo);
    lv_img_set_size_mode(logo, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_antialias(logo, true);  // bilinear scaling — smoother
    lv_img_set_zoom(logo, compute_fit_zoom(STATIONS[i].logo, 38, 138));
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 4, 0);

    // Station full name, top-right
    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, STATIONS[i].name);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_jp_16, 0);
    lv_obj_set_width(name, 168);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(name, LV_ALIGN_TOP_RIGHT, -6, 4);

    // Program title (dim color, bottom-right)
    lv_obj_t *prog = lv_label_create(row);
    lv_label_set_text(prog, stationProgs[i].length() > 0 ? stationProgs[i].c_str() : "---");
    lv_obj_set_style_text_color(prog, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(prog, &lv_font_jp_full, 0);
    lv_obj_set_width(prog, 168);
    lv_label_set_long_mode(prog, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(prog, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(prog, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    list_prog_lbls[i] = prog;
  }
}

// Refresh program-info labels on the station list (called when fetch completes)
static void refresh_list_progs() {
  for (int i = 0; i < NUM_STATIONS; i++) {
    if (list_prog_lbls[i]) {
      lv_label_set_text(list_prog_lbls[i],
        stationProgs[i].length() > 0 ? stationProgs[i].c_str() : "---");
    }
  }
}

// ============================================================
// WIFI SETUP SCREEN — scan SSIDs, select, enter password
// ============================================================
static lv_obj_t *wifi_list = nullptr;
static lv_obj_t *wifi_kb   = nullptr;
static lv_obj_t *wifi_ta   = nullptr;
static String    wifi_selected_ssid = "";

static void ev_wifi_ssid_selected(lv_event_t* e) {
  // SSID stored in user_data, not from label (label has signal info appended)
  const char* ssid = (const char*)lv_event_get_user_data(e);
  wifi_selected_ssid = ssid;
  // If this SSID matches saved credentials, connect directly
  String savedSSID = prefs.getString("ssid", "");
  if (wifi_selected_ssid == savedSSID) {
    wifiSSID = savedSSID;
    wifiPass = prefs.getString("pass", "");
    wifi_setup_done = true;
    return;
  }
  // Otherwise show password input
  lv_textarea_set_text(wifi_ta, "");
  lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ev_wifi_kb_ready(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    wifiSSID = wifi_selected_ssid;
    wifiPass = String(lv_textarea_get_text(wifi_ta));
    wifi_setup_done = true;
  } else if (code == LV_EVENT_CANCEL) {
    // Go back to SSID list
    lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
  }
}

static void build_wifi_screen() {
  scr_wifi = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_wifi, lv_color_hex(C_BG), 0);

  // Header with back button
  lv_obj_t *hdr = lv_obj_create(scr_wifi);
  lv_obj_set_size(hdr, 320, 24);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 2, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(hdr);
  lv_obj_set_size(back, 70, 20);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_add_event_cb(back, [](lv_event_t*) {
    // Return to player without changing WiFi
    wifiSSID = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    if (wifiSSID.length() > 0) {
      wifi_setup_done = true;  // reconnect with existing credentials
    } else {
      lv_scr_load(scr_play);  // just go back, no WiFi
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *blbl = lv_label_create(back);
  lv_label_set_text(blbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(blbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(blbl, &lv_font_montserrat_12, 0);
  lv_obj_center(blbl);

  lv_obj_t* title = lv_label_create(hdr);
  lv_label_set_text(title, "WiFi Setup");
  lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 20, 0);

  // Scrollable SSID list
  wifi_list = lv_obj_create(scr_wifi);
  lv_obj_set_size(wifi_list, 310, 210);
  lv_obj_set_pos(wifi_list, 5, 24);
  lv_obj_set_style_bg_color(wifi_list, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(wifi_list, 0, 0);
  lv_obj_set_style_pad_all(wifi_list, 2, 0);
  lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wifi_list, 2, 0);

  // Scan WiFi — store SSIDs in persistent array for callbacks
  static char ssid_buf[15][33];  // max 15 SSIDs, 32 chars each
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 15; i++) {
    strncpy(ssid_buf[i], WiFi.SSID(i).c_str(), 32);
    ssid_buf[i][32] = 0;

    lv_obj_t* btn = lv_btn_create(wifi_list);
    lv_obj_set_size(btn, 300, 36);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_PANEL), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_HL), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, ev_wifi_ssid_selected, LV_EVENT_CLICKED, ssid_buf[i]);

    lv_obj_t* lbl = lv_label_create(btn);
    char txt[48];
    snprintf(txt, sizeof txt, "%s  (%ddB)", ssid_buf[i], WiFi.RSSI(i));
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);
  }
  WiFi.scanDelete();

  // Password textarea (hidden initially)
  wifi_ta = lv_textarea_create(scr_wifi);
  lv_obj_set_size(wifi_ta, 300, 36);
  lv_obj_align(wifi_ta, LV_ALIGN_TOP_MID, 0, 24);
  lv_textarea_set_placeholder_text(wifi_ta, "Enter password...");
  lv_textarea_set_one_line(wifi_ta, true);
  lv_textarea_set_password_mode(wifi_ta, false);  // show password in plain text
  lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);

  // Keyboard (hidden initially)
  wifi_kb = lv_keyboard_create(scr_wifi);
  lv_keyboard_set_textarea(wifi_kb, wifi_ta);
  lv_obj_add_event_cb(wifi_kb, ev_wifi_kb_ready, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// SETTINGS / INFO SCREEN
// ============================================================
// Dim/Off timeout option tables (index → ms, and map for "set checked by value")
static const uint32_t dim_ms_opts[] = { 60000UL, 180000UL, 300000UL, 600000UL, 900000UL };
static const uint32_t off_ms_opts[] = { 180000UL, 300000UL, 600000UL, 1800000UL, 0UL };  // 0=never
static const int      sleep_min_opts[] = { 0, 30, 60, 90 };

static int dim_opt_index() {
  for (int i = 0; i < 5; i++) if (dim_ms_opts[i] == dim_timeout_ms) return i;
  return 2;  // default 5m
}
static int off_opt_index() {
  for (int i = 0; i < 5; i++) if (off_ms_opts[i] == off_timeout_ms) return i;
  return 2;  // default 10m
}
static int sleep_opt_index() {
  for (int i = 0; i < 4; i++) if (sleep_min_opts[i] == sleepMins) return i;
  return 0;
}

static void refresh_settings_info() {
  if (!scr_settings) return;
  char b[64];

  // WiFi
  if (set_lbl_wifi) {
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(b, sizeof b, "WiFi: %s (%ddBm)", WiFi.SSID().c_str(), (int)WiFi.RSSI());
    } else {
      snprintf(b, sizeof b, "WiFi: disconnected");
    }
    lv_label_set_text(set_lbl_wifi, b);
  }
  if (set_lbl_ip) {
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(b, sizeof b, "IP: %s", WiFi.localIP().toString().c_str());
    } else {
      snprintf(b, sizeof b, "IP: ---");
    }
    lv_label_set_text(set_lbl_ip, b);
  }
  if (set_lbl_area) {
    snprintf(b, sizeof b, "Area: %s", radikoArea.c_str());
    lv_label_set_text(set_lbl_area, b);
  }
  if (set_lbl_bat) {
    int mv = s_bat_mv_avg;
    int pct = bat_pct();
    snprintf(b, sizeof b, "Battery: %dmV (%d%%)", mv, pct);
    lv_label_set_text(set_lbl_bat, b);
  }
  if (set_lbl_uptime) {
    uint32_t s = millis() / 1000;
    uint32_t h = s / 3600;
    uint32_t m = (s % 3600) / 60;
    uint32_t sec = s % 60;
    snprintf(b, sizeof b, "Uptime: %luh %lum %lus", (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    lv_label_set_text(set_lbl_uptime, b);
  }
  if (set_lbl_mem) {
    snprintf(b, sizeof b, "Heap: %uKB  PSRAM: %uKB",
      (unsigned)(ESP.getFreeHeap() / 1024),
      (unsigned)(ESP.getFreePsram() / 1024));
    lv_label_set_text(set_lbl_mem, b);
  }
}

static lv_obj_t *set_bm_rot = nullptr;
static int rot_opt_index() { return scr_rotation == 3 ? 1 : 0; }

// Display labels for each slider's stops
static const char* dim_labels[]   = {"1m", "3m", "5m", "10m", "15m"};
static const char* off_labels[]   = {"3m", "5m", "10m", "30m", "Never"};
static const char* sleep_labels[] = {"Off", "30m", "60m", "90m"};

static void ev_sl_bl_changed(lv_event_t* e) {
  int idx = (int)lv_slider_get_value(set_sl_bl);
  if (idx < 0 || idx > 3) return;
  bl_level = (uint8_t)idx;
  apply_brightness();
  if (set_val_bl) lv_label_set_text(set_val_bl, bl_names[bl_level]);
  if (wi_bat) {
    char b[16]; snprintf(b, sizeof b, "%s%s", LV_SYMBOL_SETTINGS, bl_names[bl_level]);
    lv_label_set_text(wi_bat, b);
  }
  screenState = 0;
  lastTouch = millis();
}

static void ev_sl_dim_changed(lv_event_t* e) {
  int idx = (int)lv_slider_get_value(set_sl_dim);
  if (idx < 0 || idx > 4) return;
  dim_timeout_ms = dim_ms_opts[idx];
  prefs.putUInt("dim_ms", dim_timeout_ms);
  if (set_val_dim) lv_label_set_text(set_val_dim, dim_labels[idx]);
}

static void ev_sl_off_changed(lv_event_t* e) {
  int idx = (int)lv_slider_get_value(set_sl_off);
  if (idx < 0 || idx > 4) return;
  off_timeout_ms = off_ms_opts[idx];
  prefs.putUInt("off_ms", off_timeout_ms);
  if (set_val_off) lv_label_set_text(set_val_off, off_labels[idx]);
}

static void ev_sl_sleep_changed(lv_event_t* e) {
  int idx = (int)lv_slider_get_value(set_sl_sleep);
  if (idx < 0 || idx > 3) return;
  sleepMins = sleep_min_opts[idx];
  if (sleepMins > 0) {
    sleepTimer = millis() + sleepMins * 60000UL;
    char b[8]; snprintf(b, sizeof b, "%dm", sleepMins);
    if (wi_sleep_btn) lv_label_set_text(wi_sleep_btn, b);
  } else {
    sleepTimer = 0;
    if (wi_sleep_btn) lv_label_set_text(wi_sleep_btn, LV_SYMBOL_BELL);
  }
  if (set_val_sleep) lv_label_set_text(set_val_sleep, sleep_labels[idx]);
}

static void ev_bm_rot_changed(lv_event_t* e) {
  int idx = lv_btnmatrix_get_selected_btn(set_bm_rot);
  if (idx < 0 || idx > 1) return;
  uint8_t newRot = (idx == 1) ? 3 : 1;
  if (newRot == scr_rotation) return;
  scr_rotation = newRot;
  prefs.putUChar("rot", scr_rotation);
  // Apply live: rotate display + touch, force full redraw
  tft.setRotation(scr_rotation);
  touch_set_rotation(scr_rotation);
  lv_obj_invalidate(lv_scr_act());
}

static void build_settings_screen() {
  scr_settings = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_settings, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr_settings, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

  // Header
  lv_obj_t *hdr = lv_obj_create(scr_settings);
  lv_obj_set_size(hdr, 320, 28);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 2, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(hdr);
  lv_obj_set_size(back, 72, 24);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(C_HL), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_add_event_cb(back, [](lv_event_t*) {
    lv_scr_load(scr_play);
    // Resume playback
    if (!isPlaying) {
      play_stn(currentStn);
      s_pending_connect_ms = millis() - 2000;  // connect immediately
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *blbl = lv_label_create(back);
  lv_label_set_text(blbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(blbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(blbl, &lv_font_montserrat_12, 0);
  lv_obj_center(blbl);

  lv_obj_t *title = lv_label_create(hdr);
  lv_label_set_text(title, "Settings / Info");
  lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 24, 0);

  // Scrollable content area
  lv_obj_t *cont = lv_obj_create(scr_settings);
  lv_obj_set_size(cont, 320, 212);
  lv_obj_set_pos(cont, 0, 28);
  lv_obj_set_style_bg_color(cont, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 6, 0);
  lv_obj_set_style_pad_row(cont, 6, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  // Section helper (creates a header label)
  auto make_section = [&](const char* txt) {
    lv_obj_t *l = lv_label_create(cont);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(C_HL), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  };
  auto make_info = [&]() -> lv_obj_t* {
    lv_obj_t *l = lv_label_create(cont);
    lv_label_set_text(l, "...");
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    return l;
  };
  // Slider row helper: title (left) + value (right) over a stepped slider.
  // Returns slider, sets *out_val to the value label.
  auto make_slider_row = [&](const char* title, int max_idx, int cur_idx,
                              const char* cur_label, lv_event_cb_t cb,
                              lv_obj_t** out_val) -> lv_obj_t* {
    lv_obj_t* row = lv_obj_create(cont);
    lv_obj_set_size(row, 300, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_top(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tl = lv_label_create(row);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, lv_color_hex(C_HL), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 4, 0);

    lv_obj_t* vl = lv_label_create(row);
    lv_label_set_text(vl, cur_label);
    lv_obj_set_style_text_color(vl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(vl, &lv_font_montserrat_12, 0);
    lv_obj_align(vl, LV_ALIGN_TOP_RIGHT, -4, 0);
    *out_val = vl;

    lv_obj_t* sl = lv_slider_create(row);
    lv_obj_set_size(sl, 286, 8);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_slider_set_range(sl, 0, max_idx);
    lv_slider_set_value(sl, cur_idx, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_HL),    LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_HL),    LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 5,                       LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sl;
  };

  // --- Brightness ---
  set_sl_bl = make_slider_row("Brightness", 3, bl_level, bl_names[bl_level],
                               ev_sl_bl_changed, &set_val_bl);

  // --- Screen Dim ---
  {
    int idx = dim_opt_index();
    set_sl_dim = make_slider_row("Screen Dim", 4, idx, dim_labels[idx],
                                  ev_sl_dim_changed, &set_val_dim);
  }

  // --- Screen Off ---
  {
    int idx = off_opt_index();
    set_sl_off = make_slider_row("Screen Off", 4, idx, off_labels[idx],
                                  ev_sl_off_changed, &set_val_off);
  }

  // --- Sleep Timer ---
  {
    int idx = sleep_opt_index();
    set_sl_sleep = make_slider_row("Sleep Timer", 3, idx, sleep_labels[idx],
                                    ev_sl_sleep_changed, &set_val_sleep);
  }

  make_section("Screen Rotation");
  static const char* rot_map[] = {"Normal", "Flip 180", ""};
  set_bm_rot = lv_btnmatrix_create(cont);
  lv_btnmatrix_set_map(set_bm_rot, rot_map);
  lv_obj_set_size(set_bm_rot, 300, 28);
  lv_btnmatrix_set_btn_ctrl_all(set_bm_rot, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_btnmatrix_set_one_checked(set_bm_rot, true);
  lv_btnmatrix_set_btn_ctrl(set_bm_rot, rot_opt_index(), LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_set_style_bg_color(set_bm_rot, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(set_bm_rot, 0, 0);
  lv_obj_set_style_pad_all(set_bm_rot, 2, 0);
  lv_obj_set_style_bg_color(set_bm_rot, lv_color_hex(C_ACCENT), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(set_bm_rot, lv_color_hex(C_HL),    LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_font(set_bm_rot, &lv_font_montserrat_12, LV_PART_ITEMS);
  lv_obj_set_style_text_color(set_bm_rot, lv_color_hex(C_TEXT), LV_PART_ITEMS);
  lv_obj_add_event_cb(set_bm_rot, ev_bm_rot_changed, LV_EVENT_VALUE_CHANGED, NULL);

  // --- System info ---
  make_section("System Info");
  set_lbl_wifi   = make_info();
  set_lbl_ip     = make_info();
  set_lbl_area   = make_info();
  set_lbl_bat    = make_info();
  set_lbl_uptime = make_info();
  set_lbl_mem    = make_info();

  // --- Firmware info ---
  make_section("Firmware");
  lv_obj_t *fw1 = make_info();
  lv_label_set_text(fw1, FW_VERSION);
  lv_obj_t *fw2 = make_info();
  char fwb[64];
  snprintf(fwb, sizeof fwb, "Built: %s %s", __DATE__, __TIME__);
  lv_label_set_text(fw2, fwb);
  lv_obj_t *fw3 = make_info();
  snprintf(fwb, sizeof fwb, "Chip: %s r%d",
           ESP.getChipModel(), ESP.getChipRevision());
  lv_label_set_text(fw3, fwb);
  lv_obj_t *fw4 = make_info();
  snprintf(fwb, sizeof fwb, "IDF: %s", ESP.getSdkVersion());
  lv_label_set_text(fw4, fwb);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // Load saved settings
  prefs.begin("radiko", false);
  currentStn = prefs.getInt("stn", 0);
  currentVol = prefs.getInt("vol", 20);
  ledMode = prefs.getInt("led", 0);
  dim_timeout_ms = prefs.getUInt("dim_ms", 300000UL);
  off_timeout_ms = prefs.getUInt("off_ms", 600000UL);
  scr_rotation   = prefs.getUChar("rot", 1);
  if (scr_rotation != 1 && scr_rotation != 3) scr_rotation = 1;
  bl_level       = prefs.getUChar("bl", 3);
  if (bl_level > 3) bl_level = 3;
  if (currentStn >= NUM_STATIONS) currentStn = 0;

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
  tft.setRotation(scr_rotation);
  tft.fillScreen(TFT_BLACK);

  // Backlight PWM – attach AFTER tft.init() which calls pinMode(TFT_BL)
  ledcAttach(PIN_BL, 5000, 8);
  bl_set(bl_duty_levels[bl_level]);

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

  // WiFi connection with setup screen fallback
  WiFi.mode(WIFI_STA);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");

  while (WiFi.status() != WL_CONNECTED) {
    if (wifiSSID.length() > 0) {
      // Try connecting
      String msg = "Connecting: " + wifiSSID;
      show_status(msg.c_str());
      lv_task_handler();
      WiFi.disconnect();
      delay(200);
      WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
        lv_task_handler(); delay(100);
      }
      if (WiFi.status() == WL_CONNECTED) {
        // Save successful credentials
        prefs.putString("ssid", wifiSSID);
        prefs.putString("pass", wifiPass);
        break;
      }
      // Failed
      WiFi.disconnect();
      show_status(("Failed: " + wifiSSID).c_str());
      lv_task_handler();
      delay(1500);
    }

    // No credentials or connection failed — show WiFi setup screen
    hide_status();
    if (scr_wifi) { lv_obj_del(scr_wifi); scr_wifi = nullptr; }  // clean up old screen
    build_wifi_screen();
    lv_scr_load(scr_wifi);
    wifi_setup_done = false;
    while (!wifi_setup_done) { lv_task_handler(); delay(10); }
    lv_scr_load(scr_play);
  }
  hide_status();

  // NTP time sync (JST = UTC+9)
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");

  // Radiko auth
  show_status("Authenticating Radiko...");
  lv_task_handler();
  if (!radiko_auth()) { show_status("Auth failed!"); return; }


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

  // RGB LED task on Core 0 — independent of audio + LVGL on Core 1
  xTaskCreatePinnedToCore([](void*) {
    for (;;) {
      rgb_update();
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }, "rgb", 2048, NULL, 1, NULL, 0);
}

// ============================================================
// LOOP (single-core: audio + LVGL on same core)
// ============================================================
void loop() {
  lv_task_handler();
  audio.loop();

  // WiFi setup triggered from player (wifi icon tap)
  if (scr_wifi && lv_scr_act() == scr_wifi && wifi_setup_done) {
    wifi_setup_done = false;
    lv_scr_load(scr_play);
    show_status(("Connecting: " + wifiSSID).c_str());
    lv_task_handler();
    WiFi.disconnect();
    delay(200);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
      lv_task_handler(); delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      prefs.putString("ssid", wifiSSID);
      prefs.putString("pass", wifiPass);
      hide_status();
      // Re-auth Radiko with new connection
      show_status("Authenticating Radiko...");
      lv_task_handler();
      radiko_auth();
      hide_status();
      play_stn(currentStn);
      s_pending_connect_ms = millis() - 2000;  // connect immediately
    } else {
      show_status("Failed!");
      lv_task_handler(); delay(1500);
      hide_status();
      // Go back to WiFi setup
      if (scr_wifi) { lv_obj_del(scr_wifi); scr_wifi = nullptr; }
      build_wifi_screen();
      lv_scr_load(scr_wifi);
    }
  }

  // Debounced station connect: 1s after last button press
  if (s_pending_stn >= 0 && s_pending_connect_ms > 0 &&
      millis() - s_pending_connect_ms >= 1500) {
    do_connect(s_pending_stn);
    refresh_playing();
  }

  // Background program info fetch (runs in separate task on Core 0)
  static uint32_t last_fetch = 0;
  static bool fetch_running = false;
  // Trigger: new station connect OR every 3 minutes
  if (s_fetch_station >= 0 || (isPlaying && !fetch_running && millis() - last_fetch > 180000)) {
    int idx = (s_fetch_station >= 0) ? s_fetch_station : currentStn;
    s_fetch_station = -1;
    last_fetch = millis();
    fetch_running = true;
    xTaskCreatePinnedToCore([](void* p) {
      int i = (int)(intptr_t)p;
      fetch_program_info(STATIONS[i].id);  // now parses ALL stations
      fetch_running = false;
      vTaskDelete(NULL);
    }, "fetch", 8192, (void*)(intptr_t)idx, 1, NULL, 0);
  }

  // Refresh station list labels when fetch task signals new data
  if (s_progs_updated) {
    s_progs_updated = false;
    refresh_list_progs();
  }

  // Auto-reconnect WiFi if disconnected during playback
  static uint32_t last_wifi_check = 0;
  if (millis() - last_wifi_check > 5000) {
    last_wifi_check = millis();
    if (WiFi.status() != WL_CONNECTED && wifiSSID.length() > 0) {
      WiFi.reconnect();
    }
  }

  // Sleep timer — stop radio + screen off
  if (sleepTimer > 0 && millis() > sleepTimer) {
    sleepTimer = 0; sleepMins = 0;
    audio.stopSong(); isPlaying = false;
    bl_set(0); screenState = 2;
    lv_label_set_text(wi_sleep_btn, LV_SYMBOL_BELL);
    lv_label_set_text(wi_play, LV_SYMBOL_PLAY);
  }

  // Screen timeout: 5min→dim, 10min→off
  uint32_t idle = millis() - lastTouch;
  if (screenState == 0 && idle > dim_timeout_ms) {
    screenState = 1;
    bl_set(DIM_DUTY);
  } else if (screenState == 1 && idle > off_timeout_ms) {
    screenState = 2;
    bl_set(0);
  }

  // RGB LED runs in its own task on Core 0 (started in setup)

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
