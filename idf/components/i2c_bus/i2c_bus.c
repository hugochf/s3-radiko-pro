#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

#define I2C_PORT I2C_NUM_0
#define PIN_SDA  16
#define PIN_SCL  15

static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus) return ESP_OK;  // idempotent

    i2c_master_bus_config_t cfg = {
        .i2c_port                     = I2C_PORT,
        .sda_io_num                   = PIN_SDA,
        .scl_io_num                   = PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "i2c bus");
    ESP_LOGI(TAG, "shared I2C bus up (SDA=%d SCL=%d)", PIN_SDA, PIN_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_handle(void)
{
    return s_bus;
}

static SemaphoreHandle_t s_i2c_mutex;
void i2c_bus_lock(void)
{
    if (!s_i2c_mutex) s_i2c_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
}
void i2c_bus_unlock(void) { xSemaphoreGive(s_i2c_mutex); }
