#!/usr/bin/env python3
"""
Fetch the highest-resolution station logos from Radiko's API for area JP14
and regenerate station_logos.c with RGB565 data.

Requires: Pillow (pip3 install Pillow)
After running: re-run crop_logos.py to strip white padding from the new logos.
"""
import io
import re
import sys
import urllib.request
import urllib.error
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)

OUT = Path(__file__).parent / "station_logos.c"
AREA = "JP14"

# Station IDs in the order they appear in S3_Radiko.ino
STATIONS = [
    ("TBS",      "logo_TBS"),
    ("QRR",      "logo_QRR"),
    ("LFR",      "logo_LFR"),
    ("RN1",      "logo_RN1"),
    ("RN2",      "logo_RN2"),
    ("INT",      "logo_INT"),
    ("FMT",      "logo_FMT"),
    ("FMJ",      "logo_FMJ"),
    ("JORF",     "logo_JORF"),
    ("BAYFM78",  "logo_BAYFM78"),
    ("NACK5",    "logo_NACK5"),
    ("YFM",      "logo_YFM"),
    ("IBS",      "logo_IBS"),
    ("JOAK",     "logo_JOAK"),
    ("JOAK-FM",  "logo_JOAK_FM"),
]

# Common logo sizes Radiko offers (try largest first)
CANDIDATE_SIZES = [
    (688, 160),
    (440, 100),
    (224, 100),
    (216, 54),
    (168, 62),
    (112, 46),
]

def fetch_station_list(area):
    """Get the station list XML and parse out logo URLs per station."""
    urls = [
        f"https://radiko.jp/v3/station/list/{area}.xml",
        f"https://radiko.jp/v2/station/list/{area}.xml",
    ]
    for url in urls:
        try:
            print(f"Fetching {url}")
            data = urllib.request.urlopen(url, timeout=15).read()
            return ET.fromstring(data)
        except (urllib.error.URLError, ET.ParseError) as e:
            print(f"  failed: {e}")
    return None

def best_logo_url(station_elem):
    """Pick the largest logo URL from a <station> element. Returns (url, w, h)."""
    logos = station_elem.findall("logo")
    if not logos:
        return None
    # Sort by area (w*h) descending
    def area(l):
        try:
            return int(l.get("width", "0")) * int(l.get("height", "0"))
        except ValueError:
            return 0
    logos = sorted(logos, key=area, reverse=True)
    best = logos[0]
    return (best.text.strip(), int(best.get("width")), int(best.get("height")))

def try_direct_urls(sid):
    """Fallback: try standard URL patterns if station list lookup failed."""
    for w, h in CANDIDATE_SIZES:
        url = f"https://radiko.jp/v2/static/station/logo/{sid}/{w}x{h}.png"
        try:
            req = urllib.request.Request(url, method="HEAD")
            urllib.request.urlopen(req, timeout=10)
            return (url, w, h)
        except urllib.error.URLError:
            continue
    return None

def fetch_and_convert(url):
    """Download an image, composite onto white, return (pixels[], w, h)."""
    print(f"  GET {url}")
    data = urllib.request.urlopen(url, timeout=20).read()
    img = Image.open(io.BytesIO(data))
    # Composite alpha onto white background
    if img.mode in ("RGBA", "LA") or "transparency" in img.info:
        rgba = img.convert("RGBA")
        bg = Image.new("RGB", rgba.size, (255, 255, 255))
        bg.paste(rgba, mask=rgba.split()[3])
        img = bg
    img = img.convert("RGB")
    w, h = img.size
    pixels = []
    raw = img.tobytes()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        pixels.append(rgb565)
    return pixels, w, h

def format_array(pixels, indent="  "):
    lines = []
    per_line = 16
    for i in range(0, len(pixels), per_line):
        chunk = pixels[i:i + per_line]
        lines.append(indent + ", ".join(f"0x{p:04X}" for p in chunk) + ",")
    return "\n".join(lines)

def write_station_logos_c(results):
    """Generate the full station_logos.c file from the (var, pixels, w, h) list."""
    out = ['#include "station_logos.h"\n']
    for var, pixels, w, h in results:
        out.append(f"static const uint16_t {var}_data[] = {{")
        out.append(format_array(pixels))
        out.append("};\n")
    for var, pixels, w, h in results:
        out.append(f"const lv_img_dsc_t {var} = {{")
        out.append("  .header.cf = LV_IMG_CF_TRUE_COLOR,")
        out.append("  .header.always_zero = 0,")
        out.append("  .header.reserved = 0,")
        out.append(f"  .header.w = {w},")
        out.append(f"  .header.h = {h},")
        out.append(f"  .data_size = {w * h * 2},")
        out.append(f"  .data = (const uint8_t*){var}_data,")
        out.append("};\n")
    OUT.write_text("\n".join(out))
    print(f"\nWrote {OUT}")

def main():
    root = fetch_station_list(AREA)
    station_elems = {}
    if root is not None:
        for s in root.findall("station"):
            sid_elem = s.find("id")
            if sid_elem is not None:
                station_elems[sid_elem.text] = s

    results = []
    for sid, var in STATIONS:
        print(f"\n[{sid}]")
        info = None
        if sid in station_elems:
            info = best_logo_url(station_elems[sid])
        if not info:
            print("  not in station list, trying direct URLs")
            info = try_direct_urls(sid)
        if not info:
            print(f"  ERROR: no logo found for {sid}")
            sys.exit(1)
        url, w, h = info
        pixels, real_w, real_h = fetch_and_convert(url)
        print(f"  {sid}: {real_w}x{real_h} ({len(pixels) * 2} bytes)")
        results.append((var, pixels, real_w, real_h))

    # Backup before overwriting
    bak = OUT.with_suffix(".c.bak2")
    if OUT.exists():
        bak.write_text(OUT.read_text())
        print(f"\nBackup: {bak}")

    write_station_logos_c(results)
    print("\nNext: run `python3 crop_logos.py` to strip white padding.")

if __name__ == "__main__":
    main()
