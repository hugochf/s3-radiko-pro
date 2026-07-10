idf_component_register(
    SRCS "httpc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client
    PRIV_REQUIRES mbedtls esp-tls
)
