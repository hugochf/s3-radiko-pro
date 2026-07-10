idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl display esp_lcd esp_timer stations fonts wifi
)
