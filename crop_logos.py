#!/usr/bin/env python3
"""
Crop white padding from each logo in station_logos.c.
Reads each logo_X_data[] block, scans for non-white pixel bbox,
emits cropped data and updates dsc dimensions.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).parent / "station_logos.c"
OUT = SRC  # rewrite in place

WHITE = 0xFFFF

def parse_arrays(text):
    """Yield (name, [pixel ints]) for every static const uint16_t logo_X_data[] block."""
    pat = re.compile(
        r"static const uint16_t (logo_\w+)_data\[\]\s*=\s*\{([^}]*)\};",
        re.DOTALL,
    )
    for m in pat.finditer(text):
        name = m.group(1)
        body = m.group(2)
        nums = re.findall(r"0x[0-9A-Fa-f]+", body)
        pixels = [int(n, 16) for n in nums]
        yield name, pixels, m.start(), m.end()

def crop_bbox(pixels, w, h, margin=2):
    """Find tightest bbox of non-white pixels with optional margin."""
    min_x, min_y, max_x, max_y = w, h, -1, -1
    for y in range(h):
        row = pixels[y * w:(y + 1) * w]
        for x in range(w):
            if row[x] != WHITE:
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y
    if max_x < 0:
        return 0, 0, w, h  # all white, no crop
    # Add margin (clamp to image bounds)
    min_x = max(0, min_x - margin)
    min_y = max(0, min_y - margin)
    max_x = min(w - 1, max_x + margin)
    max_y = min(h - 1, max_y + margin)
    new_w = max_x - min_x + 1
    new_h = max_y - min_y + 1
    return min_x, min_y, new_w, new_h

def crop_pixels(pixels, w, h, x0, y0, new_w, new_h):
    out = []
    for y in range(y0, y0 + new_h):
        out.extend(pixels[y * w + x0 : y * w + x0 + new_w])
    return out

def format_array(pixels, new_w, indent="  "):
    """Format pixel ints as 16-per-line hex array body."""
    lines = []
    per_line = 16
    for i in range(0, len(pixels), per_line):
        chunk = pixels[i:i + per_line]
        lines.append(indent + ", ".join(f"0x{p:04X}" for p in chunk) + ",")
    return "\n".join(lines)

def parse_dsc_dims(text, name):
    """Read .header.w and .header.h from the lv_img_dsc_t block for `name`."""
    pat = re.compile(
        rf"const lv_img_dsc_t {name} = \{{.*?\.header\.w\s*=\s*(\d+).*?\.header\.h\s*=\s*(\d+)",
        re.DOTALL,
    )
    m = pat.search(text)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))

def main():
    text = SRC.read_text()

    # Parse all arrays first
    arrays = list(parse_arrays(text))
    print(f"Found {len(arrays)} logo arrays")

    # Compute crops
    new_dims = {}  # name -> (w, h)
    new_data = {}  # name -> list of pixels
    for name, pixels, _start, _end in arrays:
        dims = parse_dsc_dims(text, name)
        if not dims:
            print(f"  WARN {name}: no dsc found, skipping")
            continue
        SRC_W, SRC_H = dims
        if len(pixels) != SRC_W * SRC_H:
            print(f"  WARN {name}: pixel count {len(pixels)} != {SRC_W*SRC_H}, skipping")
            continue
        x0, y0, nw, nh = crop_bbox(pixels, SRC_W, SRC_H, margin=2)
        cropped = crop_pixels(pixels, SRC_W, SRC_H, x0, y0, nw, nh)
        new_dims[name] = (nw, nh)
        new_data[name] = cropped
        print(f"  {name}: {SRC_W}x{SRC_H} -> {nw}x{nh}  (-{((SRC_W*SRC_H - nw*nh)*100)//(SRC_W*SRC_H)}%)")

    # Replace each array body, working from the end so offsets stay valid
    for name, _pixels, start, end in reversed(arrays):
        if name not in new_data:
            continue
        nw, _nh = new_dims[name]
        body = format_array(new_data[name], nw)
        replacement = (
            f"static const uint16_t {name}_data[] = {{\n{body}\n}};"
        )
        text = text[:start] + replacement + text[end:]

    # Update each lv_img_dsc_t with new dimensions and data_size
    for name, (nw, nh) in new_dims.items():
        # Match the dsc block: const lv_img_dsc_t logo_X = { ... };
        dsc_pat = re.compile(
            rf"(const lv_img_dsc_t {name} = \{{)(.*?)(\}};)",
            re.DOTALL,
        )
        m = dsc_pat.search(text)
        if not m:
            print(f"  WARN: dsc for {name} not found")
            continue
        block = m.group(2)
        # Replace .header.w
        block = re.sub(r"\.header\.w\s*=\s*\d+", f".header.w = {nw}", block)
        block = re.sub(r"\.header\.h\s*=\s*\d+", f".header.h = {nh}", block)
        block = re.sub(r"\.data_size\s*=\s*\d+", f".data_size = {nw * nh * 2}", block)
        text = text[:m.start()] + m.group(1) + block + m.group(3) + text[m.end():]

    OUT.write_text(text)
    print(f"\nWrote {OUT}")

if __name__ == "__main__":
    main()
