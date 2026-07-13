#include "radiko.h"

#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "radiko";

// Well-known pc_html5 app key (public; same as the Arduino build).
static const char AUTH_KEY[] = "bcd151073c03b352e1ef2fd66c32209da9ca0afa";

// Cached result of the last successful authentication.
static radiko_auth_t s_auth;

const char *radiko_token(void) { return s_auth.token; }
const char *radiko_area(void)  { return s_auth.area; }

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
    // Both auth calls hit the same host, so they share ONE keep-alive client:
    // the second request rides the first request's TLS session instead of
    // paying another full handshake (~6 s on this target — mbedtls in PSRAM).
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

    // ---- auth1: get token + key offset/length from response headers ----
    esp_http_client_set_header(c, "X-Radiko-App",         "pc_html5");
    esp_http_client_set_header(c, "X-Radiko-App-Version", "0.0.1");
    esp_http_client_set_header(c, "X-Radiko-User",        "dummy_user");
    esp_http_client_set_header(c, "X-Radiko-Device",      "pc");
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
    if (strlen(token) == 0 || klen <= 0 || koff < 0 ||
        koff + klen > (int)strlen(AUTH_KEY)) {
        ESP_LOGE(TAG, "auth1 bad params: token=%d klen=%d koff=%d",
                 (int)strlen(token), klen, koff);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    // ---- partial key = base64(AUTH_KEY[koff .. koff+klen]) ----
    unsigned char pkey[128];
    size_t pkey_len = 0;
    if (mbedtls_base64_encode(pkey, sizeof(pkey), &pkey_len,
                              (const unsigned char *)AUTH_KEY + koff, klen) != 0) {
        ESP_LOGE(TAG, "base64 failed");
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    pkey[pkey_len] = '\0';

    // ---- auth2 on the same connection: confirm token, learn the area ----
    s_rx.blen = 0;
    s_rx.body[0] = '\0';
    esp_http_client_set_url(c, "https://radiko.jp/v2/api/auth2");
    esp_http_client_set_header(c, "X-Radiko-AuthToken",  token);
    esp_http_client_set_header(c, "X-Radiko-PartialKey", (const char *)pkey);
    err = esp_http_client_perform(c);
    status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "auth2 failed (%s, status %d)", esp_err_to_name(err), status);
        return ESP_FAIL;
    }

    // Body looks like "JP14,神奈川県,kanagawa Japan" — area is up to the first comma.
    strlcpy(out->token, token, sizeof(out->token));
    const char *comma = strchr(s_rx.body, ',');
    size_t alen = comma ? (size_t)(comma - s_rx.body) : strlen(s_rx.body);
    if (alen >= sizeof(out->area)) alen = sizeof(out->area) - 1;
    memcpy(out->area, s_rx.body, alen);
    out->area[alen] = '\0';

    s_auth = *out;   // cache for radiko_token()/radiko_area()
    ESP_LOGI(TAG, "authenticated: area=%s token=%.12s...", out->area, out->token);
    return ESP_OK;
}
