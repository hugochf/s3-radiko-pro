#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BAT_ADC_CH ADC_CHANNEL_8   // GPIO9, voltage divider ×2

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;
static int s_mv_avg = 0;   // exponential moving average of the cell voltage

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) return err;

    // 12 dB attenuation covers the divided cell voltage (4.2 V / 2 = 2.1 V).
    adc_oneshot_chan_cfg_t chan = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CH, &chan);
    if (err != ESP_OK) return err;

    adc_cali_curve_fitting_config_t cali = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration — raw readings only");
        s_cali = NULL;
    }
    ESP_LOGI(TAG, "ADC1 ch%d (GPIO9) ready", BAT_ADC_CH);
    return ESP_OK;
}

int battery_mv(void)
{
    if (!s_adc || !s_cali) return -1;

    // Average 8 samples to reduce ADC noise, then EMA (alpha ~0.1) across calls.
    long sum = 0;
    int  good = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CH, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
            good++;
        }
    }
    if (good == 0) return -1;

    int mv_now = (int)(sum / good) * 2;   // undo the divider
    s_mv_avg = (s_mv_avg == 0) ? mv_now : (s_mv_avg * 9 + mv_now) / 10;
    return s_mv_avg;
}

int battery_pct(void)
{
    int mv = battery_mv();
    if (mv < 0) return -1;
    // Approximate Li-ion discharge curve:
    // 4200 mV = 100%, 3900 ~75%, 3700 ~50%, 3550 ~25%, 3300 ~5%, 3000 = 0%.
    if (mv >= 4200) return 100;
    if (mv >= 3900) return 75 + (mv - 3900) * 25 / 300;
    if (mv >= 3700) return 50 + (mv - 3700) * 25 / 200;
    if (mv >= 3550) return 25 + (mv - 3550) * 25 / 150;
    if (mv >= 3300) return 5 + (mv - 3300) * 20 / 250;
    if (mv >= 3000) return (mv - 3000) * 5 / 300;
    return 0;
}

bool battery_charging(void)
{
    // This board exposes NO charge-status pin (TP4054 CHRG -> LED only; the
    // Q3 USB-power signal isn't on a GPIO — verified against the schematic and
    // BSP). So charging can only be INFERRED from voltage, which is honest only
    // when the cell is clearly climbing (CC charge of a not-full battery). Near
    // full the charger holds voltage flat (CV), indistinguishable from idle —
    // there we deliberately report "not charging" rather than guess wrong.
    //
    // 15-slot ring at the ~2 s status-bar cadence ≈ 30 s window; require a
    // clear >30 mV rise so ADC noise (±5 mV) and brief load-recovery bumps
    // don't trigger a false "charging".
    static int  history[15] = { 0 };
    static int  idx = 0;
    static bool filled = false;

    int mv = s_mv_avg;
    history[idx] = mv;
    idx = (idx + 1) % 15;
    if (idx == 0) filled = true;

    if (!filled) return false;
    int oldest = history[idx];   // next write slot = oldest sample
    return mv > oldest + 30;
}
