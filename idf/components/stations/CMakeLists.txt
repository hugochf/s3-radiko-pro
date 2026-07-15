idf_component_register(
    SRCS "stations.c" "stations_gen.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES esp_partition
)
