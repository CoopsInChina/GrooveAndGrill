#!/usr/bin/env python3
"""Generates src/meat_temps.h from data/meat_temps.json.

Run this manually after editing the JSON:
    python3 scripts/gen_meat_temps.py
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "data", "meat_temps.json")
DST  = os.path.join(ROOT, "src", "meat_temps.h")

with open(SRC) as f:
    data = json.load(f)

meats = data["meats"]

lines = []
lines.append("#pragma once")
lines.append("")
lines.append("// AUTO-GENERATED from data/meat_temps.json — do not hand-edit.")
lines.append("// Regenerate with: python3 scripts/gen_meat_temps.py")
lines.append("")
lines.append("typedef struct {")
lines.append("    const char *label;")
lines.append("    int         target_c;")
lines.append("} meat_level_t;")
lines.append("")
lines.append("typedef struct {")
lines.append("    const char         *name;")
lines.append("    const meat_level_t *levels;")
lines.append("    int                 level_count;")
lines.append("} meat_type_t;")
lines.append("")

array_names = {}
for meat_name in meats:
    arr_name = f"{meat_name.upper()}_LEVELS"
    array_names[meat_name] = arr_name
    levels = meats[meat_name]["levels"]
    lines.append(f"static const meat_level_t {arr_name}[] = {{")
    for lvl in levels:
        label = lvl["label"].replace('"', '\\"')
        lines.append(f'    {{ "{label}", {lvl["target_c"]} }},')
    lines.append("};")
    lines.append("")

lines.append("static const meat_type_t MEAT_TYPES[] = {")
for meat_name in meats:
    arr_name = array_names[meat_name]
    count = len(meats[meat_name]["levels"])
    display_name = meat_name.capitalize()
    lines.append(f'    {{ "{display_name}", {arr_name}, {count} }},')
lines.append("};")
lines.append(f"#define MEAT_TYPE_COUNT {len(meats)}")
lines.append("")
lines.append("// Poultry has a single food-safety target rather than a doneness")
lines.append("// preference, so it isn't part of the table above.")
lines.append("#define CHICKEN_SAFE_TARGET_C 74")
lines.append("")

with open(DST, "w") as f:
    f.write("\n".join(lines))

print(f"Wrote {DST}")
