# s3-radiko-pro — ESP-IDF reimplementation of the Arduino S3 Radiko player.
# Pinned to ESP-IDF v5.3.x. See ../PLAN.md for the phased roadmap.
cmake_minimum_required(VERSION 3.16)

# Firmware version — surfaced at boot and compared against the latest GitHub
# release tag by the OTA check (Phase 22). Bump when cutting a release.
set(PROJECT_VER "0.31.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL keeps dirty rectangles in a fixed array and, when it overflows, SILENTLY
# REPAINTS THE WHOLE SCREEN (lv_refr.c: "If no place for the area add the
# screen"). The LED visualiser's ~74 objects invalidate ~148 areas a frame, blew
# the default 32, and every frame became a full 320x240 repaint — a rock-steady
# 163 ms stall that froze the UI and dragged the decoder under real-time. Not
# exposed in LVGL's Kconfig, hence -D. Raising it lets viz_tick's single
# whole-strip lv_obj_invalidate() actually swallow the small areas instead of
# being the slot that tips it over. (Phase 32)
idf_build_set_property(COMPILE_OPTIONS "-DLV_INV_BUF_SIZE=192" APPEND)

project(s3_radiko_pro)
