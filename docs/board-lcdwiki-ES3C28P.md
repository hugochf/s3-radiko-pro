# Board reference — lcdwiki ES3C28P (ESP32-S3 2.8" IPS)

A self-contained hardware cheat-sheet for this board, so any future project can
skip the trial-and-error. Verified on-device with ESP-IDF v5.3.5.

## Getting one

This board has no single canonical name — it's a generic design sold by several
vendors (DIY MORE and others) on Taobao, Pinduoduo and AliExpress. Searching any
of these finds it:

- `ESP32-S3 2.8 inch capacitive touch LCD development board`
- `S3小智AI聊天机器人WiFi开发板` — the "XiaoZhi AI chatbot" board, its most
  common listing name (the AI-assistant firmware it's marketed for is irrelevant
  here; the hardware is what matters)
- `ESP32-S3 开发板 WiFi 蓝牙 智能显示屏 2.8寸 触摸屏幕`

Roughly ¥70–90 / US$10–15 as of mid-2026. It usually arrives bare — a speaker
(JST connector, on-board amp) and a LiPo battery are separate.

**Match these specs, or this project won't run.** Near-identical boards ship with
weaker memory, and the difference is invisible in the photos:

| Must have | Why | Common wrong variant |
|---|---|---|
| **16 MB flash** | dual-OTA layout + a 3 MB logo partition + 128 KB key partition | 8 MB — the partition table won't fit |
| **8 MB PSRAM, Octal (OPI)** | audio pipeline, mbedtls buffers, LVGL spill pool | 2 MB Quad (QSPI), or none — `CONFIG_SPIRAM_MODE_OCT` won't boot |
| ILI9341 320×240 SPI | display driver | ST7789 variants exist |
| FT6336G capacitive touch | touch driver | resistive versions exist |
| ES8311 codec + amp | audio out | display-only boards look the same |

The ESP32-S3 module marking to look for is **N16R8** (16 MB flash / 8 MB PSRAM).
If a listing doesn't state flash and PSRAM explicitly, assume the smaller part.
`esptool.py flash_id` and the boot log confirm both once it arrives — this repo's
boot log prints `Adding pool of 8192K of PSRAM`.

## SoC / memory

| Item | Value |
|------|-------|
| MCU | ESP32-S3 (dual-core Xtensa LX7), rev v0.2 |
| Flash | 16 MB, QIO @ 80 MHz |
| PSRAM | 8 MB **Octal (OPI)** @ 80 MHz (AP gen-3, 64 Mbit) |
| USB | Native **USB-Serial-JTAG** (flash + monitor + JTAG over one cable) |

`sdkconfig.defaults` essentials:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y          # OPI PSRAM — mandatory, not Quad
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

## Peripherals on board

- **Display:** ILI9341 320×240 IPS, SPI
- **Touch:** FT6336G capacitive, I²C
- **Audio codec:** ES8311, I²C control + I²S data
- **Speaker amp:** FM8002E (enable is active-LOW)
- **RGB LED:** 1× WS2812B
- **Battery:** Li-ion via ADC voltage divider; charge by TP4054 (no charge-status GPIO)
- **Buttons:** BOOT (GPIO0), RESET/EN

## Full pin map

### Display — ILI9341 over SPI (SPI2_HOST / FSPI)

| Signal | GPIO | Notes |
|--------|------|-------|
| SCLK | 12 | |
| MOSI | 11 | |
| MISO | 13 | unused (write-only), wired |
| CS | 10 | |
| DC | 46 | ⚠ strapping pin |
| RST | −1 | tied to board RESET → use **software** reset |
| Backlight | 45 | ⚠ strapping pin; active-HIGH, drive with LEDC PWM |

SPI clock 40 MHz. Native panel is 240(W)×320(H) portrait.

**Panel quirks (all three required, or colours/orientation break):**

1. **Colour inversion ON.** `esp_lcd_panel_invert_color(panel, true)`.
   (Tell-tale if wrong: white shows as black.)
2. **BGR order + big-endian pixels, together.** `rgb_ele_order = BGR` **and**
   byte-swap every RGB565 word (`__builtin_bswap16`, or LVGL's 16-bit swap).
   Only one of the two → looks like a green/blue swap; the other alone → red/blue swap.
3. **Mounted upside-down.** For 320×240 landscape: `swap_xy(true)` +
   `mirror(true, true)`. With `swap_xy` on, `mirror_x` is the *vertical* axis and
   `mirror_y` the *horizontal* one (transposed).

ILI9341 vendor driver: managed component `espressif/esp_lcd_ili9341` (^1.2.0) —
not part of esp_lcd core.

### Shared I²C bus (ES8311 + FT6336G)

| Signal | GPIO |
|--------|------|
| SDA | 16 |
| SCL | 15 |

400 kHz. One `i2c_master` bus, two devices.

### Touch — FT6336G (I²C)

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA/SCL | 16 / 15 | shared bus above |
| INT | 17 | |
| RST | 18 | reset pulse: HIGH 20 ms → LOW 20 ms → HIGH 500 ms |
| I²C addr | 0x38 | TD_STATUS reg 0x02; touch data from reg 0x03 |

Native touch coords are 240×320 → map to your rotation. For this board's
upside-down landscape the Arduino used: `x = 320 − raw_y; y = raw_x` (rotation 3).

### Audio codec — ES8311 (I²C control + I²S data)

| Signal | GPIO | Notes |
|--------|------|-------|
| I²C addr | 0x18 | CE low = 0x18 |
| MCLK | 4 | 12.288 MHz = 256 × 48 kHz |
| BCLK | 5 | |
| WS/LRCK | 7 | |
| DOUT (→DAC) | 8 | ESP32 → codec |
| Amp enable | 1 | FM8002E, **active-LOW** (drive LOW to unmute speaker) |

Codec runs I²S **slave**, MCLK = 256·fs, 16-bit. Lock I²S at 48 kHz so MCLK stays
12.288 MHz. (Espressif ES8311 driver init works; init twice — once before MCLK is
running, once after it locks.)

### RGB LED — WS2812B

| Signal | GPIO |
|--------|------|
| Data | 42 |

Drive with the `led_strip` component (RMT backend) under ESP-IDF.

### Battery ADC

| Item | Value |
|------|-------|
| Pin | GPIO9 = **ADC1 channel 8** |
| Divider | ×2 (multiply reading by 2) |
| Atten / width | `ADC_ATTEN_DB_12`, 12-bit; use curve-fitting calibration |
| Charge status | none — TP4054 CHRG drives an LED only (infer charging from rising mV) |

Rough Li-ion curve: 4200 mV≈100 %, 3700 mV≈50 %, 3300 mV≈5 %, 3000 mV≈0 %.

### Buttons / power

- **BOOT** = GPIO0 (also download-mode strap). Usable as a wake source.
- Deep-sleep wake used by the Arduino build: `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)` (wake on LOW).

## Strapping pins in use

GPIO0 (BOOT), **GPIO45 (backlight)** and **GPIO46 (DC)** are ESP32-S3 strapping
pins. Driving 45/46 as normal outputs after boot is fine (the Arduino build did),
but keep it in mind if download-mode entry ever gets flaky.

## Flashing / debug gotchas

- Flash + monitor over the native USB: `idf.py -p /dev/cu.usbmodem2101 flash monitor`.
- **Do NOT toggle RTS/DTR** on the USB-Serial-JTAG port from your own scripts — it
  wedges the port (`esptool: No serial data received`, no reset mode recovers).
  Recover by unplugging/replugging the cable (or hold BOOT during reset). Read the
  port passively.
- If another ESP board is plugged in, target the port explicitly; esptool's
  chip-type check will refuse a mismatched flash, but don't rely on luck.
