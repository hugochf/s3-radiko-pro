idf_component_register(
    SRCS "touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd_touch_ft5x06 i2c_bus display ui lvgl
)
