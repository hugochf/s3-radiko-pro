# Architecture

How the pieces fit at runtime. This is the cross-cutting reference that the
per-phase notes in [../PLAN.md](../PLAN.md) don't consolidate — the task map and
the memory budget in particular have been the source of several bugs, so they
live here as the single source of truth.

## Data flow

```
                        Wi-Fi (STA) ── SNTP (JST) ──┐  (auth needs real time)
                                                    ▼
  radiko_auth ──► auth1/auth2 ──► token + area (JP14)
                                                    │
                                                    ▼
  ┌─────────────── stream (two-stage HLS pipeline) ───────────────┐
  │  fetcher (core 0)                     decoder (core 1)         │
  │  stream-info XML                                               │
  │   → playlist_create_url                                       │
  │   → master m3u8  ──► media m3u8 ──► .aac segments             │
  │                         │                                      │
  │            heap_caps_malloc(SPIRAM) 64 KB each                 │
  │                         ▼                                      │
  │                   seg queue (depth 4) ──► AACDecode (libhelix) │
  │                                              │  int16 PCM      │
  └──────────────────────────────────────────────┼───────────────┘
                                                  ▼
                     PCM StreamBuffer (15 s, PSRAM) ──► i2s_writer (core 1)
                                                          │ I2S0 @ 48 kHz
                                                          ▼
                                     ES8311 codec ──► FM8002E amp ──► speaker

  LVGL UI (core 1) ── touch poll (I2C FT6336) ── station/volume ─► stream + audio
```

The two-stage split (separate fetcher and decoder tasks with a queue between) is
mandatory: the CDN serves live segments at ~1× the stream bitrate, so a single
fetch→decode→play loop runs sub-real-time and starves. Overlapping network
latency with decode/playback is what keeps audio continuous.

## Task map

Everything time-sensitive is pinned. **Core 1 hosts the UI, the decoder, and the
I2S writer; core 0 hosts networking (Wi-Fi + the segment fetcher).** Priorities on
core 1 are deliberate — see the note below.

| Task          | Core | Prio | Stack  | Lifetime  | Job |
|---------------|:----:|:----:|-------:|-----------|-----|
| `i2s_wr`      |  1   |  6   |  4 KB  | permanent | Drain PCM ring buffer → I2S0 DMA |
| `stream_ctl`  | any  |  6   |  3 KB  | permanent | Serialize play/stop commands (1-deep queue, latest wins) |
| `seg_fetch`   |  0   |  5   | 16 KB  | per-play  | Resolve playlists, fetch AAC segments → queue |
| `lvgl`        |  1   |  4   | 16 KB  | permanent | `lv_timer_handler`: render, poll touch, run UI events |
| `seg_dec`     |  1   |  3   | 20 KB  | per-play  | Queue → libhelix AAC decode → `audio_write` |
| `radiko_auth` | any  |  5   |  8 KB  | one-shot  | auth1/auth2 at boot, then exits |
| `wifiscan`    | any  |  4   |  4 KB  | one-shot  | Wi-Fi AP scan for the setup screen |

Plus LVGL timers running inside the `lvgl` task: the 2 s status-bar refresh and
the one-shot 450 ms prev/next debounce.

**Why the decoder sits *below* LVGL (prio 3 < 4) on the same core.** Keeping the UI
above the decoder means touch stays responsive under decode load, and the 15 s PCM
buffer gives the decoder plenty of slack to catch up. The tradeoff: sustained heavy
UI work *can* starve the decoder. That is exactly what bit Phase 13 — rendering 15
scaled logos ran LVGL's transform path continuously and stalled audio; the fix was
to make the logos render at 1× (a cheap blit) rather than to reshuffle priorities.

**Stack sizes are not arbitrary.** The decoder needs 20 KB (libhelix frame work).
The LVGL task was raised to 16 KB because v9's image draw/decode path is far deeper
than the label/tile path. With `CONFIG_COMPILER_STACK_CHECK_MODE_NONE`, a plain
overflow silently corrupts memory and wedges the chip — so the hardware
stack-overflow watchpoint (see [debugging.md](debugging.md)) is enabled to catch it.

## Memory budget

Internal SRAM is the scarce resource; PSRAM (8 MB) is plentiful but cache-backed
(slower, and unusable while a flash write has the cache disabled). The rule:
**latency/DMA-critical and TLS buffers go in internal RAM; big bulk buffers go in
PSRAM.**

### Internal SRAM (~200 KB usable after the stack/heap init)

| User | Approx | Why internal |
|------|-------:|--------------|
| TLS / mbedtls session | ~40 KB contiguous | `mbedtls_ssl_setup` fails (`-0x7F00`) if it can't get a contiguous internal block |
| libhelix decoder state | ~20–30 KB | CPU-bound; `helix_malloc` → plain `malloc` (internal). Routing it to PSRAM once caused an IDLE-starve watchdog — it was too slow there |
| Task stacks, FreeRTOS objects, Wi-Fi | — | Kernel + driver requirements |

The internal-RAM squeeze is real: fitting the ~40 KB TLS block is what forced the
LVGL draw buffers (below) out to PSRAM in the first place.

### PSRAM (8 MB)

| User | Size | Notes |
|------|-----:|-------|
| PCM ring buffer | ~2.75 MB | `48000 × 4 bytes × 15 s`, static `StreamBuffer` |
| AAC segment buffers | up to ~320 KB | 64 KB × (queue depth 4 + in-flight) |
| LVGL draw buffers | ~50 KB | 2 × (320 × 40 lines × 2 B), partial render. Freed the internal RAM TLS needed |
| Playlist scratch | 4 KB | m3u8 text |

### Flash (16 MB, dual-OTA)

Station logos are embedded RGB565 blobs (~110 KB) living in memory-mapped flash
(DROM) via `EMBED_FILES` — they cost flash, not RAM. Partition map
(`idf/partitions.csv`): two 3 MB OTA app slots, 256 KB coredump, the rest SPIFFS
storage. The layout was fixed at Phase 0 so it never has to change in the field.

## Init sequence

`main/main.c` brings the system up in dependency order:

```
nvs_flash_init → settings_init → display_init → ui_init (lv_init) →
i2c_bus_init → touch_init → audio_init (+ apply saved volume) →
stream_control_start → wifi_start → [ui_show_wifi_setup if no creds] →
timesync_start → radiko_auth task
```

`ui_init` runs before `touch_init` because the touch input device attaches to an
already-initialized LVGL. `stream_control_start` creates the persistent keep-alive
HTTP client and the control task, but no stream plays until auth completes and the
UI calls `stream_play`.

## Concurrency & flush notes

- **UI thread-safety:** all LVGL calls hold a recursive mutex (`ui_lock`/`ui_unlock`).
  The `lvgl` task holds it across `lv_timer_handler`; other tasks (e.g. `radiko_auth`
  calling `ui_set_playing`) take it too.
- **Display flush is semaphore-driven, not busy-wait.** The SPI DMA-done ISR calls
  `lv_display_flush_ready` and gives a semaphore; a custom `flush_wait_cb` blocks on
  it (yielding the CPU) with a 100 ms timeout that self-heals a lost completion.
  LVGL's default busy-spin would starve the idle task and trip the watchdog.
- **Station navigation is debounced.** Prev/next update the UI instantly but defer
  the NVS write and the actual stream switch until ~450 ms after the user settles,
  so mashing the buttons doesn't hammer flash or restart the pipeline repeatedly.
- **Touch is polled, not interrupt-driven.** The FT6336 INT line is left
  unconnected (`GPIO_NUM_NC`) on purpose — a non-IRAM GPIO ISR firing during a
  flash write (cache disabled) would hard-wedge the chip.
