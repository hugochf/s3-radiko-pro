// FT6336G capacitive touch via ESP-IDF i2c_master (shared bus with ES8311)
// Pins: INT=17, RST=18; bus (SDA=16, SCL=15) provided by shared g_i2c_bus handle
#pragma once
#include "driver/i2c_master.h"

#define TOUCH_INT  17
#define TOUCH_RST  18
#define FT6336_ADDR  0x38

int touch_last_x = 0, touch_last_y = 0;
static uint16_t _tw = 320, _th = 240;

extern i2c_master_bus_handle_t g_i2c_bus;
static i2c_master_dev_handle_t _ft_dev = nullptr;

static uint8_t _ft_read_reg(uint8_t reg) {
  uint8_t val = 0;
  i2c_master_transmit_receive(_ft_dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
  return val;
}

void touch_init(uint16_t w, uint16_t h, uint8_t /*rot*/) {
  _tw = w; _th = h;

  // Hardware reset FT6336
  pinMode(TOUCH_INT, INPUT);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, HIGH); delay(20);
  digitalWrite(TOUCH_RST, LOW);  delay(20);
  digitalWrite(TOUCH_RST, HIGH); delay(500);

  // Add FT6336 to shared i2c_master bus
  i2c_device_config_t ft_cfg = {};
  ft_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  ft_cfg.device_address  = FT6336_ADDR;
  ft_cfg.scl_speed_hz    = 400000;
  i2c_master_bus_add_device(g_i2c_bus, &ft_cfg, &_ft_dev);
}

bool touch_touched() {
  uint8_t td = _ft_read_reg(0x02);  // TD_STATUS: number of touches
  if (td == 0 || td > 2) return false;

  uint8_t data[4];
  uint8_t reg = 0x03;
  i2c_master_transmit_receive(_ft_dev, &reg, 1, data, 4, pdMS_TO_TICKS(100));

  uint16_t raw_x = (uint16_t)((data[0] & 0x0F) << 8) | data[1];
  uint16_t raw_y = (uint16_t)((data[2] & 0x0F) << 8) | data[3];

  // ROTATION_RIGHT: x = raw_y, y = width(240) - raw_x
  uint16_t x = raw_y;
  uint16_t y = 240 - raw_x;

  touch_last_x = map(x, 0, 320, 0, (int)_tw - 1);
  touch_last_y = map(y, 0, 240, 0, (int)_th - 1);
  return true;
}
