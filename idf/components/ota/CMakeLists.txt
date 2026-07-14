idf_component_register(
    SRCS "ota.c" "ota_parse.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_https_ota esp_http_client esp_app_format mbedtls esp_partition app_update
)
