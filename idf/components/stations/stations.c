#include "stations.h"

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "stations";

// Base of the mmap'd `logos` partition; logo pixels are read from here directly.
static const uint8_t *s_map;

// Active (area-filtered) list: indices into STATIONS_ALL, plus prebuilt LVGL
// image descriptors pointing into the mmap. Rebuilt on stations_set_area.
static int           s_active[MAX_STATIONS];
static int           s_count;
static lv_image_dsc_t s_big[MAX_STATIONS];
static lv_image_dsc_t s_small[MAX_STATIONS];

esp_err_t stations_init(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "logos");
    if (!p) {
        ESP_LOGE(TAG, "logos partition missing (flash logos.bin)");
        return ESP_ERR_NOT_FOUND;
    }
    esp_partition_mmap_handle_t h;
    esp_err_t err = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA,
                                       (const void **)&s_map, &h);
    if (err != ESP_OK) ESP_LOGE(TAG, "logos mmap failed (%s)", esp_err_to_name(err));
    else ESP_LOGI(TAG, "logos mmap'd (%u KB), %d stations in DB",
                  (unsigned)(p->size / 1024), STATIONS_ALL_COUNT);
    return err;
}

static void fill_dsc(lv_image_dsc_t *d, uint32_t off, uint16_t w, uint16_t h)
{
    d->header.magic  = LV_IMAGE_HEADER_MAGIC;
    d->header.cf     = LV_COLOR_FORMAT_RGB565;
    d->header.w      = w;
    d->header.h      = h;
    d->header.stride = w * 2;
    d->data          = s_map + off;
    d->data_size     = (uint32_t)w * h * 2;
}

void stations_set_area(int area)
{
    if (area < 1 || area > 47) area = 13;
    uint64_t bit = 1ULL << (area - 1);
    s_count = 0;
    for (int i = 0; i < STATIONS_ALL_COUNT && s_count < MAX_STATIONS; i++) {
        if (!(STATIONS_ALL[i].area_mask & bit)) continue;
        const station_t *st = &STATIONS_ALL[i];
        s_active[s_count] = i;
        if (s_map) {
            fill_dsc(&s_big[s_count],   st->big_off, st->big_w, st->big_h);
            fill_dsc(&s_small[s_count], st->sm_off,  st->sm_w,  st->sm_h);
        }
        s_count++;
    }
    ESP_LOGI(TAG, "area JP%d: %d stations", area, s_count);
}

int stations_count(void) { return s_count; }

const char *station_id(int i)
{
    return (i >= 0 && i < s_count) ? STATIONS_ALL[s_active[i]].id : "";
}
const char *station_name(int i)
{
    return (i >= 0 && i < s_count) ? STATIONS_ALL[s_active[i]].name : "";
}
const lv_image_dsc_t *station_logo_big(int i)
{
    return (i >= 0 && i < s_count) ? &s_big[i] : NULL;
}
const lv_image_dsc_t *station_logo_small(int i)
{
    return (i >= 0 && i < s_count) ? &s_small[i] : NULL;
}
