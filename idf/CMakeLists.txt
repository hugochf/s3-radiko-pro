# s3-radiko-pro — ESP-IDF reimplementation of the Arduino S3 Radiko player.
# Pinned to ESP-IDF v5.3.x. See ../PLAN.md for the phased roadmap.
cmake_minimum_required(VERSION 3.16)

# Firmware version — surfaced at boot and (later) used by the OTA version check.
set(PROJECT_VER "0.0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3_radiko_pro)
