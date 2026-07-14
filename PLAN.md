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
- [x] **Audio framework**: **libhelix-aac + hand-written HLS** (revised at Phase 12).
      Chosen over esp-adf to avoid the ~250 MB SDK and its IDF-version coupling, keep
      a clean BSD license, reuse our httpc/audio components, and actually own the
      pipeline (the project's whole point). **Fallback:** if libhelix hits an
      unfixable wall, switch this phase to esp-adf.
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

**Decision (Phase 12)**: **libhelix-aac + hand-written HLS**. Owns the pipeline,
BSD-licensed, no 250 MB SDK, no IDF-version gamble, reuses `httpc`/`audio`. Falls
back to esp-adf only if libhelix proves unworkable.

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
| 12 | **HLS pipeline (libhelix-aac)** | Hand-written audio pipeline: m3u8 parse, chunk fetch, AAC decode, ring buffer | `components/hls/` + libhelix — playable! |
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
- [x] **Phase 2** — LVGL via managed component ✅ LVGL v9.3.0 rendering on-device
- [x] **Phase 3** — Touch driver ✅ FT6336 -> LVGL, all corners mapped, shared I2C bus up
- [x] **Phase 4** — Player UI port ✅ player + station list, JP font, touch nav (stub data)
- [x] **Phase 5** — WiFi state machine ✅ connects w/ retry+backoff, status in UI
- [x] **Phase 6** — WiFi setup (on-device: scan + keyboard, NVS-persisted) ✅
- [x] **Phase 7** — NVS settings (versioned blob, defaults, corruption recovery) ✅
- [x] **Phase 8** — NTP time sync (multi-server SNTP, JST, live clock) ✅
- [x] **Phase 9** — HTTPS + esp-tls (cert-bundle validation, httpc helper) ✅
- [x] **Phase 10** — Radiko auth1/auth2 (partial-key derivation, area=JP14) ✅
- [x] **Phase 11** — Audio output: I2S + ES8311 (test tone audible) ✅
- [x] **Phase 12** — HLS pipeline (libhelix-aac + hand-written HLS) ✅ Radiko audio plays!
- [x] **Phase 13** — Station logos as embedded data ✅ real logos, RGB565 via EMBED_FILES
- [x] **Phase 14** — Program info fetch ✅ + Arduino UI parity, RGB LED, sleep timer
- [x] **Phase 15** — Settings page port ✅ full Arduino parity: dim/off, flip 180°, battery gauge, LED persist
- [x] **Phase 16** — Screen saver port ✅ bouncing rainbow clock, saver-mode dim/off
- [x] **Phase 17** — Error handling pass ✅ re-auth/backoff/degrade + fixed SPI-DMA freeze & LVGL transform wedge
- [x] **Phase 18** — Watchdog tuning ✅ TWDT 15 s panic-on-starve, critical tasks subscribed (app_watchdog)
- [x] **Phase 19** — Crash dump partition ✅ crashlog boot summary + Settings line, flow proven with forced crash
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

### Phase 2 — LVGL (v9 via managed component)

- **`lvgl/lvgl` pinned `~9.3.0`** (resolved 9.3.0), configured via Kconfig
  (`CONFIG_LV_COLOR_DEPTH_16=y`) — no hand-written `lv_conf.h`. Adds ~230 KB to the
  app (now ~475 KB, 85 % of slot free).
- **Manual esp_lcd <-> LVGL glue (components/ui):**
  - Flush: `flush_cb` calls `esp_lcd_panel_draw_bitmap`; **flush-ready is signalled
    from the panel's `on_color_trans_done` ISR callback** (`lv_display_flush_ready`),
    reusing the Phase-1 hook via `display_register_flush_ready_cb()`.
  - **Byte-swap belongs in the flush**, not the pixel data: LVGL emits native
    little-endian RGB565, this panel wants big-endian, so `lv_draw_sw_rgb565_swap()`
    in `flush_cb` (paired with BGR order in the panel). Confirms the Phase-1 finding.
  - Tick via `lv_tick_set_cb()` returning `esp_timer_get_time()/1000` — no periodic timer.
  - LVGL is single-threaded: one task on core 1 runs `lv_timer_handler()`; all lv_*
    calls guard on a **recursive mutex** (`ui_lock`/`ui_unlock`). Core 0 stays free
    for network/audio later.
  - Two partial draw buffers (40 lines each) in internal DMA RAM.
- **Built-in Montserrat font is ASCII + a few symbols only** — an em-dash (U+2014)
  rendered as a tofu box, and CJK has no glyphs at all. The Japanese station names
  in Phase 4 need a custom subset font (like the Arduino's `lv_font_jp`).
- LVGL v9 API names (vs v8): `lv_screen_active`, `lv_button_create`,
  `lv_display_*`, `lv_display_set_buffers(..., size_in_bytes, RENDER_MODE_PARTIAL)`.

### Phase 3 — Touch driver (FT6336G via esp_lcd_touch)

- **Shared I2C bus is its own component (`i2c_bus`, SDA=16/SCL=15).** `i2c_master`
  is thread-safe, so touch (this phase) and the ES8311 codec (Phase 11) share the
  one bus from different tasks. `i2c_bus_init()` is idempotent.
- **Drivers:** `espressif/esp_lcd_touch_ft5x06` (^1.0.6 -> 1.1.0) covers the
  FocalTech FT6336; pulls in `esp_lcd_touch`. Panel IO via
  `esp_lcd_new_panel_io_i2c` on the i2c_master bus — **must set
  `io_cfg.scl_speed_hz`** (required by the v2/bus-handle path; the FT5x06 config
  macro doesn't).
- **`esp_lcd_touch`'s `mirror_x`/`mirror_y` are broken for this geometry.** They
  mirror on the *raw* axis (0..240) using `x_max` (=320) *before* the swap, which
  offsets by 80 and kills the top edge (symptom: top unresponsive, y stuck ~85).
  Fix: driver does **swap_xy only**; flip X manually in the read callback
  (`x = (DISPLAY_H_RES-1) - x`). Verified against all four corners.
- **Touch mapping needs live on-device tuning, not reasoning.** A click-only
  readout is unreliable near edges (edge touches read as drags, not clicks) — use
  a live `LV_EVENT_PRESSING` readout and read raw corners with mirroring *off*
  first, then derive the transform.
- Register the `lv_indev` under `ui_lock()` since the LVGL task is already running.
  The read callback polls I2C from the LVGL task — fine at 400 kHz for a few bytes.
- Minor: `esp_lcd_touch_get_coordinates` is deprecated (→ `_get_data` in 2.0) but
  still works; left as-is.

### Phase 4 — Player UI port (stub data)

- **v8 LVGL font `.c` files do NOT compile on v9** (the `lv_font_t.get_glyph_bitmap`
  callback signature changed). Regenerate with `lv_font_conv` (Homebrew). The
  installed version emits descriptors that happen to match v9.3
  (`lv_font_get_bitmap_fmt_txt` exists with the v9 signature), so its output builds
  clean. Source font: Noto Sans JP (`~/Library/Fonts`). **Subset = only the glyphs
  the station names use** (ASCII + katakana 0x30A2-0x30FC + a dozen kanji) → ~3.5 K
  lines, tiny. Full-range JP (arbitrary program titles) waits for real data.
- **`STATION_COUNT` is a runtime `const int`, not usable for array sizes** — added a
  `#define NUM_STATIONS 15` for compile-time sizing; loops use the runtime value.
- **Logos are colour tiles + abbreviation for now** (real embedded logos = Phase 13).
- Ported the player screen + station list only. Settings and screen-saver are their
  own later phases (15, 16); WiFi setup rides with provisioning (6).
- Components split by concern: `fonts` (JP glyphs), `stations` (data table), `ui`
  (screens). Player state (current station / volume / playing) is local — no audio
  or network yet, exactly the Tier-A end state.
- **End of Tier A:** device boots, shows the player UI, Japanese renders, touch
  navigates player <-> list, buttons/slider respond. No audio, no network.

## Tier B lessons

### Phase 5 — WiFi state machine (esp_wifi + esp_event)

- **`esp_wifi` requires NVS initialised first** — `nvs_flash_init()` (with the
  no-free-pages/new-version erase-and-retry dance) before `wifi_start()`.
- **Event-driven, no blocking loops** (unlike the Arduino `WiFi.begin()` wait):
  handlers on `WIFI_EVENT`/`IP_EVENT`. STA_START → connect; STA_DISCONNECTED →
  backoff + reconnect; GOT_IP → connected, reset retry.
- **Backoff is a non-blocking `esp_timer` one-shot** (500 ms << retry, capped 30 s),
  so nothing spins. Reset the counter on GOT_IP.
- **Always log the disconnect reason** (`wifi_event_sta_disconnected_t.reason`):
  201=AP-not-found, 15=bad-password saved a lot of guessing. Note: **ESP32-S3 is
  2.4 GHz only** — a 5 GHz-only AP shows up as reason 201, not an auth error.
- **Credentials stay out of git:** gitignored `wifi_secrets.h` pulled in via
  `#if __has_include`, falling back to empty strings so CI still builds without it
  (a committed `wifi_secrets.h.example` shows the shape). Component needs
  `PRIV_INCLUDE_DIRS "."` so the header next to `wifi.c` is found.
- **UI reflects WiFi by polling in an `lv_timer`** (runs inside `lv_timer_handler`,
  already under the LVGL lock) — `wifi_get_*` are just thread-safe reads. No
  cross-task LVGL calls needed.
- The S3 **USB-Serial-JTAG intermittently refuses download mode** ("No serial data
  received") right after some app resets; a flash retry loop (2-4 tries) clears it
  without a physical replug.

### Phase 12 — HLS pipeline (libhelix-aac, hand-written) — Radiko plays!

- **Radiko's streaming flow changed since the Arduino era.** Current chain:
  stream-info XML (`v3/station/stream/pc_html5/{id}.xml`) → `playlist_create_url`
  → `playlist.m3u8?station_id&l=15&lsid=<rand32hex>&type=b` (+ auth token header)
  → master playlist → `medialist?session=…` → media playlist → `.aac` segments.
  Codec is **HE-AAC (`mp4a.40.5`)** → SBR must be enabled in the libhelix shim.
- **Two-stage pipeline is mandatory.** The CDN serves live segments at ~1x the
  stream bitrate (a 5 s / 31 KB segment takes ~5 s to fetch). A single
  fetch→decode→play loop is sub-real-time and starves. Split into a **fetcher
  task** (queue of segments) + **decoder task** (drain → libhelix → audio) so
  network latency overlaps playback. Plus a 15 s PCM ring buffer + I2S writer task.
- **Internal RAM is the tight resource, and TLS needs a contiguous ~40 KB.** With
  two task stacks it dropped to ~51 KB and `mbedtls_ssl_setup` returned
  `-0x7F00` (ALLOC_FAILED). Fixes: route the Helix decoder state to PSRAM
  (`helix_malloc` → `heap_caps_malloc(SPIRAM)`) **and** move the LVGL draw buffers
  to PSRAM — freed internal RAM to ~99 KB, TLS connects.
- **Keep-alive + a 4 KB HTTP read buffer** cut per-segment fetch overhead.
- Track the last played segment by URL (unique timestamps) so live-playlist
  refreshes and session re-resolves never replay or skip.
- **End of the hard part:** device authenticates and plays live Radiko audio
  through a pipeline we wrote and own — no esp-adf, no 250 MB SDK.

### Phase 13 — Station logos as embedded data

- **Embed pixels with `EMBED_FILES`, not a giant C array.** Raw RGB565 `.bin`
  files at the component root → CMake `EMBED_FILES` → symbols
  `_binary_<name>_bin_start`. A thin generated `logos_gen.c` wraps each blob in a
  v9 `lv_image_dsc_t` (`.magic = LV_IMAGE_HEADER_MAGIC`, `.cf =
  LV_COLOR_FORMAT_RGB565`, `w/h/stride`). Keeps flash use explicit and the source
  tree tiny. `.bin` files MUST sit at the component root (not a subdir) for the
  symbol names to match.
- **Pre-size assets so LVGL renders at exactly 1x.** A non-1x `lv_image_set_scale`
  forces LVGL v9's per-frame transform/blit path. Rendering 15 scaled logos in the
  station list ran that path continuously on the LVGL task (prio 4, core 1), which
  sits *above* the audio decoder (prio 3, core 1) → decoder starved → audio died.
  Fix: fit each logo (aspect-preserved) into ≤108x42 so both the list box (110x44)
  and player box (148x58) resolve to 1x → a plain blit, no transform.
- **Debugging a hard hang with no serial output.** Three separate bugs, surfaced in
  order by fixing each:
  1. *Hard wedge, no panic, no coredump, USB-JTAG dead* while mashing prev/next.
     Cause: touch is **polled**, but `esp_lcd_touch` was still given `int_gpio_num`,
     so it installed a **non-IRAM GPIO ISR**. A touch edge firing *during* a
     per-press NVS flash write (cache disabled on both cores) tried to run the ISR
     from uncached flash → unrecoverable. Fix: `int_gpio_num = GPIO_NUM_NC`.
  2. *Flash/stream churn.* Every prev/next did an NVS write **and** a full stream
     stop+restart. Fix: **debounce** — update the UI instantly, commit NVS + switch
     the stream once, ~450 ms after the user settles (an `lv_timer` reset on each
     press). Better UX and far less flash wear.
  3. *Task-watchdog on IDLE1, `lvgl` stuck in `wait_for_flushing`.* LVGL's default
     flush wait is a busy `while(disp->flushing)` spin that never yields; if a SPI
     flush completion stalls under load, IDLE1 starves. Fix: a **yielding**
     `flush_wait_cb` that blocks on the DMA-done semaphore, with a 100 ms timeout
     that force-clears the flag (drops the frame) so a lost completion self-heals
     instead of wedging.
- **Diagnostics worth keeping (pulled forward from Tier D):** hardware
  stack-overflow watchpoint (`CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK`) and
  coredump-to-flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, ELF). The watchpoint
  ruled out a stack overflow; once the hard wedge became a catchable watchdog, the
  ELF coredump + `xtensa-esp32s3-elf-addr2line` gave the exact stuck backtrace.
  (`CONFIG_COMPILER_STACK_CHECK_MODE_NONE` means a plain overflow silently corrupts
  and wedges — enable the watchpoint before trusting "no crash = no overflow".)

### Phase 14 — Program info + audio-stability marathon

The feature (now-on-air titles) was a day; making audio *stay* playing was the
real phase. Eight distinct bugs, each only visible after fixing the previous:

- **`/v3/program/now/{area}.xml` is always gzip'd** (even without Accept-Encoding)
  and covers every station in one ~90 KB XML. Fetched over **plain HTTP** on
  purpose — public data, and it avoids a second TLS session competing for RAM.
- **Decompress with zlib's `puff`** (vendored, ~2 KB stack, zero heap — it uses
  the output buffer as the DEFLATE window). The ROM `tinfl` needs an ~11 KB
  internal-RAM decompressor that either starved the stream's TLS (as a task
  stack) or crashed (as a PSRAM object). puff ended that whole class of problem.
- **Radiko's medialist session expires after a few minutes** and then serves a
  frozen playlist **with HTTP 200** — fetches never fail, audio just silently
  drains. Detect "N consecutive polls with no new segment" → force a fresh
  session. Also: on any session re-resolve, **clear `last_seg`** — the new live
  window has different segment names, so keeping the old marker skips every
  segment forever ("plays 5 s then stops").
- **Never `goto done` on a transient network failure.** The fetcher's stream-info
  resolve now retries with capped exponential backoff; auth retries too. A boot
  hiccup must degrade into a delay, not silence.
- **TLS vs internal RAM, the final round:** mbedtls needs its 16 KB IN record
  buffer *contiguous internal* — and whether a ≥16 KB block survives boot-time
  churn varied run to run. Fixes in order of discovery: asymmetric content len
  (OUT 4 KB), create long-lived task stacks at boot (allocation order is part of
  the memory design!), and finally **`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`** —
  all mbedtls buffers in PSRAM. That ends the fragility structurally; the TLS
  slowdown is irrelevant at 160 kbps.
- **Core split: decoder moved to core 0** (with the I/O-bound fetcher), LVGL owns
  core 1. The full-CJK font made rendering heavy enough that whichever task lost
  the core-1 fight starved — stuttering audio AND laggy UI. CPU-bound work also
  needs periodic yields (`vTaskDelay(1)` every ~32 frames) or a burst starves the
  idle task → task watchdog.
- **LVGL flush done right:** `flush_cb` writes the bitmap, then blocks (yielding)
  on the DMA-done semaphore, then `lv_display_flush_ready` — one take per flush,
  one give per completion. Both earlier attempts (default busy-spin; separate
  `flush_wait_cb` with timeout) starved the idle task or dropped frames.
- **Full-CJK font** (JIS-ish ranges + symbol blocks, bpp2, ~1.05 MB) replaces the
  station-name subset — program titles use arbitrary kanji and 記号 like ♪.
  LVGL v9 gotchas: `LONG_DOT` needs a *fixed-height* box or it wraps; image
  scale is visual-only (align by the computed visual box, pivot 0,0); a widget
  smaller than its source **crops before transforming**.
- **Arduino UI parity pass:** full-width swipeable logo strip (swipe=prev/next,
  centre tap=list), white logo card, slow-crawl marquees (player + list rows),
  list rows = logo left / name top-right / programme bottom-right, WS2812 mood
  LED component (GPIO42, 7 modes, prio-1 tick task), sleep-timer button.
- **Boot→audio 50 s → 18 s:** stream-info over HTTP (−1 TLS handshake), test tone
  removed, and auth1+auth2 share one keep-alive connection (−1 handshake, ~6 s).
- **Periodic stop/resume choppiness = session-expiry gap vs buffer depth.** The
  dropout window (stale detection + re-resolve) must stay well under the buffered
  audio. Fixes: request `l=30` (30 s live backlog instead of 15), detect a stale
  playlist after ~5 s of empty polls (was 12), and a 30 s PCM ring buffer.
  Verified 7 min continuous with zero stalls.

### Phase 15 — Settings screen + two concurrency bugs

- Settings/Info screen (tap the centre title): Brightness, Screen Dim (1–15 min),
  Screen Off (3 min–Never), and Sleep Timer sliders; Flip Screen 180° and Screen
  Saver switches; System Info (incl. battery mV/%); firmware info. All persisted
  via the settings blob. The saver's bouncing-clock screen itself is Phase 16 —
  until then saver-on just keeps the display awake and greys out Dim/Off (the
  saver replaces those timeouts, Arduino behaviour).
- **LVGL's inactivity clock gives idle machinery for free.** A 300 ms
  `lv_timer` polling `lv_display_get_inactive_time()` drives on→dim→off; a
  *decrease* in idle time between ticks is the wake signal (any input resets the
  clock), so the touch driver needs no hooks. Tick fast: wake latency is one
  period, and 1 s of black screen after a touch feels broken.
- **Flip 180° = invert both panel mirrors AND both touch axes.** The flipped
  flag lives in the display component (`display_flipped()`), which touch already
  depends on — putting it in the UI would create a ui↔touch dependency cycle
  (touch needs ui_lock for indev registration).
- **No charge-status pin (TP4054 CHRG only drives an LED)** — charging is
  inferred from the voltage trend: >10 mV rise over a ~30 s window of smoothed
  readings, sampled on the status bar's steady 2 s tick.
- `esp_app_desc` build date is stamped when *esp_app_desc.c* recompiles, not on
  every build — a stale-looking banner date does not mean a stale flash. Verify
  flashes by a log line unique to the new build instead.
- **Every ISR must be IRAM-safe if ANY task writes flash.** The WS2812's RMT
  interrupt (20 ms refresh tick) wasn't; any NVS write (volume release, station
  select, settings sliders) disables the flash cache, and an RMT interrupt in
  that window wedged the chip. `CONFIG_RMT_ISR_IRAM_SAFE=y`. Same failure class
  as the Phase 13 touch-INT bug — audit this for every new interrupt source.
- **ESP-IDF's `i2c_master` is NOT thread-safe across tasks.** Touch polls (LVGL
  task) racing ES8311 mute/volume writes (stream ctrl task) on the shared bus
  corrupted the driver and hung both: touch dead + stream never restarted.
  All transactions on a shared bus must hold a mutex (`i2c_bus_lock()`).
  Repro was "press Next, then drag the volume mid-switch".

### Phase 16 — Screen saver (bouncing clock)

- The whole feature is one extra LVGL screen plus a 60 ms `lv_timer` that stays
  paused unless the saver is showing — zero cost the rest of the time. Enter
  only from the player screen (Arduino rule); exit is a single idempotent
  `saver_exit()` reached from both the tap handler and the idle timer's wake
  path, so a missed click event can't strand the saver.
- Saver-mode timeouts invert the normal machinery: dim timeout *shows* the
  clock at full brightness, off timeout *dims* it (~7%), and the panel never
  fully blanks. The normal dim→off path stays untouched behind the mode check.
- LVGL ships `lv_color_hsv_to_rgb(h, s, v)` (h 0-359, s/v 0-100) — no need to
  port the Arduino's hand-rolled HSV for the bounce colour cycling.
- Extra Montserrat sizes are Kconfig options (`CONFIG_LV_FONT_MONTSERRAT_48`),
  and any `sdkconfig.defaults` change needs `rm -f sdkconfig` + rebuild to
  actually apply — a silently stale sdkconfig looks exactly like "my change
  did nothing".

### Phase 17 — Error handling pass + two root-caused display failures

Recovery work (replace every dead-end error path with retry/degrade):

- **Radiko can invalidate the auth token at any time; every retry loop that
  keeps the dead token is an infinite loop.** The fetcher now counts 401/403s
  (fetch() returns -status) and after two in a row re-authenticates in place,
  then resyncs to the live edge. Any success resets the counter.
- All HLS retry paths share one capped exponential backoff (1 s → 10 s); the
  master-playlist path used to retry at a fixed 1 Hz forever.
- Program-info failures retry in 30 s, not the full 5 min refresh interval.
- Peripheral init failures (I2C bus, touch, codec) log and degrade instead of
  `ESP_ERROR_CHECK`-aborting into a boot loop — a silent or touchless radio
  that still boots (and can be flashed/OTA'd) beats a brick. Display/LVGL
  still abort: with no screen there is no product.
- `settings_save()` failures are logged; `audio_write()` guards a NULL ring
  buffer; boot auth backs off 3 s → 30 s.

Root cause #1 — **screen froze when internal RAM fragmented (SPI DMA bounce).**
LVGL draw buffers lived in PSRAM since Phase 2. GPSPI on the S3 cannot DMA
from PSRAM, and IDF's spi_master hides that by allocating a MALLOC_CAP_DMA
bounce buffer and memcpy'ing EVERY transaction (spi_master.c `setup_priv_desc`).
So every flush silently cost a 25 KB internal alloc + copy, and when station-
switch TLS churn dropped the largest internal free block under the chunk size
(heartbeats showed "largest 15 KB"), the alloc failed → `spi transmit (queue)
color failed` → frozen screen while audio played on. The ili9341 driver
swallows tx_color errors (draw_bitmap returned ESP_OK), which hid the cause.
Fix: draw buffers now live in internal DMA RAM (2×20 lines, affordable since
mbedtls moved to PSRAM) — zero-copy DMA, no allocation in the flush path.
Lesson: **know which memory your DMA can actually reach; "it works" may be a
hidden per-transfer bounce that fails only under fragmentation.**

Root cause #2 — **LVGL task wedged inside `lv_draw_sw_img` transform.** With
the smaller draw buffers, torture-testing hung the LVGL task permanently in
`transform_and_recolor` (task watchdog on IDLE1 every 5 s, identical
backtrace for minutes; `lv_timer_handler` never returned). The only runtime-
scaled images were the 15 station-list logos. Rather than debug LVGL's
software transform, we removed the dependency: a second pre-scaled asset set
(fit 138×38, ~118 KB) generated offline, so every image in the UI blits 1:1
and the transform path is unreachable. Second strike for runtime transforms
(Phase 13: starved the decoder; here: wedged the UI) → **rule: never scale
images at runtime on this target; pre-scale at asset-generation time.**
- Debug flow that cracked both: instrument the exact failure boundary (flush
  timeout counter + draw error code), long passive serial capture while the
  user reproduces, then read the driver source at the failing line instead of
  guessing. `task_wdt` backtraces + addr2line pinpointed the wedge.

### Phase 18 — Watchdog tuning (app_watchdog)

- **Default TWDT policy is "print and carry on" — useless for a headless
  appliance.** The Phase 17 wedge logged a warning every 5 s to a serial port
  nobody watches while the radio sat frozen. Now `CONFIG_ESP_TASK_WDT_PANIC=y`:
  starvation → panic → coredump to flash (already enabled) → reboot into a
  working radio, with the evidence preserved for Phase 19 to read back.
- **The timeout is set by the worst-case *legitimate* silence, not by desired
  detection speed.** The fetcher can sit 10 s in one esp_http_client timeout
  with no chance to feed; 15 s clears that with margin. A wedge that lasted
  14 s was going to last forever — fast detection buys nothing.
- **Who gets watched = whose silent death bricks the product**: lvgl (frozen
  UI+touch), i2s_wr (silent audio), seg_fetch/seg_dec (dead stream). Placement
  of feeds is part of the design: every retry path feeds before its backoff
  sleep, the queue-full pacing loop feeds every 200 ms, and per-play tasks
  MUST `esp_task_wdt_delete` before `vTaskDelete` — a watched dead handle
  trips the watchdog for everyone. Verified across station switches
  (subscribe/unsubscribe cycles clean).
- Tasks that block indefinitely BY DESIGN (stream_ctl on its command queue,
  the LED tick) are not subscribed — a watchdog on a legitimately-idle task
  just forces fake heartbeat churn.
- **LED smoothness, a scheduling case study.** The mood LED ran "one effect
  step per 20 ms tick" at priority 1 on the decoder's core: decode bursts
  starved it, so the animations ran SLOW (lost ticks) and CHOPPY (bursty
  wake-ups). Two orthogonal fixes: (1) derive the animation phase from the
  wall clock, never from counted invocations — a starved task then shows the
  right colour late instead of falling behind; (2) priority reflects LATENCY
  NEED, not importance: the tick is ~30 µs of work every 30 ms (~0.1% CPU),
  so priority 6 above the whole decode pipeline costs the audio nothing and
  makes the cadence exact. Also matched the Arduino's 30 ms tick so every
  effect runs at the reference speed.

### Phase 19 — Crash dump recovery flow (crashlog)

- The partition + coredump config existed since Phase 13; the phase's real
  content is the RECOVERY FLOW: `esp_core_dump_image_check()` +
  `esp_core_dump_get_summary()` at boot decode the stored dump with no host
  attached — task name, PC, addr2line-ready backtrace into the log, a
  `Last crash:` line into Settings. Field failures now self-report.
- **Test the disaster path by causing the disaster.** A deliberate NULL-store
  proved the whole loop: panic → ELF dump to flash (~29 KB, CRC-checked) →
  reboot → on-device summary naming the exact task and PC →
  `idf.py coredump-info` resolving to the exact source line. Infrastructure
  you've never fired is a hope, not a capability.
- Gotchas: the panic handler runs the dump on a RESERVED stack (~1.9 KB used —
  don't fatten it); `coredump-info` symbolizes against the CURRENT build's
  ELF, so read dumps out before reflashing new code; an ERASED partition
  after a reboot means the panic handler never ran at all — that's the hard-
  wedge signature (see Phase 13/15 IRAM-ISR lessons).
