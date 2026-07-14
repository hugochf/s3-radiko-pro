# Embed the downscaled RGB565 station logos at build time. Symbols become
# _binary_<name>_bin_start (used by logos_gen.c).
file(GLOB LOGO_BINS RELATIVE ${CMAKE_CURRENT_LIST_DIR} "*.bin")

idf_component_register(
    SRCS "logos_gen.c" "logos_small_gen.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    EMBED_FILES ${LOGO_BINS}
)
