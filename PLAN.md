# ESP32 Radiko Player Pro — Plan

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
- [x] **License**: MIT (`LICENSE`, repo root). Third-party components keep their own
      licenses (e.g. the ES8311 driver is Apache-2.0).
- [x] **Commit cadence**: honest "every step" history — commit frequently within a
      phase, warts and all.
- [x] **Audio framework**: **libhelix-aac + hand-written HLS** (revised at Phase 12).
      Chosen over esp-adf to avoid the ~250 MB SDK and its IDF-version coupling, keep
      a clean BSD license, reuse our httpc/audio components, and actually own the
      pipeline (the project's whole point). **Fallback:** if libhelix hits an
      unfixable wall, switch this phase to esp-adf.
- [x] **Secure boot timing**: final phase (Phase 25) — kept debugging easy the
      whole way; Stage A (signed OTA) is reversible and left JTAG/USB flashing
      intact, and the irreversible Stage B burn was consciously not run.

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
| 25 | **Secure boot v2 + flash encryption** | Efuse one-shot, key management, locked production firmware | Stage A: signed OTA + runbook (reversible). Stage B: the burn (irreversible, separate consent) |

### Tier E — Polish (optional)

| # | Phase | Concern |
|---|-------|---------|
| 26 | Localization framework | Japanese UI strings via lookup table |
| 27 | Better volume curve, equalizer | Audio quality |
| 28 | Bluetooth output | A2DP source |
| 29 | **Recording to SD + playback** | 29a ✅ live record + on-device playback; 29b time-free deferred |
| 30 | **VPN-free geo-auth + area picker** ✅ | Android-app auth so any area streams from any IP; Settings area selector (built) |
| 31 | **LVGL heap exhaustion fix** ✅ | Opening WiFi setup rebooted the radio: LVGL's fixed 64 KB pool hit 100%. +256 KB PSRAM spill pool |
| 32 | **Audio visualiser** | Rainbow spectrum bars in the logo tile; long-press to toggle. FFT of the PCM actually being played |

#### Phase 29 design — record to SD (studied, not yet built)

**Hardware verdict: CONFIRMED ON-DEVICE.** A throwaway SDMMC-4-bit self-test
mounted, wrote, read and byte-verified a card first try — pins from the BSP were
correct. Card: SDHC 15 GB. **Write 1.03 MB/s, read 0.99 MB/s, worst single-block
write 113 ms** (32 KB block, fsync each). Recording needs ~15-70 KB/s, so
throughput is 15-70× the requirement and 15 GB holds ~500-700 h. The **113 ms
worst-case stall is the design driver**: writes must run on a dedicated task+queue
so a FAT/flush stall never reaches the decoder on core 0.

The board's micro-SD slot is **SDMMC 4-bit on dedicated pins** — CLK=IO38,
CMD=IO40, D0-D3=IO39/41/48/47 — and does **not** share the LCD's SPI bus. Cross-checked against
every pin we use (LCD 10-13/45-46, I²C 15-16, touch 17-18, I²S 4/5/7/8+1, WS2812
42, batt 9): **zero conflicts**. So card writes can't stall the display or audio
DMA. Mount with `esp_vfs_fat_sdmmc_mount` + FATFS; 4-bit needs the D-lines pulled
up (the wired slot should provide this; fall back to 1-bit if a card won't init).

**Segment format (verified on-device, not assumed).** A probe in the fetcher
logged a real live segment: URL ends `.aac`, bytes are an **ID3v2 tag (73 B) then
`FF F9`** — the ADTS AAC syncword (MPEG-4, no CRC). So each segment is a standard
ID3+ADTS elementary stream. **Recording = strip the ID3 tag (syncsafe size at
bytes 6-9, +10) and append the ADTS payload** to a `.aac` file. No decode, no
re-encode, ~near-zero CPU. Concatenated ADTS is a universally playable file.

**Where to tap: the fetcher already has the bytes.** In `stream.c` the fetcher
downloads each ~4 s segment into a PSRAM buffer (`seg_t{buf,len}`) before queuing
it for the decoder. The recorder forks a copy there. Size: ~48 kbps..160 kbps
content → roughly **20-70 MB/hour** — trivial for SD.

**Protect the audio (same discipline as the visualiser).** SD writes can block
tens of ms (FAT allocation, cheap-card hiccups), and the fetcher shares core 0
with the decoder. So writing goes on a **dedicated recorder task fed by a queue**:
the fetcher hands off a buffer and moves on; an SD stall costs a queued segment,
never an audio glitch. The 30 s PCM buffer covers any fetcher pause anyway.

**Phased build (user chose "29a + on-device playback"; time-free deferred):**
- **29a — live recording.** Tap the fetcher, ID3-strip, write `<station>_<JST
  timestamp>.aac` via the recorder task. Trigger UI decided later (manual button
  vs scheduled) — the capture/storage mechanism is the same either way, so build
  that first and prove it end-to-end (record, eject, play the file on a PC).
- **29a′ — on-device playback (chosen scope).** A recordings browser + a player
  that reads a `.aac` off SD and feeds the EXISTING pipeline: same libhelix
  decoder, same PCM ring, same I²S/ES8311 output. It's a second *source* into the
  audio stack we already have, not a second stack — swap the network fetcher for a
  file reader that pushes ADTS frames into the decoder queue. Makes the radio
  self-contained (record and replay without a PC) and is the foundation 29b needs.
- **29b — Radiko time-free (タイムフリー). DEFERRED.** Fetch programmes that already
  aired in the past 7 days: separate API (`/v2/api/ts/playlist.m3u8?station_id=…&
  ft=…&to=…`) with a time-free auth token + a programme-guide picker. Reuses 29a's
  ID3-strip + recorder task AND 29a′'s file player. Not in the current scope; the
  user chose "29a + on-device playback" — build time-free later if wanted.

**Open items to settle at build time:** SDMMC 4-bit bring-up (a card that won't
pull up D3 falls back to 1-bit — verify first); card hot-plug / mount-on-boot vs
on-demand; filesystem-full handling; filename metadata (station id + JST); the
record trigger UI (manual button vs scheduled — deferred); and playback controls
(list, play/pause, seek within a recording).

#### Phase 29a — built (record + on-device playback)

Shipped end-to-end and verified on-device: record a live station to SD, browse
recordings, play one back through the speaker. New components: `storage` (SDMMC
4-bit mount), `recorder` (writer task + command queue). Playback reuses the
stream component's decoder — a file source feeding the SAME `s_q` the fetcher
uses, so there is ONE decode/audio path, not two.

- **Recording is a fork, not a copy of the pipeline.** The fetcher already holds
  each downloaded ID3+ADTS segment; `recorder_feed()` dups it to a writer task.
  Non-blocking (drops a segment on a full queue, never stalls the fetcher). ID3
  stripped, raw ADTS appended → a `.aac` that plays anywhere (confirmed on macOS)
  and feeds straight back into libhelix.
- **FAT 8.3 bit HARD.** `FMT_20260717_115841.aac` is 19 chars; the default
  `CONFIG_FATFS_LFN_NONE` silently makes `fopen(..,"w")` FAIL for any name past
  8.3. Enable `CONFIG_FATFS_LFN_HEAP`. Anyone doing dated filenames on ESP-IDF
  hits this. (And regenerate sdkconfig — the Phase 31 defaults-vs-generated trap.)
- **Playback = a second source into the one pipeline.** `stream_play_file()`
  stops the fetcher and starts a file-reader task that ADTS-frame-aligns its
  reads (never splits a frame across a `seg_t`, which the decoder would drop) and
  feeds `s_q`. The decoder/PCM-ring/I2S path is untouched. This is the payoff of
  keeping the audio pipeline hand-owned: adding a source was ~120 lines, no new
  decoder.
- **"File read" ≠ "audio finished."** A short recording (≤ a few segments) fits
  entirely in the 4-deep queue + the 30 s PCM ring, so the reader hits EOF in
  ~250 ms while ~30 s of audio is still buffered. The EOF callback fired ~30 s
  early and cleared "now playing" prematurely. Fix: after read-EOF, wait until
  `uxQueueMessagesWaiting(s_q)==0 && audio_ring_bytes()<4 KB` — i.e. the ring has
  actually drained — before signalling end. (`audio_ring_bytes()` already existed
  from the Phase 32 visualiser gate.)
- **Throughput/latency validated the design before code:** SD 1.03 MB/s write,
  113 ms worst-block stall → writes on their own task (recording needs ~15-70
  KB/s, so 15-70× margin). 15 GB ≈ 500-700 h.
- Trigger UI is a 6th button in the transport row (a Settings-page toggle was
  rejected — recording must be one tap from the player). While recording, the
  title bar flashes "● REC m:ss" in red and the record glyph becomes a ■ stop
  square. An in-progress recording shows 0 KB until Stop flushes/closes it (fsync
  every 10 segments; fclose on Stop) — playing a not-yet-closed file is empty.

**The internal-RAM war (the real cost of Phase 29a).** Adding SD + a second
audio source to a board already at its internal-RAM limit was the whole battle;
the record/playback code was the easy part. In order, each fix bought RAM the
next thing then needed — a chain worth reading before touching this again:

- **Task stacks must be claimed while RAM is unfragmented.** Re-creating the
  16 KB fetcher / 20 KB decoder per stream started failing once SDMMC had split
  the largest free block below 20 KB (`xTaskCreate` → NULL, silent-dead radio).
  Fix: create both ONCE at boot, PERSISTENT, before `storage_init()`; gate them
  with a flag between sessions instead of deleting.
- **Keeping the SD mounted starves TLS.** The SDMMC/FATFS mount holds ~12 KB
  internal; left mounted it dropped free internal low enough that the TLS
  handshake failed forever (no audio). Fix: LAZY refcounted mount — the card is
  mounted only while recording or browsing, unmounted during normal streaming.
- **PSRAM cannot be a DMA source, twice over.** (1) SDMMC DMA straight from a
  PSRAM buffer is pathologically slow (~2.8 s / 31 KB, froze the UI) → the writer
  bounces through a static internal 4 KB buffer. (2) The mount call chain is deep
  and overflowed the 3 KB stream_ctl stack. Enlarging that stack permanently just
  starved TLS again — so run the mount on a TRANSIENT helper task (6 KB stack,
  deleted the instant the mount returns): zero lasting stack cost.
- **The decoder must bound itself to its buffer.** libhelix trusts the ADTS
  frame-length field with no internal bounds check, so a false 0xFFF sync near a
  buffer's end (its garbage length exceeds what's left) walks the decoder off the
  end of the allocation (`RefillBitstreamCache` OOB → LoadProhibited). Live
  streams rarely desync; a recorded file that
  desyncs once crashed `seg_dec` on playback. Fix: reject any frame whose
  declared `flen > bytes-left` and resync one byte on. Hardens live too.
- **THE root cause of every "no sound": hardware AES + PSRAM mbedtls buffers.**
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` puts TLS buffers in PSRAM, but the
  hardware AES accelerator uses DMA (internal-only), so every TLS record
  allocated an internal bounce for AES. Once SD/recording shrank the headroom
  that allocation intermittently failed (`esp-aes: Failed to allocate memory`),
  breaking the connection — and every reconnect failed identically, so the radio
  never recovered (silent at boot, or died minutes in when free dipped). Fix:
  `CONFIG_MBEDTLS_HARDWARE_AES=n` — software AES works directly on the PSRAM
  buffers, no internal DMA. Negligible CPU at stream bitrate; made streaming +
  SD finally coexist. Soak: 5 min continuous, 0 AES failures, free internal a
  steady 19 KB. **Lesson: HW-crypto + external-mem-alloc is a latent OOM on any
  RAM-tight design; prefer software crypto once internal RAM is contended.**

#### Phase 32 design — audio visualiser

**Where to tap the audio (the one decision that matters).** The PCM ring is
`SAMPLE_RATE * 4 * 30` — **30 seconds** deep. Tapping the decoder's output would
drive the bars from audio the user won't hear for half a minute: visibly wrong,
and it would look like a broken FFT rather than a wrong tap point. Tap inside
`i2s_writer_task`, on the `chunk[4096]` handed to `i2s_channel_write()`. There
the 30 s is already behind the samples; residual lead is the chunk (~21 ms) plus
the I2S DMA queue (~30 ms) ≈ **20–50 ms**, far below perception.

**Audio must never wait for the visualiser.** The writer takes the tap mutex with
timeout 0 and simply skips the copy if the UI happens to hold it — a missed 21 ms
window is invisible, a stalled I2S writer is a glitch. The scheduler already
helps: `i2s_wr` is priority 6 and `lvgl` priority 4, both pinned to core 1, so
audio preempts drawing by construction.

**Layout — nothing moves.** `w_logo_tile` is already a transparent 320×68
CLICKABLE gesture surface at y=28, with the logo card as its *child* and the
station name / programme line as *siblings below it*. So the visualiser swaps in
as a sibling of the logo card **inside the tile**: the station name stays put for
free, and swipe-to-change-station keeps working on the visualiser with zero
gesture changes.

**The long-press latch.** The tile's gestures are hand-rolled on PRESSED/RELEASED,
not CLICKED. LVGL fires `LONG_PRESSED` at ~400 ms *while still held*, and
`RELEASED` still fires on lift — so a long press would toggle the visualiser AND
(dx≈0, centre region) open the station list. One press, two actions. Fix: latch a
flag in the long-press handler and swallow the next RELEASED. Guard the long-press
on `|dx| < 8` so a slow swipe can't toggle.

**Rainbow: hue by band index, not amplitude.** Precompute
`lv_color_hsv_to_rgb(i * 360 / N, ...)` once per bar — bass red → treble violet,
fixed per bar, so the bars dance while the spectrum stays legible.
Amplitude-mapped hue makes the whole strip strobe as one and reads as noise.

**Sizing.** 512-point FFT @ 48 kHz = 93.75 Hz bins; 16 log-spaced bands from
~60 Hz to ~16 kHz (linear bins give bass one bar and cymbals hundreds). Bars are
plain `lv_obj` rects, not an `lv_canvas`: 16 objects cost ~4 KB versus 42.5 KB for
a 320×68 RGB565 canvas, and LVGL does the drawing. Fast attack / slow decay
smoothing (`level = max(new, level*0.85)`) or it flickers.

#### Phase 30 design — VPN-free Radiko via Android-app geo-auth

**Problem.** Today's `radiko_auth.c` uses the **PC/HTML5** auth path, where
auth2 derives the area purely from the **source IP**. So the board needs a
Japan VPN, and Radiko Premium (エリアフリー) does NOT work from a Hong Kong IP.

**Discovery (studied from jackyzy823/rajiko v3.2026.2 — the same extension the
owner runs on PC Chrome; maintained fork gynix/rajiko cross-checked).** Radiko's
**Android app** auth path additionally accepts **GPS coordinates**
(`X-Radiko-Location`) and trusts them *over* the IP. Sending Tokyo coordinates
makes a HK-IP device stream as if in Tokyo — no VPN, no proxy. This is exactly
what rajiko does; it runs in a PC browser but sends *Android-app* auth.

**The delta from our current PC flow:**

| Field | PC (current) | Android (Phase 30) |
|-------|--------------|--------------------|
| `X-Radiko-App` | `pc_html5` | `aSmartPhone7a` |
| `X-Radiko-App-Version` | `0.0.1` | e.g. `6.3.6` (rajiko rotates a small list) |
| Full auth key | `bcd151073c…` (40-char ASCII) | ~1350-byte binary, shipped base64 (`APP_KEY_MAP` in rajiko `modules/static.js`) |
| Partial key | `base64(ASCII[off:off+len])` | `base64(atob(fullkey_b64)[off:off+len])` — decode first |
| auth2 extra headers | — | `X-Radiko-Location: "<lat>,<lng>,gps"`, `X-Radiko-Connection: wifi` |
| `X-Radiko-Device` | `pc` | `<sdk>.<model>` (sdk per app version; model from a list) |
| `X-Radiko-User` | `dummy_user` | 32-char random hex |

GPS jitter (per rajiko `modules/util.js`, so each boot looks like a slightly
different real device): `coord += (esp_random()/(float)UINT32_MAX)/40.0f * (±1)`
(~±2.7 km). Area→coordinate table (47 prefectures) lives in rajiko
`modules/constants.js` — e.g. Tokyo `35.68944,139.69167`, Osaka `34.68639,135.52`.

**Area picker.** Radiko's area is whatever the coordinates say, so "choose your
area" == "choose which prefecture's coordinates to send" — a Settings list
(persisted in `settings_t`, like station/volume). This subsumes the old
"Phase 30 multi-area" idea: you'd browse *any* region's stations from HK.
Note our `STATIONS[]` is a fixed Kanto set today; a full area picker also needs
per-area station lists (fetch `radiko.jp/v3/station/list/<area>.xml`), so v1 can
ship "auth as Tokyo/Osaka/…" with the current station list and defer dynamic
station lists to a follow-up.

**Implementation outline.**
1. Branch `feature/android-geo-auth`.
2. Embed the Android full key (base64 const → `atob` at runtime, or pre-decode
   to a byte array). Pull the *exact current* key + version list from
   jackyzy823/rajiko master (matches the installed extension) — a stale
   key/version silently fails auth.
3. `radiko_auth.c`: Android headers, decode-then-slice partial key, add
   `X-Radiko-Location`/`X-Radiko-Connection`, random device/user, chosen-area
   coordinates + jitter.
4. `settings_t.area` (default JP13 Tokyo) + a Settings area picker.
5. Keep the PC-key path as a compile-time fallback; comment the maintenance
   dependency ("if auth 401s, refresh APP_KEY_MAP/version from rajiko").
6. **On-device test from the raw HK WiFi (VPN off):** auth2 must return
   `JP13,tokyo,…` and a station must stream. That is the acceptance test.

**Caveats.** (a) Maintenance: app-version/key rotate when Radiko updates the
app; failures are fixed by refreshing two constants. (b) Scope: same method the
owner already uses via rajiko, for personal listening to free-to-air radio.
(c) The full key is embedded firmware data, not a secret of ours — it's the
public app key, same as the PC key we already ship.

**Built + verified (VPN-free from a Hong Kong IP).** Delta from the outline:

- The v8 app key is ~123 KB (a JPEG-disguised blob, an anti-extraction move —
  the old 7a key was 16 KB) and the nationwide logo set is ~2.7 MB. Both are
  too big for the near-full app slot, so each lives in its OWN data partition
  (`radikokey`, `logos`), flashed via parttool and mmap'd read-only. Removing
  the old 15 embedded Kanto logos actually freed the app slot from 4% → 17%.
- Full multi-area (109 stations, all 47 prefectures) via an offline asset
  pipeline (`tools/gen_stationdb.py`): fetch every area's station list + logos,
  downscale to big/small RGB565 on white, pack + emit the C DB with a logo
  index. The `stations` component filters to the active area and hands the UI
  pre-scaled logos straight from the mmap'd partition. Player/list/dots rebuild
  on area change; `settings.area` (schema v2) persists it.
- **The "same font name" trap.** Our `lv_font_jp_16` ≠ the Arduino's: ours is
  Noto Sans JP (open) regenerated full-CJK; the Arduino's was Hiragino W3, a
  132 KB 13-glyph SUBSET. We MUST use full-CJK (any station/area name), which
  forces the size/bpp trade: 4 bpp full-CJK is >3 MB (won't fit the app slot,
  Noto or Hiragino) — settled on 3 bpp compressed (`LV_USE_FONT_COMPRESSED` +
  `LV_FONT_FMT_TXT_LARGE`). Hiragino is Apple-proprietary → never committed to
  the public repo. Title reverted to English/default font; area picker stays
  Japanese.
- LVGL widgets need explicit theming: the stock dropdown is light and clashed
  with the dark palette; a Noto JP font has no LVGL symbol glyphs (dropdown
  arrow → tofu; used U+25BC ▼ which our font carries). Screensaver-on now
  means NO dimming (moving clock handles burn-in), and "Screen Dim" stays live
  as the saver-appear delay while "Screen Off" greys out.
- **`LV_USE_FONT_COMPRESSED` + `LV_FONT_FMT_TXT_LARGE` corrupted the heap.**
  Enabled for the 3 bpp JP font; the radio then crashed intermittently with
  the classic corruption signature — panics in UNRELATED tasks (`led`, `lvgl`)
  with GARBAGE backtraces (an led-task trace containing `argb8888_image_blend`
  is impossible unless the stack was overwritten), watchdog reboots. These are
  GLOBAL flags — they change the glyph-decode/draw path for EVERY font (the
  fatal draw was even a built-in Montserrat label). The Tier D stack paid off:
  coredump + crashlog + elog together showed the multi-task/garbage-backtrace
  pattern that fingerprinted corruption vs a plain logic bug. Fix: reverted to
  the proven 2 bpp uncompressed font. **Lesson: to ship a smoother/bigger
  font, put it in its own partition and load it as a runtime binary font — do
  NOT flip the global compressed/large `lv_conf` flags.**

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
- [x] **Phase 20** — Logging to flash ring buffer ✅ elog partition + esp_log hook, host dump tool
- [x] **Phase 21** — Unit tests ✅ Unity host tests for the parsers (16 green), CI job runs them per push
- [x] **Phase 22** — OTA from GitHub releases ✅ v0.22.0→v0.22.1 updated over the air, rollback-armed
- [x] **Phase 23** — CI/CD: build + test + release ✅ tag → tested, CI-built GitHub release; v0.23.0 delivered OTA
- [x] **Phase 24** — JTAG debug session ✅ OpenOCD+GDB walkthrough live on the radio; IDE attach configured
- [x] **Phase 25** — Secure boot v2 + flash encryption — split by reversibility:
  - [x] **Stage A (reversible, no eFuses):** signing key + backup ritual; signed
        OTA enforcement as a *switchable build profile* (`sdkconfig.signed`
        overlay — default build stays exactly as today); CI signs every release
        (signed images boot fine on non-verifying radios, so one release stream
        serves both profiles); written Stage B burn runbook. ✅ CI-signed
        v0.25.0 verified: valid RSA block, digest matches the local key.
  - [—] **Stage B (irreversible eFuse burn):** secure boot v2 (+ optional flash
        encryption / release mode) — **deliberately NOT executed.** The runbook
        (`docs/secure-boot-runbook.md`) is complete and factory-ready. The burn
        is one-way (even dev-mode flash encryption blows fuses; the chip can
        never return to factory-fresh, key or no key) and defends a *physical*
        attacker outside this project's threat model, on a single in-use board
        with no sacrificial twin. Knowing when NOT to pull the trigger is the
        deliverable. Would be run only on a board bought to be burned.

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
  https://github.com/hugochf/esp32-radiko-player-pro — CI green in ~2m10s (ESP-IDF v5.3.5).
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

### Phase 20 — Persistent event log (elog)

- **The plan said "NVS ring buffer"; the right tool was a raw flash ring.**
  Our NVS partition is 20 KB shared with WiFi creds/settings, and NVS
  key-value semantics fit config, not append-only logs. A dedicated 64 KB
  raw partition (carved from the head of the unused storage area — nothing
  existing moved) gives plain-text records readable with `strings`, one
  sector erase per 4 KB of text, and zero interference with real NVS.
- `esp_log_set_vprintf()` intercepts EVERYTHING once — format privately
  (va_copy!) before forwarding, filter to W/E by prefix after stripping the
  ANSI colour code, and stage into RAM under a mutex. Flash writes happen in
  a low-prio task every 10 s so no logging call ever blocks on flash. When
  the staging buffer is full, DROP — a log path must never apply
  back-pressure to the code it observes.
- Head recovery = scan for the first 0xFF at boot; no index metadata to
  corrupt. Boot marker records reset reason + version, so the ring doubles
  as a reboot history (crash loops become visible after the fact).
- Verified end-to-end: three reboots' markers + captured warnings read back
  via `parttool.py read_partition` + `tools/elog_dump.py` (ring reassembled
  oldest-first). Survives reflashes — the partition is outside the app slots.

### Boot splash (polish, after Phase 20)

- **A splash should end on the event it covers, not on a timer.** First
  thought was "show it 10 s"; measured boot-to-first-audio is actually
  22–27 s and varies with WiFi/auth. The splash is dismissed by the first
  real PCM entering the pipeline (`audio_on_first_audio` one-shot), with a
  2.5 s minimum (fast boots must not blink it) and a 35 s failsafe (boot
  trouble must not trap the user — sized ABOVE the normal spread or the
  failsafe becomes the ordinary exit). Status line tracks the boot stages.
- Backlight now turns on from the flush hook when the first complete frame
  is on the glass (`lv_display_flush_is_last`) — kills the 2 s black screen
  at power-up with zero garbage-frame risk.
- App slot now 95% full (~156 KB free): the splash bitmap (71 KB) fit, but
  Phase 22 (OTA) should budget flash first — trim CJK font ranges or move
  big assets to the storage partition.

### Phase 21 — Unit tests (host-side Unity)

- **Testability drove a real refactor, and that's the point.** The parsers
  were `static` functions buried inside components that #include half of
  ESP-IDF. Extracting them into libc-only units (`hls_parse.c`,
  `radiko_parse.c`) made them compilable with plain `cc` on any machine —
  the same separation that makes code testable makes it portable and
  reviewable. Firmware call sites just renamed; behaviour re-verified
  on-device after the refactor.
- **The tests found a real bug immediately**: the backoff doubled 8000 →
  16000 before capping (`cur >= 10000 ? 10000 : cur * 2`) — present since
  Phase 14 in its original form. Writing the assertion "8000 → 10000" is
  what surfaced it.
- Test what the field will throw at it: gzip FNAME-flag headers, truncated
  bodies, wrong magic, an id that prefixes another (JOAK vs JOAK-FM), an
  empty `<pfm></pfm>`, and a real gzip fixture round-tripped through the
  vendored puff. The fixture bytes were generated once with Python and
  embedded — deterministic, no test-time dependencies.
- Unity (v2.6.1, MIT) is vendored as three files under test/host/unity —
  no submodule, no package manager, nothing to break in CI. The CI job is
  plain `make -C test/host` on ubuntu: seconds of feedback next to the
  minutes-long firmware build.
- On-target tests (HW-dependent components: audio path, display, touch)
  are deliberately out of scope here — they need a runner attached to the
  device and come with the JTAG work (Phase 24) if at all. Host tests
  cover the logic that actually regresses.

### Phase 22 — OTA from GitHub releases

- **The device stores a URL, not an account.** `/releases/latest` on a
  public repo is anonymous + read-only; TLS (cert bundle) proves it's
  GitHub. What TLS does NOT prove is that the firmware came from us —
  until secure boot (Phase 25) signs images, repo write access IS
  firmware access. Documented trust anchor, not an accident.
- **Two-phase API on purpose**: ota_check() is cheap and leaves playback
  alone; only a confirmed-newer release stops the stream for the ~3 MB
  download. Release JSON parsing + dotted-version compare are libc-pure
  (`ota_parse.c`) and host-tested like the other parsers.
- **Rollback = bootloader + watchdog working together.**
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: the new image boots
  PENDING_VERIFY; main marks it valid after 30 s alive. A crash/wedge in
  fresh firmware → Phase 18 panic → reboot → automatic downgrade. Verified
  by resetting after the update: v0.22.1 stayed (slot B, 0x320000).
- **First field failure was diagnosed BY the Tier D tooling**: the update
  died with a status line on screen; the elog ring had the exact chain
  (`HTTP_CLIENT: Out of buffer`). Root cause: GitHub redirects assets to
  objects.githubusercontent.com with a >1 KB signed URL — the redirected
  request must fit the http client's TX buffer (default 1 KB). Fix:
  `.buffer_size_tx = 4096`. Lesson: size BOTH http buffers when a service
  uses signed redirect URLs.
- Provenance gap flagged for Phase 23: this release's .bin was built on a
  laptop from momentarily-uncommitted state. CI should build releases
  from tags so artifact == commit, always.
- Also: montserrat has no em-dash — a "—" in a UI string renders as tofu.
  ASCII hyphens in UI text.

### Phase 23 — CI/CD release pipeline

- **The provenance bug bit before the pipeline could prevent it** — a
  perfect demonstration. The hand-built v0.22.1 release binary predated
  its own TX-buffer fix (built → released → bug found → fixed → flashed
  locally), so the device that OTA'd onto v0.22.1 got the UNFIXED code
  back and failed to update again. Hand-built artifacts drift from
  source; that is the whole disease. Cure: releases are built only by CI
  from the tagged commit (`release.yml`) — artifact == commit, always.
- Guard rails in the workflow: host tests must pass before the build;
  the tag must equal PROJECT_VER (a mismatch would poison the device's
  version comparison); sha256 published beside the .bin. Cryptographic
  signing is Phase 25's.
- **CI failures are the product working**: the first release run went red
  because `idf/build` is root-owned (the IDF action builds inside Docker
  as root) and the non-root checksum step couldn't write there. A class
  of bug that cannot exist on the dev machine — exactly what the fresh
  runner is for. Write derived files to the runner-owned workspace root.
- Full loop verified live: `git tag v0.23.0` + push → tests → guarded
  build → release with checksum → device OTA'd onto it (30 s self-test,
  rollback armed). One manual step between source and speaker.

### Phase 24 — JTAG debug session (walkthrough, no new code)

- One USB cable = console + JTAG (USB-Serial-JTAG exposes both). OpenOCD
  bridges the chip to GDB's port 3333; `info threads` shows the entire
  task map LIVE — we caught seg_dec running on CPU0 and lvgl on CPU1 at
  the halt instant, the designed core split photographed in the act.
- Breakpoint on `ev_next` + a real finger tap captured the full input
  pipeline as a call stack (touch release → LV_EVENT_CLICKED → handler)
  with `s_cur=6 / TOKYO FM` at entry; single-step showed the increment
  poised. CPU0 was frozen inside `raac_QMFAnalysis` — mid-audio-frame.
- **Audio keeps playing while both CPUs are halted**: I2S DMA is hardware
  and drains the 30 s PCM ring. You can HEAR the memory architecture.
- Three self-inflicted lessons, all field-relevant: (1) discarding stderr
  hid "No such file" — the first breakpoint silently never existed;
  (2) with no live target GDB `print` answers from the ELF's initializers
  — plausible and fake; (3) **the ELF must match the running build
  exactly** — the device was on the CI-built v0.23.0 while GDB held the
  local ELF, so the trap sat at an address where nothing lived. Fix
  applied: releases now archive the ELF beside the .bin.
- IDE attach (.vscode/launch.json, cppdbg → OpenOCD): gutter breakpoints
  on the live radio, verified pausing on the heartbeat every 10 s.

### Phase 25 Stage A — Signed OTA (reversible, no eFuses)

- **Signature verification has a software-only mode**
  (`SECURE_SIGNED_APPS_NO_SECURE_BOOT`) — the app checks OTA signatures with
  the public key, zero eFuse involvement. Flash encryption has NO such mode
  (its key MUST live in eFuse), which is why encryption is Stage B by
  definition and signing can be Stage A.
- **A signed image is a superset**: signing appends a block that
  non-verifying radios ignore. So ONE CI release stream serves both profiles
  — no dual artifacts (which our "first .bin in the release" OTA picker
  couldn't disambiguate anyway).
- Delivered as a **switchable overlay** (`sdkconfig.signed`), not a branch:
  default `idf.py build` is byte-for-byte today's radio; the signed profile
  is opt-in per build. Both coexist in the repo permanently — there is no
  "before" to revert to.
- **Overlay gotcha that cost a rebuild**: `SDKCONFIG_DEFAULTS` is consulted
  ONLY when the target `sdkconfig` is absent. Two profiles sharing the root
  `sdkconfig` means the second silently inherits the first's config. Fix:
  give each profile its own `-DSDKCONFIG=<build_dir>/sdkconfig`.
- Key hygiene: private key in `~/.s3-radiko/` (never the repo), a `*.pem`
  gitignore tripwire, `idf/signing_key.pem` a gitignored symlink for local
  builds, and the key as a GitHub Actions secret for CI signing. The key is
  the same one Stage B would burn — generate it once, back it up now.
- Threat-model honesty (in the runbook): Stage A closes the *software* gap
  (repo access ≠ firmware access). Stage B's hardware burn defends against a
  *physical* attacker with a soldering iron — not this project's threat, and
  irreversible on a one-of-one board. Parked deliberately, documented fully.

### Phase 31 — LVGL heap exhaustion (the WiFi page reboot)

Symptom: touching the WiFi icon rebooted the radio, every time. Not a crash —
an **out-of-memory condition wearing a crash costume**.

- **LVGL OOM presents as a HANG, not an error.** `lv_malloc` returned NULL,
  `LV_ASSERT_MALLOC` fired, and LVGL's default `LV_ASSERT_HANDLER` is literally
  `while(1);` ("Halt by default"). The LVGL task froze mid-draw, stopped feeding
  the task watchdog, and 15 s later the WDT panicked and rebooted. The coredump
  therefore blamed `circ_calc_aa4` — innocent rounded-corner drawing code — and
  the panic reason said "watchdog". Neither is the bug. When a malloc-fail
  handler is an infinite loop, **every OOM is misreported as a hang in whatever
  code happened to allocate last.** (Sometimes a NULL write won the race first
  and it panicked as `StoreProhibited` instead — same cause, different costume.)
- **Measure before theorising.** The obvious suspect was `lv_keyboard_create()`
  — the biggest widget in LVGL, built by the WiFi page and cached forever.
  `lv_mem_monitor()` said the whole WiFi page costs **2.9 KB**. The real problem
  was a **77% baseline**: player + station list + settings + 47-area dropdown all
  stay resident. The scan list (~9 KB for 20 APs) merely delivered the last straw.
  The expensive-looking thing was not the expensive thing.
- **`idf.py size` "remain" is NOT free runtime memory.** It reported ~109 KB
  DIRAM spare; the boot log reported `free internal 27 KB (largest 8)` — WiFi and
  LWIP claim the rest at runtime. Growing the internal pool by 32 KB would have
  traded a WiFi-page reboot for a WiFi-stack failure. **Size internal-RAM
  decisions from a running device, never from the link map.** Meanwhile 2.1 MB of
  PSRAM sat idle.
- **Fix:** `lv_mem_add_pool()` a 256 KB PSRAM pool in `ui_init()`. LVGL's TLSF
  happily spans multiple pools. Draw buffers stay internal + DMA-capable (Phase
  14's hard-won constraint); the spill pool only ever holds object/style/mask
  metadata, which is small and cache-friendly. Peak went 100% → 19%, at zero cost
  to internal RAM.
- **The trap that nearly shipped a fake fix:** `lv_mem_add_pool()` **silently
  rejected** the pool. TLSF sizes its index tables at COMPILE time from
  `TLSF_MAX_POOL_SIZE = LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE`, and refuses any
  pool larger than `1 << log2ceil(that)` — 64 KB with the default expand of 0.
  Its complaint goes to `LV_LOG_WARN` (compiled out: `LV_USE_LOG` off) and a
  `printf` that early-boot buffering swallowed. So it failed **with no output at
  all**, and the first test run "passed" only because that scan found 16 APs
  instead of 20. **`CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=256` is the knob**;
  it reserves TLSF index capacity and allocates nothing itself. Lessons: check
  the return value of anything that can fail silently, and **"it didn't crash
  once" is not verification** — prove the mechanism engaged (here: `total_size`
  jumping 64 KB → 320 KB), because the environment (AP count) moves under you.
- Editing `sdkconfig.defaults` alone does nothing once `sdkconfig` exists — it is
  only seeded when absent. Delete `sdkconfig` + `idf.py reconfigure`, then diff to
  confirm exactly one line moved (same family as the Phase 25 overlay gotcha).

### Phase 32 — audio visualiser (and the crack it caused)

- **Tap the audio where it is HEARD, not where it is made.** The PCM ring is 30 s
  deep, so an FFT of the decoder's output would drive the bars from audio nobody
  hears for half a minute. Tapping `chunk[4096]` in `i2s_writer_task` leads the
  speaker by the chunk (~21 ms) + DMA queue (~30 ms) — under perception. Free,
  once you notice the trap; a day lost if you don't.
- **AAC is already frequency-domain** (libhelix has MDCT coefficients before the
  inverse transform), so a "free" spectrum is tempting. It fails the same test:
  those coefficients live at the decoder, 30 s early.
- **esp-dsp on the S3 uses a static twiddle table for FFT sizes ≤ 1024** — zero
  heap. Ask for 4096 and it `memalign`s 16 KB of internal RAM, which this radio
  does not have. 512-pt (93.75 Hz bins) is plenty for 16 log bands.
- **`lv_grad_stop_t` carries a per-stop `opa`.** A zero-initialised
  `lv_grad_dsc_t` therefore describes fully TRANSPARENT stops: the LED bars
  vanished while the peak caps (plain `bg_color`) still drew. Symptom read as
  "gradient unsupported"; cause was one unset field.
- **Guards that turn a crash into silence are worse than the crash.**
  `viz_apply()` ran before `w_name`/`w_prog` existed; the `if (w_name)` guards
  dutifully skipped half the layout, so a radio booting into the visualiser drew
  the station name on top of the bars. A NULL deref would have been found in
  seconds. Order-of-construction bugs deserve a comment, not a guard.

**The audio crack — three wrong theories, then the data.**

Switching to the visualiser made audio crack after a while; switching back fixed
it. Claimed impossible, on the grounds that `i2s_wr` (prio 6) preempts `lvgl`
(prio 4) on core 1. That argument was true and useless: **priority governs CPU,
and CPU was never the contended resource.**

- Instrumented the writer: `gap>35ms` = DMA underrun, plus the ring level at that
  moment. **`ring=4KB` was the whole answer** — the writer wasn't starved of CPU
  (it would have found the ring FULL); there was simply nothing to play. The
  producer, on the OTHER core, was losing.
- A 30 s A/B (logo vs bars, flipped by the firmware itself so no human timing was
  involved) then looked *clean* — because the ring drains slowly and 30 s windows
  never emptied it. **A too-short experiment nearly exonerated the culprit.**
- The ring fill curve settled it: **+36 KB/s with the logo showing, −17 KB/s with
  bars** — i.e. the visualiser drags the decoder from ~1.19× real-time down to
  ~0.91×. Below 1× the ring bleeds out over ~40 s and *then* it cracks. That delay
  is why it read as "cracks a few seconds after switching" rather than "the bars
  cost throughput".
- Cause: **cross-core contention for PSRAM/cache, not CPU.** The PCM ring, the
  mbedtls buffers and LVGL's spill pool are all in PSRAM, and libhelix executes
  from flash through the same cache. `obj_max≈3.8 ms` for ~32 rectangle updates
  (~120 µs each!) is LVGL walking style data over PSRAM — that traffic is what
  starves the decoder on core 0.
- Fix: **10 fps** (slope back to +5..+12 KB/s, and visually indistinguishable),
  plus a **hard gate**: `viz_tick` refuses to draw below 1.5 s buffered and only
  resumes above 3 s. The frame rate is a budget that could be blown by a slow CDN;
  the gate makes starving the audio structurally impossible. **Audio wins, always.**

**Lesson: "X can't affect Y, different cores/priorities" is a hypothesis, not a
proof.** Shared silicon — cache, PSRAM bandwidth, flash — couples things the
scheduler diagram says are independent. Measure the *sign of the queue*: a buffer
that drains tells you who is losing, and which side of the pipe to look at.

### Phase 32 (cont.) — the screen freeze, and five more wrong theories

With audio fixed, the bars *froze the UI* for 2-3 s periodically. The hunt took
six failed hypotheses before the data spoke — a record even for this project.

- **It's not the gradient, object count, CPU, or the audio code.** The first LED
  build used a full-height green->amber->red gradient per band + a shutter masking
  the unlit part: every pixel painted ~3x with per-pixel interpolation. Replaced
  with solid colour *zones* sized to the lit slice — paint lit pixels once. Helped
  throughput but did NOT fix the freeze.
- **The real cause: LVGL silently repaints the WHOLE SCREEN when its dirty-area
  list overflows.** `lv_refr.c`: "If no place for the area add the screen". The
  list is `LV_INV_BUF_SIZE` = **32** by default. The LED style's ~74 objects
  invalidate ~148 areas a frame, blew 32, and every frame became a full 320x240
  repaint — a rock-steady 163 ms stall. Fix: `-DLV_INV_BUF_SIZE=192` (not exposed
  in Kconfig) so the list holds the small areas, plus a deliberate whole-strip
  `lv_obj_invalidate()` that lets LVGL merge them into ~5 buffer-sized flushes.
- **The number that cracked it was px/s × flush-count, not either alone.** The
  logo screen paints *more* pixels than the bars and is smooth; the bars pushed
  400 k px/s in **70 flushes** (vs the tile's true ~262 k in ~50). 400 k in 70
  flushes is the fingerprint of the whole-screen fallback. No single metric shows
  it — pixels looked fine, flush count looked fine-ish, the product did not.
- **Disjoint rects don't merge.** 16 bars with gaps between them = 16 dirty areas
  LVGL can't coalesce = ~15 tiny SPI flushes/frame, each paying an address-window
  + DMA-start + ISR + semaphore round-trip to move ~800 px. Asking for ONE big
  area (more pixels!) was ~2x faster. Repainting more can be cheaper.
- **The measurement caused a second, fake freeze.** My stall/repaint/ring probes
  logged with `ESP_LOGW`, and the elog flash ring captures W/E. That flooded elog,
  which flushed to flash every 10 s, and a 4 KB sector erase disables the cache
  for ~40 ms — stalling the (non-IRAM) SPI submit path. A silent ~48 ms flush
  every ~20 s, gone the instant the instrumentation was removed. Debug output that
  writes to flash perturbs exactly the timing you're trying to measure.

**Lesson: a metric can look healthy on every axis and still be wrong on the
product of two.** And instrumentation that touches flash (or any cache-disabling
op) is not free — it can manufacture the symptom you're chasing.

### Phase 32 — final shape (shipped)

- **Four player-tile styles**, long-press the logo to cycle: logo -> rainbow bars
  -> LED/car (green/amber/red, peak caps, grille) -> LED-rainbow (car layout, band
  hues) -> logo. `settings.viz` is `uint8_t` so styles cost no schema bump.
- **Audio-safe by construction**: PCM tapped in `i2s_writer_task` (what's heard,
  not the 30 s-early decoder output); 10 fps; a ring-level gate that pauses the
  bars below 1.5 s buffered and resumes above 3.0 s — which also covers startup and
  station changes (the decoder is catching up to the live edge then; lowering the
  gate to start bars sooner brought crackle straight back). Audio always wins.
- **Idle fall**: when the buffer goes thin the bars *ease* to a flat baseline over
  ~0.7 s (peak caps riding down with them) instead of snapping or freezing at their
  last heights, so boot/station-change reads as "settling", not a hang.
- **Gestures** (swipe/tap/long-press) cover the whole logo + name + programme
  block via a transparent layer, not just the logo.
