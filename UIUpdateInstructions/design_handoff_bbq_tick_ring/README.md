# Handoff: BBQ Segmented Tick-Ring Temperature Gauge (1c)

## Overview
Replaces the BBQ cook-view's dual concentric arc gauge (`ui_bbq.c`) with a **segmented tick ring**: instead of a smooth arc fill, discrete tick marks around the ring light up one by one as the reading approaches target. Two states, both must be supported since a grill-ambient sensor is not always assigned to a meat probe:
- **Meat only** — single tick ring (one sensor).
- **Meat + grill** — two concentric tick rings (grill outer, meat inner), one lit progressively per sensor.

## About the design files
The bundled `BBQ-Temperature-UI-Styles.dc.html` is an **HTML/CSS design reference**, built to explore and communicate the visual direction — it is not code to port line-for-line. The real target is the existing **ESP32 / ESP-IDF / LVGL 8.3 C firmware** (`ui_bbq.c` and friends). Recreate the look using LVGL widgets/primitives idiomatic to that codebase (the existing arcs, labels, images, buttons), not by embedding a web view.

Open the HTML file in a browser to see it live; option **1c** ("Segmented tick ring") is the one to implement — the other four options (1a, 1b, 1d, 1e) in that file are alternatives that were explored and NOT selected, included only for context on direction/tone.

## Fidelity
**High-fidelity.** Colors, states, and relative geometry below are final. Exact pixel radii in the HTML mock are at a 260px-diameter preview scale — Section "Geometry" below gives the formulas to re-derive them at the real 480×480 display scale, consistent with the current `ui_bbq.c` arc dimensions (so the new ring sits exactly where the old one did).

## Screens / Views

### BBQ cook view — meat only (no grill sensor assigned)
- One tick ring, same 135°→405° (270° sweep, 90° gap at bottom) as the current `s_outer_arc`/`s_inner_arc`.
- Ring centered on screen (CX=CY=240 per `ui_common.h`).
- Ticks: short rounded-rect segments arranged radially, evenly spaced around the 270° sweep. Each tick is either **lit** (bright orange, `#E87722` / `COL_ACCENT2`) or **dim** (`#2a2a2a`), with the lit count = `round(total_ticks * clamp((current - 0) / (target - 0), 0, 1))`, always starting from the 135° end (bottom-left) — same fill semantics as today's `set_arc_live`/`set_arc_no_reading`/`set_arc_alarm`, just discretized.
- Centre stack (top→bottom), all centered on CX/CY:
  1. **Connectivity badge** — small filled circle, `#2196F3` (BT_BLUE) background, centered.
  2. **Meat icon** — the existing meat-kind image (`img_meat_chicken`/`img_meat_lamb`/`img_meat_pork`/`img_meat_beef` from `img_meat_icons.c`), same asset as today's `s_meat_icon`.
  3. **Numeral readout** — `"<temp>°C"`, bold, white, large (Montserrat — this codebase already ships `lv_font_montserrat_*`; use the largest available bundled size, or generate a custom LVGL font at the size specified below via `lv_font_conv` if none fits).
  4. **Target caption** — `"target <target>°C"`, small, `#E87722`.
- **Home button** — circular outline button (transparent fill, thin `#333`/`COL_BUTTON`-ish border), `LV_SYMBOL_HOME` icon, positioned in the empty 90° bottom gap of the ring, between the centre text stack and the ring itself (i.e. below the target caption, above the bottom edge — do not overlap either).
- No "0 C"/"target C" end-of-arc labels are needed in this version — the target is already shown in the centre caption. (Flag to confirm with design owner if the 0° end label should be kept as a secondary caption near the ring's start.)

### BBQ cook view — meat + grill sensor assigned
- Two concentric tick rings: **outer = grill** (green family, `#1ED760` lit / `#123a24` dim), **inner = meat** (orange family, `#E87722` lit / `#3a2015` dim). Same 270° sweep and lit-count logic per ring, independent per sensor.
- Centre stack (top→bottom):
  1. **Two connectivity badges side by side** — same blue `#2196F3` fill, but each with a 2px border in its sensor's accent color (green border = grill, orange border = meat) so they're distinguishable at a glance.
  2. **Meat icon** — same asset/rules as the meat-only state. (Always show it once a meat probe exists, whether or not a grill sensor is also assigned.)
  3. **Grill numeral** — smaller, green (`#1ED760`), e.g. `"225°"`.
  4. **Meat numeral** — larger, orange (`#E87722`), e.g. `"24°"`.
  5. Small caption `"grill · meat"` (optional legend, dim gray) if the two colors alone aren't judged clear enough on-device.
- **Home button** — identical placement/spec to the meat-only state, in the bottom gap.
- Existing lost-connection alarm behavior (`meat_alarm`/`grill_alarm`, blinking) should map onto tick color: alternate the *lit* ticks' color between bright/dim alarm colors (`ALARM_BRIGHT`/`ALARM_DIM`) at the existing ~1Hz `s_blink_on` cadence, same trigger as `set_arc_alarm` today.

## Geometry (deriving real 480×480 coordinates)
The current `ui_bbq.c` arcs use: outer arc diameter 440, thickness 38; inner arc diameter 370, thickness 32; both centered at (240,240), sweep 135°→405°. Reuse those exact centerline radii for the new tick rings so they occupy the same footprint as today's gauge:
- `centerline_radius = diameter/2 - thickness/2`
- Meat-only single ring → use the outer arc's numbers: **radius = 220 - 19 = 201**, tick length ≈ 32, tick width ≈ 6.
- Grill (outer) ring → **radius = 201**, tick length ≈ 32, tick width ≈ 6.
- Meat (inner) ring → use inner arc's numbers: **radius = 185 - 16 = 169**, tick length ≈ 32, tick width ≈ 6.
- Tick count: pick an even spacing over 270° (e.g. 24–30 ticks per ring) — not load-bearing on the exact count, but keep it consistent across both rings' visual density.
- Each tick's angle: `angle_i = 135 + 270 * i / (total_ticks - 1)` degrees, matching the same 135°→405° convention as `lv_arc_set_bg_angles(arc, 135, 405)` elsewhere in this file.

### Suggested LVGL approach
LVGL 8.3's `lv_arc` doesn't support discrete per-tick coloring, so render each tick as its own small object (a plain `lv_obj` rectangle, or `lv_line`) placed at `(CX + radius*cos, CY + radius*sin)` for its angle, then rotated to point radially outward using `lv_obj_set_style_transform_angle`/`transform_pivot_x/y` (this codebase already uses `lv_img_set_zoom` for the meat icon, so transform styles are already in play). Precompute the per-tick (position, base-rotation) once at screen creation; each refresh tick (`refresh_timer_cb`, already running at 1Hz) only needs to update each tick's **color** based on the live lit-count and alarm state, not recreate/reposition objects.

## Interactions & Behavior
- Live refresh: reuse the existing 1Hz `s_refresh_timer` / `refresh_timer_cb` pattern — recompute lit-count and update tick colors, exactly analogous to today's `set_arc_live`/`set_arc_no_reading`/`set_arc_alarm`.
- Home button tap → `ui_navigate_to(SCREEN_MENU)`, same as today's `home_btn_cb`.
- Config/delete/add-probe/gesture-swipe behavior (`config_btn_cb`, `add_meat_btn_cb`, `go_next`/`go_prev`, etc.) is unchanged — only the gauge visualization and the home button's position are in scope for this handoff. **Open question for design owner:** the current screen also has a config-gear button (`s_config_btn`) next to home at the bottom — this handoff doesn't specify a new position for it since only the home button was discussed; keep it functionally available (e.g. same bottom-gap area, alongside home) until confirmed otherwise.
- No-reading / alarm states: same semantics as today (`v.meat_present`, `meat_alarm`, etc.) — dim-vs-lit tick coloring substitutes for the arc's pale-vs-bright fill.

## State Management
No new state needed beyond what `bbq_controller.*`/`bbq_view_t` already expose (`meat_temp_c`, `meat_target_c`, `meat_present`, `meat_alarm`, `grill_*` equivalents, `meat_kind`). This is a rendering change only.

## Design Tokens
| Token | Value | Use |
|---|---|---|
| Meat lit | `#E87722` | lit ticks, meat numeral, target caption (matches existing `COL_ACCENT2`) |
| Meat dim | `#3a2015` | unlit meat ticks |
| Grill lit | `#1ED760` | lit ticks, grill numeral (matches existing `COL_ACCENT` / `GRILL_BRIGHT`) |
| Grill dim | `#123a24` | unlit grill ticks |
| Connectivity badge fill | `#2196F3` | matches existing `BT_BLUE` |
| Alarm bright / dim | existing `ALARM_BRIGHT` (`#FF4500`) / `ALARM_DIM` (`#401810`) | blinking lit-tick color when a sensor's connection is lost |
| Background | `#000000` | matches `COL_BG` |
| Text (numerals) | `#FFFFFF` | matches `COL_TEXT` |
| Home button border | `#333333`-ish (matches `COL_BUTTON` `#2A2A2A`) | |
| Ring sweep | 135°→405° (270°, 90° gap at bottom) | matches existing `lv_arc_set_bg_angles` calls |
| Font | Montserrat (bundled `lv_font_montserrat_*`) | matches rest of app; no new font family |

## Assets
- Meat-kind icons: already in the repo at `assets/Meat Icons/*.png` → compiled into `src/img_meat_icons.c` via `scripts/gen_meat_icons.py`. No new assets needed — reuse `img_meat_chicken`/`img_meat_lamb`/`img_meat_pork`/`img_meat_beef`.
- `LV_SYMBOL_HOME` — built-in LVGL symbol font glyph, already used elsewhere in this file (`home_btn_cb`'s icon).
- No new image/icon assets are introduced by this design.

## Files
- **Design reference:** `BBQ-Temperature-UI-Styles.dc.html` (this bundle) — see option **1c**, both states ("MEAT ONLY" / "+ GRILL SENSOR" cards).
- **Firmware file to modify:** `src/ui_bbq.c` (+ `src/ui_bbq.h` if any new public symbols are needed) — this is where `ui_bbq_create()`, `show_index()`, and the arc-gauge logic being replaced currently live.
- **Reference only, no changes expected:**
  - `src/ui_common.h` — color palette (`COL_*`), screen geometry (`CX`/`CY`/`CR`/`SAFE_R`), `screen_id_t`.
  - `src/bbq_controller.h` — `bbq_view_t`, sensor/model accessors this screen reads.
  - `src/img_meat_icons.h`/`.c` — meat icon image descriptors.
