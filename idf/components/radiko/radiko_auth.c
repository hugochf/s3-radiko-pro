#include "radiko.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "httpc.h"
#include "mbedtls/base64.h"

static const char *TAG = "radiko";

// Well-known pc_html5 app key (public; same as the Arduino build).
static const char AUTH_KEY[] = "bcd151073c03b352e1ef2fd66c32209da9ca0afa";

// Cached result of the last successful authentication.
static radiko_auth_t s_auth;

const char *radiko_token(void) { return s_auth.token; }
const char *radiko_area(void)  { return s_auth.area; }

esp_err_t radiko_authenticate(radiko_auth_t *out)
{
    // ---- auth1: get token + key offset/length from response headers ----
    static char resp[3][128];
    const char *want[] = { "X-Radiko-AuthToken", "X-Radiko-KeyLength", "X-Radiko-KeyOffset" };
    const http_header_t hdrs1[] = {
        {"X-Radiko-App",         "pc_html5"},
        {"X-Radiko-App-Version", "0.0.1"},
        {"X-Radiko-User",        "dummy_user"},
        {"X-Radiko-Device",      "pc"},
    };
    http_req_t r1 = {
        .url           = "https://radiko.jp/v2/api/auth1",
        .method        = HTTP_METHOD_GET,
        .req_headers   = hdrs1,
        .n_req_headers = 4,
        .want_headers  = want,
        .resp_values   = resp,
        .n_want        = 3,
    };
    if (httpc_do(&r1) != ESP_OK || r1.status != 200) {
        ESP_LOGE(TAG, "auth1 failed (status %d)", r1.status);
        return ESP_FAIL;
    }

    const char *token = resp[0];
    int klen = atoi(resp[1]);
    int koff = atoi(resp[2]);
    if (strlen(token) == 0 || klen <= 0 || koff < 0 ||
        koff + klen > (int)strlen(AUTH_KEY)) {
        ESP_LOGE(TAG, "auth1 bad params: token=%d klen=%d koff=%d",
                 (int)strlen(token), klen, koff);
        return ESP_FAIL;
    }

    // ---- partial key = base64(AUTH_KEY[koff .. koff+klen]) ----
    unsigned char pkey[128];
    size_t pkey_len = 0;
    if (mbedtls_base64_encode(pkey, sizeof(pkey), &pkey_len,
                              (const unsigned char *)AUTH_KEY + koff, klen) != 0) {
        ESP_LOGE(TAG, "base64 failed");
        return ESP_FAIL;
    }
    pkey[pkey_len] = '\0';

    // ---- auth2: confirm token, learn the area ----
    static char body[128];
    const http_header_t hdrs2[] = {
        {"X-Radiko-AuthToken",  token},
        {"X-Radiko-PartialKey", (char *)pkey},
        {"X-Radiko-User",       "dummy_user"},
        {"X-Radiko-Device",     "pc"},
    };
    http_req_t r2 = {
        .url           = "https://radiko.jp/v2/api/auth2",
        .method        = HTTP_METHOD_GET,
        .req_headers   = hdrs2,
        .n_req_headers = 4,
        .body          = body,
        .body_cap      = sizeof(body),
    };
    if (httpc_do(&r2) != ESP_OK || r2.status != 200) {
        ESP_LOGE(TAG, "auth2 failed (status %d)", r2.status);
        return ESP_FAIL;
    }

    // Body looks like "JP14,神奈川県,kanagawa Japan" — area is up to the first comma.
    strlcpy(out->token, token, sizeof(out->token));
    const char *comma = strchr(body, ',');
    size_t alen = comma ? (size_t)(comma - body) : strlen(body);
    if (alen >= sizeof(out->area)) alen = sizeof(out->area) - 1;
    memcpy(out->area, body, alen);
    out->area[alen] = '\0';

    s_auth = *out;   // cache for radiko_token()/radiko_area()
    ESP_LOGI(TAG, "authenticated: area=%s token=%.12s...", out->area, out->token);
    return ESP_OK;
}
