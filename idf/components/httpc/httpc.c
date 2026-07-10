#include "httpc.h"

#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "httpc";

static esp_err_t on_event(esp_http_client_event_t *e)
{
    http_req_t *r = (http_req_t *)e->user_data;
    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        for (int i = 0; i < r->n_want; i++) {
            if (strcasecmp(e->header_key, r->want_headers[i]) == 0) {
                strncpy(r->resp_values[i], e->header_value, 127);
                r->resp_values[i][127] = '\0';
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (r->body && r->body_cap > 0) {
            size_t space = r->body_cap - 1 - r->body_len;
            size_t n = ((size_t)e->data_len < space) ? (size_t)e->data_len : space;
            memcpy(r->body + r->body_len, e->data, n);
            r->body_len += n;
            r->body[r->body_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t httpc_do(http_req_t *r)
{
    r->body_len = 0;
    if (r->body && r->body_cap) r->body[0] = '\0';
    for (int i = 0; i < r->n_want; i++) r->resp_values[i][0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = r->url,
        .method            = r->method,
        .event_handler     = on_event,
        .user_data         = r,
        .crt_bundle_attach = esp_crt_bundle_attach,   // validate against the CA bundle
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    for (int i = 0; i < r->n_req_headers; i++) {
        esp_http_client_set_header(c, r->req_headers[i].name, r->req_headers[i].value);
    }

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        r->status = esp_http_client_get_status_code(c);
    } else {
        ESP_LOGW(TAG, "%s -> %s", r->url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    return err;
}
