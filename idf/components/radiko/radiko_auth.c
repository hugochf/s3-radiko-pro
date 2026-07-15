#include "radiko.h"

#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "mbedtls/base64.h"

static const char *TAG = "radiko";

// ---- Android-app geo-auth (Phase 30) ---------------------------------------
// Radiko's PC/HTML5 auth derives the area purely from the source IP (needs a
// Japan VPN). Its ANDROID app auth additionally accepts GPS coordinates
// (X-Radiko-Location) and trusts them over the IP — so sending a Japanese
// prefecture's coordinates streams from any IP, no VPN. This is exactly what
// the rajiko extension does (studied from jackyzy823/rajiko v3.2026.2).
//
// MAINTENANCE: if auth starts returning 401/"OUT", Radiko rotated the app.
// Refresh three things from the current rajiko source (modules/static.js):
//   - APP_ID / APP_VERSION below,
//   - the aSmartPhone8 full key (components/radiko/aSmartPhone8.key, reflashed
//     to the radikokey partition — see docs),
//   - the coordinate table if the format ever changes.
#define APP_ID       "aSmartPhone8"
#define APP_VERSION  "8.2.4"
#define ANDROID_SDK  "34"                 // Android 14; only used in the device id
#define KEY_SIZE     125779               // bytes of aSmartPhone8.key (the JPEG blob)

// A few plausible device models; the exact value doesn't gate auth, but varying
// it (like rajiko) avoids every unit looking identical.
static const char *MODELS[] = { "Pixel 7", "Pixel 8", "SO-53D", "SC-53D", "A301SH" };

// Prefecture centre coordinates, indexed by JP area number (JP1..JP47). The
// area we *claim* to be in = which row we send. From rajiko modules/static.js.
static const float AREA_COORD[47][2] = {
    { 43.064615f, 141.346807f },  // JP1  Hokkaido
    { 40.824308f, 140.739998f },  // JP2  Aomori
    { 39.703619f, 141.152684f },  // JP3  Iwate
    { 38.268837f, 140.8721f },    // JP4  Miyagi
    { 39.718614f, 140.102364f },  // JP5  Akita
    { 38.240436f, 140.363633f },  // JP6  Yamagata
    { 37.750299f, 140.467551f },  // JP7  Fukushima
    { 36.341811f, 140.446793f },  // JP8  Ibaraki
    { 36.565725f, 139.883565f },  // JP9  Tochigi
    { 36.390668f, 139.060406f },  // JP10 Gunma
    { 35.856999f, 139.648849f },  // JP11 Saitama
    { 35.605057f, 140.123306f },  // JP12 Chiba
    { 35.689488f, 139.691706f },  // JP13 Tokyo
    { 35.447507f, 139.642345f },  // JP14 Kanagawa
    { 37.902552f, 139.023095f },  // JP15 Niigata
    { 36.695291f, 137.211338f },  // JP16 Toyama
    { 36.594682f, 136.625573f },  // JP17 Ishikawa
    { 36.065178f, 136.221527f },  // JP18 Fukui
    { 35.664158f, 138.568449f },  // JP19 Yamanashi
    { 36.651299f, 138.180956f },  // JP20 Nagano
    { 35.391227f, 136.722291f },  // JP21 Gifu
    { 34.97712f,  138.383084f },  // JP22 Shizuoka
    { 35.180188f, 136.906565f },  // JP23 Aichi
    { 34.730283f, 136.508588f },  // JP24 Mie
    { 35.004531f, 135.86859f },   // JP25 Shiga
    { 35.021247f, 135.755597f },  // JP26 Kyoto
    { 34.686297f, 135.519661f },  // JP27 Osaka
    { 34.691269f, 135.183071f },  // JP28 Hyogo
    { 34.685334f, 135.832742f },  // JP29 Nara
    { 34.225987f, 135.167509f },  // JP30 Wakayama
    { 35.503891f, 134.237736f },  // JP31 Tottori
    { 35.472295f, 133.0505f },    // JP32 Shimane
    { 34.661751f, 133.934406f },  // JP33 Okayama
    { 34.39656f,  132.459622f },  // JP34 Hiroshima
    { 34.185956f, 131.470649f },  // JP35 Yamaguchi
    { 34.065718f, 134.55936f },   // JP36 Tokushima
    { 34.340149f, 134.043444f },  // JP37 Kagawa
    { 33.841624f, 132.765681f },  // JP38 Ehime
    { 33.559706f, 133.531079f },  // JP39 Kochi
    { 33.606576f, 130.418297f },  // JP40 Fukuoka
    { 33.249442f, 130.299794f },  // JP41 Saga
    { 32.744839f, 129.873756f },  // JP42 Nagasaki
    { 32.789827f, 130.741667f },  // JP43 Kumamoto
    { 33.238172f, 131.612619f },  // JP44 Oita
    { 31.911096f, 131.423893f },  // JP45 Miyazaki
    { 31.560146f, 130.557978f },  // JP46 Kagoshima
    { 26.2124f,   127.680932f },  // JP47 Okinawa
};

// Prefecture display names, indexed JP1..JP47 (romanised, short).
static const char *AREA_NAME[47] = {
    "Hokkaido","Aomori","Iwate","Miyagi","Akita","Yamagata","Fukushima",
    "Ibaraki","Tochigi","Gunma","Saitama","Chiba","Tokyo","Kanagawa",
    "Niigata","Toyama","Ishikawa","Fukui","Yamanashi","Nagano","Gifu",
    "Shizuoka","Aichi","Mie","Shiga","Kyoto","Osaka","Hyogo","Nara",
    "Wakayama","Tottori","Shimane","Okayama","Hiroshima","Yamaguchi",
    "Tokushima","Kagawa","Ehime","Kochi","Fukuoka","Saga","Nagasaki",
    "Kumamoto","Oita","Miyazaki","Kagoshima","Okinawa",
};

const char *radiko_area_name(int jp_area)
{
    if (jp_area < 1 || jp_area > 47) jp_area = 13;
    return AREA_NAME[jp_area - 1];
}

// Japanese prefecture names (short form), indexed JP1..JP47. UTF-8.
static const char *AREA_NAME_JP[47] = {
    "北海道","青森","岩手","宮城","秋田","山形","福島","茨城","栃木","群馬",
    "埼玉","千葉","東京","神奈川","新潟","富山","石川","福井","山梨","長野",
    "岐阜","静岡","愛知","三重","滋賀","京都","大阪","兵庫","奈良","和歌山",
    "鳥取","島根","岡山","広島","山口","徳島","香川","愛媛","高知","福岡",
    "佐賀","長崎","熊本","大分","宮崎","鹿児島","沖縄",
};

const char *radiko_area_name_jp(int jp_area)
{
    if (jp_area < 1 || jp_area > 47) jp_area = 13;
    return AREA_NAME_JP[jp_area - 1];
}

// Which prefecture to authenticate as (JP number 1..47). Default Tokyo (13);
// set from persisted settings at boot and by the Settings area picker.
static int s_area = 13;

void radiko_set_area(int jp_area)
{
    if (jp_area >= 1 && jp_area <= 47) s_area = jp_area;
}
int radiko_area_num(void) { return s_area; }

// Cached result of the last successful authentication.
static radiko_auth_t s_auth;

const char *radiko_token(void) { return s_auth.token; }
const char *radiko_area(void)  { return s_auth.area; }

// X-Radiko-Location for the current area: prefecture centre + up to ~2.7 km of
// random jitter, formatted "lat,lng,gps" (matches rajiko genGPS). Coordinates
// are formatted with integer math, not %f, so it survives nano-printf too.
static void gen_gps(char *out, size_t cap)
{
    int idx = (s_area >= 1 && s_area <= 47) ? s_area - 1 : 12;
    double lat = AREA_COORD[idx][0];
    double lng = AREA_COORD[idx][1];
    lat += (esp_random() / (double)UINT32_MAX) / 40.0 * ((esp_random() & 1) ? 1 : -1);
    lng += (esp_random() / (double)UINT32_MAX) / 40.0 * ((esp_random() & 1) ? 1 : -1);
    long lu = (long)(lat * 1000000 + 0.5);
    long gu = (long)(lng * 1000000 + 0.5);
    snprintf(out, cap, "%ld.%06ld,%ld.%06ld,gps",
             lu / 1000000, lu % 1000000, gu / 1000000, gu % 1000000);
}

// Read key[offset..offset+len) from the radikokey flash partition. The full
// ~123 KB app key is too big for the near-full app slot, so it lives in its
// own data partition (flashed once via parttool — see docs).
static bool read_key_slice(int offset, int len, unsigned char *out)
{
    if (offset < 0 || len <= 0 || offset + len > KEY_SIZE) return false;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "radikokey");
    if (!p) { ESP_LOGE(TAG, "radikokey partition missing (flash the key)"); return false; }
    return esp_partition_read(p, offset, out, len) == ESP_OK;
}

// Response capture for the shared client: the three auth1 headers + auth2 body.
static struct {
    char   hdr[3][128];
    char   body[128];
    size_t blen;
} s_rx;
static const char *WANT_HDRS[3] =
    { "X-Radiko-AuthToken", "X-Radiko-KeyLength", "X-Radiko-KeyOffset" };

static esp_err_t on_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        for (int i = 0; i < 3; i++) {
            if (strcasecmp(e->header_key, WANT_HDRS[i]) == 0) {
                strlcpy(s_rx.hdr[i], e->header_value, sizeof(s_rx.hdr[i]));
            }
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        size_t space = sizeof(s_rx.body) - 1 - s_rx.blen;
        size_t n = ((size_t)e->data_len < space) ? (size_t)e->data_len : space;
        memcpy(s_rx.body + s_rx.blen, e->data, n);
        s_rx.blen += n;
        s_rx.body[s_rx.blen] = '\0';
    }
    return ESP_OK;
}

esp_err_t radiko_authenticate(radiko_auth_t *out)
{
    // Per-auth device identity: random model + 32-hex user (like rajiko).
    char device[48], user[33];
    snprintf(device, sizeof(device), "%s.%s",
             ANDROID_SDK, MODELS[esp_random() % (sizeof(MODELS) / sizeof(MODELS[0]))]);
    for (int i = 0; i < 32; i++) user[i] = "0123456789abcdef"[esp_random() & 15];
    user[32] = '\0';

    // Both auth calls hit the same host, so they share ONE keep-alive client:
    // the second request rides the first request's TLS session (mbedtls in PSRAM).
    memset(&s_rx, 0, sizeof(s_rx));
    esp_http_client_config_t cfg = {
        .url               = "https://radiko.jp/v2/api/auth1",
        .event_handler     = on_evt,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    // ---- auth1: Android app headers -> token + key offset/length ----
    esp_http_client_set_header(c, "X-Radiko-App",         APP_ID);
    esp_http_client_set_header(c, "X-Radiko-App-Version", APP_VERSION);
    esp_http_client_set_header(c, "X-Radiko-User",        user);
    esp_http_client_set_header(c, "X-Radiko-Device",      device);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "auth1 failed (%s, status %d)", esp_err_to_name(err), status);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    const char *token = s_rx.hdr[0];
    int klen = atoi(s_rx.hdr[1]);
    int koff = atoi(s_rx.hdr[2]);
    if (strlen(token) == 0 || klen <= 0 || koff < 0 || koff + klen > KEY_SIZE) {
        ESP_LOGE(TAG, "auth1 bad params: token=%d klen=%d koff=%d",
                 (int)strlen(token), klen, koff);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    // ---- partial key = base64(appkey[koff .. koff+klen]) from the partition ----
    unsigned char slice[128];
    if (klen > (int)sizeof(slice) || !read_key_slice(koff, klen, slice)) {
        ESP_LOGE(TAG, "key slice read failed (off=%d len=%d)", koff, klen);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    unsigned char pkey[192];
    size_t pkey_len = 0;
    if (mbedtls_base64_encode(pkey, sizeof(pkey), &pkey_len, slice, klen) != 0) {
        ESP_LOGE(TAG, "base64 failed");
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    pkey[pkey_len] = '\0';

    // ---- auth2 on the same connection: token + partial key + spoofed GPS ----
    char gps[48];
    gen_gps(gps, sizeof(gps));
    s_rx.blen = 0;
    s_rx.body[0] = '\0';
    esp_http_client_set_url(c, "https://radiko.jp/v2/api/auth2");
    esp_http_client_set_header(c, "X-Radiko-AuthToken",  token);
    esp_http_client_set_header(c, "X-Radiko-Partialkey", (const char *)pkey);
    esp_http_client_set_header(c, "X-Radiko-User",       user);
    esp_http_client_set_header(c, "X-Radiko-Device",     device);
    esp_http_client_set_header(c, "X-Radiko-Location",   gps);
    esp_http_client_set_header(c, "X-Radiko-Connection", "wifi");
    err = esp_http_client_perform(c);
    status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "auth2 failed (%s, status %d, body=%.32s)",
                 esp_err_to_name(err), status, s_rx.body);
        return ESP_FAIL;
    }

    // Body looks like "JP13,東京都,tokyo Japan" — area is up to the first comma.
    strlcpy(out->token, token, sizeof(out->token));
    const char *comma = strchr(s_rx.body, ',');
    size_t alen = comma ? (size_t)(comma - s_rx.body) : strlen(s_rx.body);
    if (alen >= sizeof(out->area)) alen = sizeof(out->area) - 1;
    memcpy(out->area, s_rx.body, alen);
    out->area[alen] = '\0';

    s_auth = *out;   // cache for radiko_token()/radiko_area()
    ESP_LOGI(TAG, "authenticated: area=%s (gps=%s) token=%.12s...",
             out->area, gps, out->token);
    return ESP_OK;
}
