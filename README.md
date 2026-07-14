# s3-radiko-pro

A touchscreen [Radiko](https://radiko.jp/) internet radio for the ESP32-S3,
built from scratch in **ESP-IDF**. It authenticates against Radiko, pulls the
live HLS stream, decodes HE-AAC on-device, and plays it through an I2S DAC — all
driven by an LVGL touch UI. No `esp-adf`, no vendored 250 MB SDK: the audio
pipeline is hand-written and owned in this repo.

This is a ground-up re-implementation of a working Arduino project, rebuilt as a
vehicle for learning commercial-grade embedded practices (CI from commit 0,
dual-OTA partition layout, versioned NVS, structured error handling). Progress is
tracked phase-by-phase in [PLAN.md](PLAN.md), whose **Lessons learned** section is
the running engineering log.

## Status

Live Radiko audio plays, touchscreen-controlled. Working today:

- Wi-Fi provisioning (on-screen scan + password) with NVS-persisted credentials
- SNTP time sync (JST) — required for Radiko auth
- Radiko `auth1`/`auth2` and live HLS playback (HE-AAC / `mp4a.40.5`, SBR)
- Player UI (Arduino-parity): swipeable full-width logo, play/pause, prev/next,
  volume, sleep timer, WS2812 mood-LED modes
- "Now on air" program info (title + performers) on the player and per list row,
  full-CJK font, refreshed every 5 min
- Settings page: brightness, screen dim/off timeouts, sleep timer, flip-180°,
  screen saver, system/firmware info — all NVS-persisted
- Screen saver (DVD-style bouncing clock) and battery gauge (ADC, status-bar %)
- Instant pause/switch, debounced station navigation, persisted station & volume

Roadmap next: the Tier D hardening pass (error handling, watchdog tuning, unit
tests, OTA). See [PLAN.md](PLAN.md).

## Hardware

**Board:** lcdwiki ES3C28P — ESP32-S3 (16 MB QIO flash, 8 MB OPI PSRAM), ILI9341
320×240 SPI LCD, FT6336G capacitive touch, ES8311 codec + FM8002E amp.

Full pin map and panel quirks: [docs/board-lcdwiki-ES3C28P.md](docs/board-lcdwiki-ES3C28P.md).

## Build & flash

Requires **ESP-IDF v5.3.5**.

```sh
. ~/esp/v5.3.5/esp-idf/export.sh      # source the toolchain
cd idf
idf.py set-target esp32s3             # first time only
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

Wi-Fi credentials seed from a gitignored `components/wifi/wifi_secrets.h` on first
boot (copy `wifi_secrets.h.example`), or enter them on-screen. `sdkconfig` is
generated and gitignored — the committed deltas live in `idf/sdkconfig.defaults`.

## Architecture (short version)

Two-stage streaming pipeline: a **fetcher** task keeps a queue of AAC segments
full while a **decoder** task drains it through libhelix into a 30-second PCM
ring buffer that an I2S writer feeds to the DAC. Both live on core 0 with the
rest of the networking; core 1 belongs to LVGL and the I2S writer alone. This
decoupling is what makes live playback keep up with a CDN that serves segments
at ~1× real time and ride through Radiko's ~5-minute session re-resolves.

Task/core/priority map, the internal-RAM-vs-PSRAM memory budget, and the full
data flow are in [docs/architecture.md](docs/architecture.md). Debugging the
board (USB-JTAG recovery, coredumps, serial capture) is in
[docs/debugging.md](docs/debugging.md).

## Repository layout

```
PLAN.md              roadmap + engineering log (Lessons learned)
docs/                board reference, architecture, debugging runbook
idf/
  main/              app entry + init sequence
  components/        one component per concern:
    display  ui  fonts  logos  stations   — screen + assets (LVGL v9)
    touch  i2c_bus                          — input
    wifi  timesync  httpc  radiko           — network + auth
    stream  libhelix_aac  audio             — HLS pipeline + decode + I2S
    settings  led  battery                  — versioned NVS config, mood LED, gauge
  sdkconfig.defaults partitions.csv         — build config, dual-OTA flash map
.github/workflows/   CI: build firmware on every push
```

## Licensing notes

- `components/libhelix_aac/` vendors the **Helix AAC** decoder under the RealNetworks
  Public Source License (RPSL) — see that directory.
- Station logos are the trademarks of their respective broadcasters, embedded here
  for a personal, non-commercial reproduction of the physical radio's UI.
- Original Arduino project by the author; this ESP-IDF rebuild is the same author's.
