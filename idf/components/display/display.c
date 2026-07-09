#include "display.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// --- Pin map (from Arduino TFT_eSPI User_Setup.h, lcdwiki ES3C28P) ---
#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13   // unused for write-only, wired anyway
#define PIN_CS   10
#define PIN_DC   46
#define PIN_RST  (-1) // tied to board RST -> software reset
#define PIN_BL   45   // backlight, active HIGH

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_PCLK_HZ    (40 * 1000 * 1000)
#define LCD_CMD_BITS   8
#define LCD_PARAM_BITS 8

// --- Backlight PWM ---
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_RES     LEDC_TIMER_8_BIT

// One flush band for the test pattern; also a sane DMA transfer ceiling.
#define BAND_LINES 40

static esp_lcd_panel_handle_t    s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;

// draw_bitmap() queues the SPI transfer and returns before it completes, so the
// pixel buffer must stay untouched until this fires. Given per draw, so callers
// can reuse/free the buffer safely.
static SemaphoreHandle_t s_trans_done = NULL;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &hp);
    return hp == pdTRUE;
}

// Draw one region and block until its DMA transfer has fully consumed `buf`.
static void draw_wait(int x0, int y0, int x1, int y1, const void *buf)
{
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, buf);
    xSemaphoreTake(s_trans_done, portMAX_DELAY);
}

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num   = PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,   // start dark
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void display_backlight_set(uint8_t duty)
{
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_err_t display_init(void)
{
    backlight_init();

    ESP_LOGI(TAG, "init SPI bus (host %d, sclk %d, mosi %d)", LCD_SPI_HOST, PIN_SCLK, PIN_MOSI);
    spi_bus_config_t bus = {
        .sclk_io_num     = PIN_SCLK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_H_RES * BAND_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = PIN_CS,
        .dc_gpio_num       = PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io),
        TAG, "panel io");
    s_io = io;

    // Completion signal for async color transfers (see draw_wait()).
    s_trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_trans_done, ESP_ERR_NO_MEM, TAG, "semaphore");
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL),
                        TAG, "io callbacks");

    // Standard ILI9341-over-SPI: BGR element order + big-endian (byte-swapped)
    // pixel data. Both are needed together; with only one, colours swap channels.
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel), TAG, "ili9341");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));  // software reset (RST=-1)
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // This panel's pixels are inverted (0xFFFF shows as black), so invert on.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    // Native 240x320 portrait -> 320x240 landscape, oriented for the upside-down
    // mounting. With swap_xy on, mirror_x maps to the vertical display axis and
    // mirror_y to the horizontal one; (true,true) puts logical (0,0) at the
    // viewer's top-left when the device is held the right way up.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "ILI9341 ready, %dx%d landscape", DISPLAY_H_RES, DISPLAY_V_RES);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void)
{
    return s_panel;
}

esp_err_t display_test_pattern(void)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    // Orientation test: four quadrants of the intended landscape frame. This is
    // unambiguous — reading which colour lands in which corner pins down the exact
    // swap_xy / mirror transform. ILI9341-over-SPI wants big-endian RGB565, so
    // byte-swap each pixel (ESP32 is little-endian).
    // Quadrants TL=red TR=green BL=blue BR=yellow. Pixels are byte-swapped to
    // big-endian to match this ILI9341's SPI wire order (paired with BGR order in
    // display_init). LVGL will do this swap for us in Phase 2.
    const uint16_t RED   = __builtin_bswap16(0xF800);
    const uint16_t GREEN = __builtin_bswap16(0x07E0);
    const uint16_t BLUE  = __builtin_bswap16(0x001F);
    const uint16_t YEL   = __builtin_bswap16(0xFFE0);

    uint16_t *band = heap_caps_malloc(DISPLAY_H_RES * BAND_LINES * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(band, ESP_ERR_NO_MEM, TAG, "band buffer");

    for (int y = 0; y < DISPLAY_V_RES; y += BAND_LINES) {
        int h = (y + BAND_LINES <= DISPLAY_V_RES) ? BAND_LINES : (DISPLAY_V_RES - y);
        for (int row = 0; row < h; row++) {
            bool bottom = (y + row) >= DISPLAY_V_RES / 2;
            for (int x = 0; x < DISPLAY_H_RES; x++) {
                bool right = x >= DISPLAY_H_RES / 2;
                band[row * DISPLAY_H_RES + x] =
                    bottom ? (right ? YEL : BLUE) : (right ? GREEN : RED);
            }
        }
        draw_wait(0, y, DISPLAY_H_RES, y + h, band);
    }

    heap_caps_free(band);
    ESP_LOGI(TAG, "byte-swapped quadrant test drawn (TL=red TR=green BL=blue BR=yellow)");
    return ESP_OK;
}
