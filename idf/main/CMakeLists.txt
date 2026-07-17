# Declaring REQUIRES opts main out of its implicit "depend on every component"
# behaviour, so list what main.c actually uses (common components — heap, log,
# freertos, esp_common — are always available and need no mention).
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hw_support spi_flash esp_app_format esp_psram nvs_flash
             display ui i2c_bus touch wifi settings stations timesync radiko audio stream led battery
             app_watchdog crashlog elog app_update storage recorder
)
