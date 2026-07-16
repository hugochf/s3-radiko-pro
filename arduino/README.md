# Arduino original (archived)

The **original** S3 Radiko — a working Arduino sketch that played live Radiko on
the lcdwiki ES3C28P board. It is kept here as the reference the ESP-IDF rebuild
was measured against, phase by phase.

**Superseded by [`../idf/`](../idf/).** This sketch gets no new features and no
bug fixes; everything since lives in the IDF project. The rebuild's motivation,
the parity checklist, and the running engineering log are in
[`../PLAN.md`](../PLAN.md).

## What's here

```
S3_Radiko/          the sketch — folder name must match the .ino (Arduino rule)
  S3_Radiko.ino     the whole app: UI, audio, Radiko auth, ~2 kloc in one file
  es8311.*          codec driver (Apache-2.0, from Espressif)
  touch.h           FT6336G polling
  lv_font_jp_16.c   16 px JP font, 4 bpp, 13-glyph subset   — see Fonts below
  lv_font_jp_full.c 16 px JP font, 4 bpp, full CJK (9.5 MB) — see Fonts below
  station_logos.*   Tokyo-area station logos, RGB565
  partitions.csv    single-app flash map (no OTA)
tools/              one-shot asset generators for this sketch
  fetch_logos.py    download + convert station logos -> station_logos.c
  crop_logos.py     strip white padding from the fetched logos (in place)
```

## Building

Arduino IDE, ESP32 board support, target ESP32-S3, 16 MB flash, OPI PSRAM
enabled. Open `S3_Radiko/S3_Radiko.ino` — the Arduino IDE requires the sketch
folder and the `.ino` to share a name, which is why it's nested.

Requires LVGL v9 and the Helix AAC decoder installed as Arduino libraries. Wi-Fi
credentials are hardcoded in the sketch (a throwaway test credential, since
rotated) — the IDF rebuild replaced this with on-screen provisioning and a
gitignored secrets header.

## Fonts — licensing note

Both `lv_font_jp_16.c` and `lv_font_jp_full.c` were generated with `lv_font_conv`
from **Hiragino Sans W3**, an Apple system font. Hiragino is proprietary and is
**not** redistributable — if you reuse this code, regenerate the fonts from a
font you're licensed to ship. The IDF rebuild uses
[Noto Sans JP](https://fonts.google.com/noto/specimen/Noto+Sans+JP) (SIL Open
Font License) for exactly this reason.

Station logos are the trademarks of their respective broadcasters, embedded for a
personal, non-commercial reproduction of the physical radio's UI.
