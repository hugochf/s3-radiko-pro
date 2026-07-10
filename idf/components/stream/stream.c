#include "stream.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "httpc.h"
#include "radiko.h"

static const char *TAG = "stream";

// Modern flow: fetch the stream-info XML to discover the playlist_create_url.
#define STREAM_INFO_FMT "https://radiko.jp/v3/station/stream/pc_html5/%s.xml"

void stream_probe(const char *station_id)
{
    char url[160];
    snprintf(url, sizeof(url), STREAM_INFO_FMT, station_id);

    static char body[4096];
    http_req_t r = {
        .url      = url,
        .method   = HTTP_METHOD_GET,
        .body     = body,
        .body_cap = sizeof(body),
    };

    if (httpc_do(&r) != ESP_OK) {
        ESP_LOGE(TAG, "stream-info fetch error");
        return;
    }
    ESP_LOGI(TAG, "stream-info %s -> status=%d len=%u", station_id, r.status, (unsigned)r.body_len);
    ESP_LOGI(TAG, "body:\n%.900s", body);
}
