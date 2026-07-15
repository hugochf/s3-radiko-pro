# Boot splash only. Station logos moved to the `logos` flash partition in
# Phase 30 (nationwide set, mmap'd by the stations component) — they no longer
# bloat the app slot as EMBED_FILES.
idf_component_register(
    SRCS "splash_gen.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    EMBED_FILES "splash.bin"
)
