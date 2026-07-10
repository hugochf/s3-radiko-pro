idf_component_register(
    SRCS "wifi.c"
    INCLUDE_DIRS "include"
    # Private include dir so wifi_secrets.h (next to wifi.c) is found.
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_timer
)
