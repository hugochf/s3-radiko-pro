idf_component_register(
    SRCS "radiko_auth.c" "program.c" "puff.c" "radiko_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES httpc esp_http_client
    PRIV_REQUIRES mbedtls stations
)
