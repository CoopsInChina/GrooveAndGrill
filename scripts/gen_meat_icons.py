#!/usr/bin/env python3
"""Generates src/img_meat_icons.c from the PNGs in assets/Meat Icons/.

The source art is white line-work on solid black. We store it as LVGL
TRUE_COLOR_ALPHA with alpha = pixel brightness, so the black background
becomes transparent and only the lines show — this lets the same icon sit
cleanly on the black grill screen AND on the dark-grey config buttons.

Run manually after changing the art:
    python3 scripts/gen_meat_icons.py
"""
from PIL import Image
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "assets", "Meat Icons")
DST  = os.path.join(ROOT, "src", "img_meat_icons.c")

SIZE = 120

ICONS = {
    "img_meat_chicken": "chicken.png",
    "img_meat_lamb":    "lamb.png",
    "img_meat_pork":    "pork.png",
    "img_meat_beef":    "beef.png",
}


def to_lvgl_array(pil_img):
    img = pil_img.convert("RGB")
    if img.size != (SIZE, SIZE):
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
    px = img.load()
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = px[x, y]
            # Line art on black: alpha follows brightness so black -> clear,
            # white -> opaque, with antialiased edges preserved.
            a = max(r, g, b)
            color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out += bytes([color & 0xFF, (color >> 8) & 0xFF, a])
    return out


lines = ['#include "lvgl.h"', ""]
for name, fname in ICONS.items():
    data = to_lvgl_array(Image.open(os.path.join(SRC, fname)))
    lines.append(f"static const uint8_t {name}_map[] = {{")
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_img_dsc_t {name} = {{")
    lines.append("    .header.always_zero = 0,")
    lines.append(f"    .header.w = {SIZE},")
    lines.append(f"    .header.h = {SIZE},")
    lines.append(f"    .data_size = {len(data)},")
    lines.append("    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,")
    lines.append(f"    .data = {name}_map,")
    lines.append("};")
    lines.append("")

with open(DST, "w") as f:
    f.write("\n".join(lines))

print(f"Wrote {DST}")
print(f"Icons: {', '.join(ICONS.keys())} @ {SIZE}x{SIZE}")
