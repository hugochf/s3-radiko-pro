idf_component_register(
    SRCS "audio.c" "es8311.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_i2c i2c_bus
)
