idf_component_register(
    SRCS "stream.c"
    INCLUDE_DIRS "include"
    REQUIRES httpc radiko esp_http_client libhelix_aac audio esp_hw_support
)
