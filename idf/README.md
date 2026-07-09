# s3-radiko-pro

An ESP-IDF reimplementation of the Arduino [S3 Radiko](../S3_Radiko.ino) internet
radio, rebuilt as a vehicle for practising **commercial-grade embedded
engineering** (OTA, secure boot, CI/CD, unit tests, JTAG, reproducible builds).

> **Learning project only — not for sale or distribution.** Radiko's Terms of
> Service prohibit unofficial clients, the station logos are trademarked, and some
> libraries under evaluation are copyleft. See [../PLAN.md](../PLAN.md).

## Hardware

- **Board:** lcdwiki ES3C28P — ESP32-S3, 16 MB flash, 8 MB OPI PSRAM
- **Display:** ILI9341 320×240 SPI · **Touch:** FT6336G (I²C) · **Audio:** ES8311 codec + FM8002E amp (I²S)
- **Full pin map + panel quirks:** [../docs/board-lcdwiki-ES3C28P.md](../docs/board-lcdwiki-ES3C28P.md) (reusable board reference).

## Toolchain

- **ESP-IDF v5.3.5** (`~/esp/v5.3.5/esp-idf`)
- Console/flash/debug over the board's native USB (USB-Serial-JTAG)

## Build & flash

```sh
. ~/esp/v5.3.5/esp-idf/export.sh   # once per shell
cd idf
idf.py set-target esp32s3          # first time only
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

At Phase 0 the firmware just prints a boot banner (chip, flash, PSRAM, heap) and
a 10-second heartbeat — enough to confirm the toolchain, partition table, PSRAM,
and USB console are all healthy.

## Flash layout

Dual-OTA from day one (`partitions.csv`): two 3 MB app slots, plus reserved
`nvs_keys` (NVS encryption, Phase 7) and `coredump` (Phase 19) so the map never
changes once devices ship. `storage` (~9.6 MB SPIFFS) is for embedded assets and
logs later.

## Roadmap & progress

The full 30-phase plan and the running "lessons learned" log live in
[../PLAN.md](../PLAN.md). One phase per session, each ending with a green build,
a green CI run, and commits.
