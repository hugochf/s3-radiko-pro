# s3-radiko-pro — Plan

ESP-IDF reimplementation of the Arduino-based S3 Radiko player as a learning
vehicle for **commercial-grade embedded development practices** on macOS.

## Disclaimer

This project is **for learning only**. It is NOT intended for sale or
distribution because:

1. Radiko's Terms of Service prohibit unofficial clients
2. Station logos are trademarked
3. Some libraries we may evaluate (e.g. ESP32-audioI2S) are GPL-licensed

The point of this project is to practice every commercial-grade engineering
discipline (OTA, secure boot, CI/CD, unit tests, error handling, JTAG debug,
reproducible builds, etc.) on a non-trivial codebase you already understand.

## Hardware

Same as the Arduino prototype:

- **Board**: lcdwiki ES3C28P (ESP32-S3 + ILI9341 + FT6336G + ES8311 + FM8002E)
- **Specs**: 16 MB flash, 8 MB OPI PSRAM
- **Reference**: see Arduino prototype `~/Documents/Arduino/S3_Radiko/`
  for pin assignments, hardware quirks, and feature behaviour to replicate

## Toolchain (already installed)

- **ESP-IDF v5.3.5** at `~/esp/v5.3.5/esp-idf/` (source `export.sh` per shell)
- **Cursor IDE** with Espressif IDF extension v1.10.0
- **Homebrew Python 3.11** at `/opt/homebrew/bin/python3.11` (NOT Anaconda)
- **ninja, cmake, git** from Homebrew

## Decisions (resolved 2026-07-09)

- [x] **Project root path**: nested in the existing repo at `S3_Radiko/idf/`
      (the Arduino prototype stays alongside as reference).
- [x] **Repo**: single repo, **public** GitHub. The hardcoded WiFi password in the
      Arduino sketch is a throwaway test credential (being rotated), so it's fine
      to publish. New port never hardcodes credentials (Kconfig + gitignored
      `secrets.h`, then Phase 6 provisioning).
- [x] **License**: MIT (`idf/LICENSE`). Third-party components keep their own
      licenses (e.g. the ES8311 driver is Apache-2.0).
- [x] **Commit cadence**: honest "every step" history — commit frequently within a
      phase, warts and all.
- [x] **Audio framework**: **esp-adf first** to learn the pipeline pattern; revisit
      libhelix-aac later if footprint matters. Audio component interface kept
      abstract so either backend fits.
- [ ] **Secure boot timing**: final phase (Phase 25) — keep debugging easy until then.

## The hard problem: replacing ESP32-audioI2S

The Arduino library is GPL-3 and does **a lot**:

- HLS playlist (m3u8) parsing and chunk fetching
- HTTPS connection management  
- AAC decoding
- ICY metadata extraction
- I2S output to codec

Replacement options:

| Option | License | Pros | Cons |
|--------|---------|------|------|
| **esp-adf** (Espressif Audio Framework) | Apache 2.0 | Official, componentized, well-documented audio pipeline | ~250 MB SDK download, large dependency |
| **libhelix-aac** + custom HLS | BSD | Tiny, focused, you own the pipeline | Have to write HLS parser, chunk fetcher, ICY metadata yourself |
| **fdk-aac** + custom HLS | FDK License (commercial OK) | Higher quality decoder | Larger code footprint |

**Recommendation**: start with **esp-adf** to learn the audio pipeline pattern,
then optionally rewrite with libhelix-aac later if footprint matters.

## Phased plan

### Tier A — Foundation

| # | Phase | Commercial concern taught | Deliverables |
|---|-------|---------------------------|--------------|
| 0 | **Project skeleton** | Reproducible builds, partition layout for OTA from day one, sdkconfig.defaults committed, CI yaml stub, pinned IDF version | `CMakeLists.txt`, `partitions.csv` (2× app + nvs + nvs_keys + coredump + storage), `sdkconfig.defaults`, `idf_component.yml`, `main/main.c`, `.github/workflows/build.yml`, `README.md`, `LICENSE` |
| 1 | **Display driver via esp_lcd** | ESP-IDF native peripheral drivers (no Arduino abstraction), DMA-friendly buffers, proper init sequences | `components/display/` with ILI9341 init via `esp_lcd_panel_io_spi` |
| 2 | **LVGL via managed component** | Managed components (`idf.py add-dependency espressif/lvgl`), version pinning, lv_conf via Kconfig | `components/ui/` builds with LVGL pinned to v9.x |
| 3 | **Touch driver** | I2C bus sharing (codec + touch on same bus), `esp_lcd_touch_ft6336` | `components/touch/`, touch points reach LVGL |
| 4 | **Player UI port** | LVGL screens identical to Arduino version with stub data | All screens build, no real data, no audio |

**End of Tier A**: device boots, shows the player UI, touch responds, no audio, no network.

### Tier B — Network & infrastructure

| # | Phase | Commercial concern taught | Deliverables |
|---|-------|---------------------------|--------------|
| 5 | **WiFi state machine** | `esp_event` proper usage, retry/backoff, multi-AP, signal-strength handling | `components/wifi/` |
| 6 | **WiFi provisioning** | First-boot UX, BLE or SoftAP provisioning via `wifi_provisioning` | First-boot wizard via phone app |
| 7 | **NVS settings** | NVS encryption, schema versioning, defaults, corruption recovery | `components/settings/` |
| 8 | **NTP time sync** | Multi-server NTP, JST timezone, fallback when no network | `components/time/` |
| 9 | **HTTPS + esp-tls** | Certificate bundles, server validation (NOT `setInsecure()`) | base for Radiko auth |

### Tier C — Radiko features

| # | Phase | Commercial concern taught | Deliverables |
|---|-------|---------------------------|--------------|
| 10 | **Radiko auth1/auth2** | HTTPS API client pattern, header parsing, key derivation | `components/radiko/auth.c` |
| 11 | **Audio output: I2S + ES8311** | `esp_codec_dev` driver, no Arduino layer | `components/audio/codec_es8311.c` |
| 12 | **HLS pipeline (esp-adf)** | Audio pipeline architecture, element-based design | `components/audio/pipeline.c` — playable! |
| 13 | **Station logos as embedded data** | Build-time asset embedding via `EMBED_FILES` in CMake | logos via cmake, not generated `.c` |
| 14 | **Program info fetch** | Background tasks, gzip via miniz/zlib, simple XML parsing | `components/radiko/program.c` |
| 15 | **Settings page port** | Same UI as Arduino but driven by NVS schema | settings screen |
| 16 | **Screen saver port** | LVGL animations, power management interactions | bouncing rainbow clock |

### Tier D — Production engineering

| # | Phase | Commercial concern taught | Deliverables |
|---|-------|---------------------------|--------------|
| 17 | **Error handling pass** | Replace every error path with proper recovery | refactor across components |
| 18 | **Watchdog tuning** | TWDT per task, reset thresholds, panic handler, brownout | `app_watchdog.c` |
| 19 | **Crash dump partition** | `esp_core_dump`, retrieval on next boot, optional upload | partition + recovery flow |
| 20 | **Logging to NVS ring buffer** | Boot/error log retrievable via USB | log component |
| 21 | **Unit tests** | Unity framework, host tests for parsers, target tests for HW | `test/` |
| 22 | **OTA from GitHub releases** | A/B rollback, signed firmware, version check, progress UI | `components/ota/` |
| 23 | **CI/CD: build + test + release** | GitHub Actions, pinned toolchain, signed artifacts | `.github/workflows/` |
| 24 | **JTAG debug session** | OpenOCD via built-in USB-JTAG, breakpoints, watch vars | walkthrough only, no new code |
| 25 | **Secure boot v2 + flash encryption** | Efuse one-shot, key management, locked production firmware | irreversible — production-locked binary |

### Tier E — Polish (optional)

| # | Phase | Concern |
|---|-------|---------|
| 26 | Localization framework | Japanese UI strings via lookup table |
| 27 | Better volume curve, equalizer | Audio quality |
| 28 | Bluetooth output | A2DP source |
| 29 | Time-free recording | Persistent storage to SD |
| 30 | Multi-area support | Not just JP14 |

## Workflow rules

These rules govern how each phase is executed. They are part of the learning,
not optional shortcuts.

1. **One phase per session**, fully completed before moving on.
2. **Each phase ends with a green build, a green CI run, and a single commit.**
3. **No skipping the engineering concern.** If a phase says "error handling",
   it means actually handle errors, not stub `// TODO`.
4. **Every component gets at least one unit test** by Tier D Phase 21 — write
   them as you go, don't backfill.
5. **Pinned everything**: ESP-IDF version, managed component versions, build
   flags. The `sdkconfig` deltas from `sdkconfig.defaults` should be tiny.
6. **Document gotchas in this file** as we hit them, in the "Lessons" section
   below. This becomes the learning artifact.
7. **No `printf` debugging committed** — use `ESP_LOGI/ESP_LOGW/ESP_LOGE` and
   leave them in (proper log levels).

## Estimated effort

Realistic estimate for evening/weekend pace:

- **Tier A**: 1–2 weeks
- **Tier B**: 1–2 weeks  
- **Tier C**: 2–4 weeks (audio pipeline is the long pole)
- **Tier D**: 2–3 weeks (error handling pass + tests are slow)
- **Tier E**: optional, anytime

**Total: 2–3 months** of focused part-time work.

Skipping the engineering concerns to "just get it working" defeats the
purpose. Take the time per phase.

## Phase tracker

- [x] **Phase 0** — Project skeleton ✅ green build + hardware boot + green CI (public repo)
- [x] **Phase 1** — Display driver via esp_lcd ✅ ILI9341 up, colours + orientation correct on-device
- [ ] **Phase 2** — LVGL via managed component
- [ ] **Phase 3** — Touch driver
- [ ] **Phase 4** — Player UI port
- [ ] **Phase 5** — WiFi state machine
- [ ] **Phase 6** — WiFi provisioning
- [ ] **Phase 7** — NVS settings
- [ ] **Phase 8** — NTP time sync
- [ ] **Phase 9** — HTTPS + esp-tls
- [ ] **Phase 10** — Radiko auth1/auth2
- [ ] **Phase 11** — Audio output: I2S + ES8311
- [ ] **Phase 12** — HLS pipeline (esp-adf)
- [ ] **Phase 13** — Station logos as embedded data
- [ ] **Phase 14** — Program info fetch
- [ ] **Phase 15** — Settings page port
- [ ] **Phase 16** — Screen saver port
- [ ] **Phase 17** — Error handling pass
- [ ] **Phase 18** — Watchdog tuning
- [ ] **Phase 19** — Crash dump partition
- [ ] **Phase 20** — Logging to NVS ring buffer
- [ ] **Phase 21** — Unit tests
- [ ] **Phase 22** — OTA from GitHub releases
- [ ] **Phase 23** — CI/CD: build + test + release
- [ ] **Phase 24** — JTAG debug session
- [ ] **Phase 25** — Secure boot v2 + flash encryption

## Lessons learned

Append discoveries here as we work through the phases. This is the actual
learning artifact.

### Phase 0 — Project skeleton

- **`esp_chip_info_t` has `revision` (not `full_revision`) in IDF v5.3.5.** It's
  encoded as `major*100 + minor` (e.g. `2` → v0.2). `full_revision` only exists on
  newer IDF. Use `chip.revision / 100` and `% 100`.
- **App partitions must start on a 64 KB (0x10000) boundary.** The gap between
  `nvs_keys` (ends 0x12000) and `ota_0` (starts 0x20000) is required alignment
  padding, not waste. `otadata` must be exactly 0x2000 (two sectors).
- **`nvs_keys` partition carries the `encrypted` flag** so it's ready for NVS
  encryption in Phase 7 without a flash-map change.
- **`sdkconfig` is gitignored; only `sdkconfig.defaults` is committed** (10 lines).
  That keeps the "delta from IDF defaults" visible and small (workflow rule 5).
- **Console on USB-Serial-JTAG** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) so one
  cable does flash + monitor + JTAG. Change if the board routes USB elsewhere —
  verify on first `idf.py monitor`.
- **CI** (`.github/workflows/build.yml`) lives at the *repo root*, not in `idf/`
  (GitHub requires `.github/` at root), and points at the subproject via `path: idf`.
- IDE (clangd) flags xtensa-gcc flags like `-mlongcalls` as errors — that's a
  clangd/toolchain mismatch, harmless. Trust `idf.py build`, not the squiggles.
- **Two USB ports enumerate**: `/dev/cu.usbmodem2101` (native USB-Serial-JTAG,
  what we flash/monitor) and `/dev/cu.usbserial-0001` (a UART bridge). Console is
  on the JTAG port. Flash+monitor: `idf.py -p /dev/cu.usbmodem2101 flash monitor`.
- **Hardware baseline (Phase 0):** free heap ~8531 KB internal + 8189 KB PSRAM,
  CPU 160 MHz, chip rev v0.2, PSRAM AP gen-3 64 Mbit @ 80 MHz. Reset cause after
  flash: `USB_UART_CHIP_RESET`. Track heap against this baseline as features land.
- **CI gotcha:** a `paths:` filter on the workflow's `push` trigger caused the
  repo's *initial* commit to skip the run, and until a run fires GitHub doesn't
  register/list the workflow at all (`actions/workflows` returns 0). Dropping the
  path filter (run on every push) fixed registration. Repo:
  https://github.com/hugochf/s3-radiko-pro — CI green in ~2m10s (ESP-IDF v5.3.5).
- **Publishing over HTTPS needs a token with `workflow` scope**, else the push
  that adds `.github/workflows/*` is rejected. `gh auth` already had it.
- Minor: `actions/checkout@v4` + `upload-artifact@v4` emit a Node 20 deprecation
  warning on GitHub runners — cosmetic; bump to v5 when convenient.

### Phase 1 — Display driver (ILI9341 via esp_lcd)

- **Pin map (from Arduino TFT_eSPI `User_Setup.h`):** SPI2_HOST, SCLK=12, MOSI=11,
  MISO=13, CS=10, DC=46, RST=−1 (software reset), backlight=45 (active-high, LEDC
  PWM). 40 MHz pclk. ILI9341 vendor driver = managed component
  `espressif/esp_lcd_ili9341` (^1.2.0); it's not in esp_lcd core.
- **This panel needed THREE non-default quirks, found empirically on-device.**
  Getting one wrong disguises the others, so debug them with unambiguous, isolated
  test patterns (solid fills, single-corner squares), not multi-colour patterns:
  1. **Inverted pixels** → `esp_lcd_panel_invert_color(panel, true)`. Tell-tale:
     white (0xFFFF) shows as black.
  2. **BGR order + big-endian data, together** → `rgb_ele_order = BGR` **and**
     byte-swap each RGB565 pixel (`__builtin_bswap16`). Getting only one of the two
     looks like a *green/blue* swap; getting the other alone looks like *red/blue*.
     LVGL will emit byte-swapped pixels itself in Phase 2 (its 16-bit swap option),
     so we won't hand-swap there.
  3. **Upside-down mount** → `swap_xy(true)` + `mirror(true, true)` for 320×240
     landscape. Note: with `swap_xy` on, `mirror_x` controls the *vertical* display
     axis and `mirror_y` the *horizontal* one (they're transposed).
- **draw_bitmap is async** — it queues the SPI transfer and returns. Reusing/freeing
  the pixel buffer immediately corrupts the image. Register `on_color_trans_done`
  and block on a semaphore per draw (see `draw_wait()`); LVGL uses the same callback
  to signal flush-ready in Phase 2.
- **USB-Serial-JTAG wedges if you toggle RTS/DTR** on the port (my serial reader
  did). Symptom: esptool "No serial data received", no reset mode recovers it. Fix:
  unplug/replug the cable (or BOOT-hold). Read the port *passively* — never touch the
  control lines. Also: a second board (plain ESP32) was on `usbserial-0001`; esptool's
  chip-mismatch check stopped a mis-flash — always pass `-p /dev/cu.usbmodem2101`.
