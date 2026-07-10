#include "touch.h"

#include "display.h"
#include "esp_check.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "touch";

// FT6336G: I2C addr 0x38, INT=17, RST=18 (shared bus SDA=16/SCL=15).
#define PIN_TOUCH_RST 18
#define PIN_TOUCH_INT 17

static esp_lcd_touch_handle_t s_touch = NULL;

// Called by LVGL (in the LVGL task) to poll the touch controller.
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    uint8_t count = 0;

    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &count, 1);

    if (pressed && count > 0) {
        // Flip X here rather than via the driver's mirror_x: esp_lcd_touch mirrors
        // on the raw axis (0..240) using x_max=320 before the swap, which offsets
        // and kills the top edge. Doing it post-swap on the display axis is clean.
        data->point.x = (DISPLAY_H_RES - 1) - x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t touch_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.scl_speed_hz = 400000;  // required for the i2c_master (v2) bus
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus_handle(), &io_cfg, &io),
                        TAG, "touch io");

    // Map raw FT6336 coords (native 240x320 portrait) to the display's
    // upside-down 320x240 landscape. These flags mirror display_init()'s
    // swap_xy(true)+mirror(true,true) and are verified on-device.
    esp_lcd_touch_config_t cfg = {
        .x_max        = DISPLAY_H_RES,
        .y_max        = DISPLAY_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        // No INT: touch is polled in touch_read_cb, so the INT pin buys nothing.
        // Worse, esp_lcd_touch installs a GPIO ISR for it, and the default GPIO
        // ISR service is NOT in IRAM. A touch edge that fires during a flash
        // write (NVS commit, cache disabled on both cores) would try to run that
        // ISR from uncached flash -> unrecoverable hard wedge (no panic, no
        // coredump). Mashing prev/next made INT edges collide with the per-press
        // NVS write and hung the whole chip. Polling alone is fully responsive.
        .int_gpio_num = GPIO_NUM_NC,
        // swap_xy only; X is flipped manually in touch_read_cb (see note there).
        // y_max=240 unused (no driver mirror). Verified against all four corners.
        .flags = {
            .swap_xy  = 1,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(io, &cfg, &s_touch), TAG, "ft6336");

    // Register the LVGL input device under the LVGL lock (task is already running).
    ui_lock(-1);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    ui_unlock();

    ESP_LOGI(TAG, "FT6336 touch ready");
    return ESP_OK;
}
