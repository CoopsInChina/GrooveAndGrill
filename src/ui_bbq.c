#include "ui_bbq.h"
#include "ui_common.h"
#include "ui_bbq_config.h"
#include "ui_bbq_add.h"
#include "bbq_controller.h"
#include "board_config.h"
#include "img_meat_icons.h"
#include "lvgl.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Real horizontal scroll-snap carousel — see ui_favourites.c (the original,
// most heavily field-tested version of this architecture) for the full
// reasoning behind each of these flags/handlers: absolute page positioning
// rather than flex, pages non-clickable/non-scrollable so the container
// resolves as the touch target, SCROLL_ELASTIC off + press/release edge
// detection rather than LVGL's own GESTURE system (which structurally
// can't fire at a scrollable boundary), and a scroll_end_cb guard against
// re-entrant lv_obj_scroll_to_view calls. Exits at both edges (like
// Favourites), not wraps (like Settings) — matches this screen's previous
// go_next/go_prev behaviour.
//
// Unlike Favourites (one image per page) or Settings (six fixed, mostly-
// static pages), each BBQ page carries a full, fairly heavy widget set
// (a custom-drawn tick-ring gauge, several labels, icons, a BT button)
// that needs live 1Hz updates — but the realistic page count here is
// small and bounded (MAX_GRILLS=4, or MAX_DIRECT_PROBES=2, plus one
// add-slot), so refreshing every page's content unconditionally each
// tick — not just the visible one — is cheap enough not to need the
// scoping Favourites' art pipeline needed.

// Grill temp (outer ring) and meat temp (inner ring) semantic colours —
// design tokens from UIUpdateInstructions/design_handoff_bbq_tick_ring
// (segmented tick-ring gauge, option 1c). Bright = lit tick (reached),
// pale (dim) = unlit tick (remaining headroom to target).
static const lv_color_t GRILL_BRIGHT = LV_COLOR_MAKE(0x1E, 0xD7, 0x60);
static const lv_color_t GRILL_PALE   = LV_COLOR_MAKE(0x12, 0x3A, 0x24);
static const lv_color_t MEAT_BRIGHT  = LV_COLOR_MAKE(0xE8, 0x77, 0x22);
static const lv_color_t MEAT_PALE    = LV_COLOR_MAKE(0x3A, 0x20, 0x15);
static const lv_color_t BT_BLUE      = LV_COLOR_MAKE(0x21, 0x96, 0xF3);
static const lv_color_t ALARM_BRIGHT = LV_COLOR_MAKE(0xFF, 0x45, 0x00);
static const lv_color_t ALARM_DIM    = LV_COLOR_MAKE(0x40, 0x18, 0x10);

// Segmented tick-ring geometry.
#define RING_TICK_COUNT  26
#define RING_ANGLE_START 135   // matches lv_arc_set_bg_angles(arc, 135, 405) elsewhere in this file
#define RING_ANGLE_SWEEP 270
#define RING_TICK_W      6

// Dual-ring (grill + meat both assigned) geometry — meat is always the
// OUTER ring, grill the inner one (fixed roles, not "whichever is outer in
// the model"). Pushed out close to the physical bezel edge (CR=240 in
// ui_common.h) rather than the old SAFE_R-based footprint, which still
// left a visible gap on real hardware; a real dark gap separates the two
// rings so they read as visually distinct, not abutting/merged.
#define RING_TICK_LEN    32
#define RING_OUTER_R     226   // meat ring (outer)
#define RING_GAP         16    // dark margin between the meat and grill rings
#define RING_INNER_R     (RING_OUTER_R - RING_TICK_LEN - RING_GAP)   // grill ring (inner) = 178

// Solo-ring (only one of grill/meat assigned) geometry — pushed even
// further out, since there's no second ring competing for room.
#define RING_SOLO_LEN    36
#define RING_SOLO_R      230

// Meat icon size (lv_img_set_zoom, 256 = 100%) — smaller in the dual
// (grill+meat) view, which has a second readout row to fit; full-size in
// the solo meat-only view, which has the room to spare.
#define ICON_ZOOM_SOLO   246   // ~96% of native 120px (20% smaller than the previous 307)
#define ICON_ZOOM_DUAL   176   // ~69% of native (20% smaller than the previous 220)

#define MAX_BBQ_PAGES (MAX_GRILLS + 1)   // MAX_GRILLS(4) covers box mode; probe mode's 2 fits easily too

typedef struct {
    lv_obj_t *page;
    bool      is_add;   // true only for the trailing "Add Meat" slot, if present

    // ---- view content (hidden on the add-slot page) ----
    lv_obj_t *title_lbl;
    lv_obj_t *ring_obj;       // custom-drawn segmented tick rings (meat outer + grill inner)
    // Numeral stack, no "Meat:"/"Grill:" prefixes — matches the design
    // handoff's reference graphic. val_primary is the big bold number
    // (the solo sensor's reading, or meat's reading when dual); val_secondary
    // is the smaller number above it, only shown in the dual view (grill's
    // reading); caption_lbl is the small line underneath ("target N C" when
    // solo, "grill / meat" legend when dual).
    lv_obj_t *val_primary, *val_secondary, *caption_lbl;
    lv_obj_t *meat_icon;
    lv_obj_t *alarm_icon;
    lv_obj_t *probe_icon;     // BT status dot (mock connect/disconnect)

    // ---- tick-ring state — computed in refresh_page(), consumed by
    // ring_draw_event_cb() at actual draw time (see file banner: the ring
    // itself isn't recreated/repositioned per refresh, only recolored). ----
    bool grill_shown, grill_alarm;
    int  grill_lit;
    bool meat_shown, meat_alarm;
    int  meat_lit;

    // ---- add-slot content (only built on the add-slot page) ----
    lv_obj_t *add_lbl;
    lv_obj_t *add_btn;
} bbq_page_t;

static lv_obj_t   *s_scr        = NULL;
static lv_obj_t   *s_container  = NULL;
static lv_obj_t   *s_config_btn = NULL;   // shared fixed overlay — configures whichever page is current
static lv_obj_t   *s_home_btn   = NULL;

static bbq_page_t s_pages[MAX_BBQ_PAGES];
static int         s_page_count   = -1;   // -1 = never built yet
static int         s_index        = 0;
static bool         s_gesture_fired = false;
static bool         s_blink_on      = false;   // alarm flash phase, toggled ~1Hz

static lv_timer_t *s_refresh_timer = NULL;

static void build_pages(void);
static void show_index_impl(int idx, bool scroll_to);

void ui_bbq_set_index(int idx)
{
    if (idx >= 0) s_index = idx;
}

// ---- Layout helper: same widget set every view page gets, at a fixed
// position relative to its own page (index only affects the page's own
// x — every child inside is aligned relative to that, so this is just
// the original ui_bbq_create() body turned into a per-page builder) ----

static const lv_img_dsc_t *meat_icon_for(meat_kind_t kind)
{
    switch (kind) {
        case MEAT_KIND_CHICKEN: return &img_meat_chicken;
        case MEAT_KIND_LAMB:    return &img_meat_lamb;
        case MEAT_KIND_PORK:    return &img_meat_pork;
        case MEAT_KIND_BEEF:    return &img_meat_beef;
        default:                return NULL;
    }
}

// How many of RING_TICK_COUNT ticks are "lit" for a current/target pair —
// the discretized equivalent of the old arcs' 0..target value fill.
static int lit_count_for(int target, float current)
{
    int max = target > 0 ? target : 1;
    float frac = current / (float)max;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int n = (int)(RING_TICK_COUNT * frac + 0.5f);
    if (n < 0) n = 0;
    if (n > RING_TICK_COUNT) n = RING_TICK_COUNT;
    return n;
}

// Draws one ring's worth of radial tick marks directly via lv_draw_line —
// see the file banner and the handoff README's "Suggested LVGL approach":
// one lv_obj per tick was computed to risk exhausting the LVGL memory pool
// (~80 bytes/tick, ~21.6KB for the full screen set vs ~14KB known headroom),
// and a raster lv_canvas large enough for this ring is far worse (~378KB
// for a single 440x440 RGB565 buffer) — so ticks are recomputed and drawn
// fresh every redraw instead, the same pattern LVGL's own lv_meter widget
// uses for its scale ticks (lv_meter.c's draw_ticks_and_labels).
static void draw_tick_ring(lv_draw_ctx_t *draw_ctx, lv_point_t center, lv_coord_t radius,
                            lv_coord_t tick_len, lv_color_t lit_color, lv_color_t dim_color,
                            int lit_count)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width = RING_TICK_W;
    dsc.round_start = 1;
    dsc.round_end = 1;

    for (int i = 0; i < RING_TICK_COUNT; i++) {
        // 1/10-degree units + lv_point_transform, exactly as lv_meter.c
        // rotates its own tick endpoints around the scale's center point.
        int32_t angle = (RING_ANGLE_START * 10) +
                         (int32_t)(((int64_t)RING_ANGLE_SWEEP * 10 * i) / (RING_TICK_COUNT - 1));
        lv_point_t p1 = { (lv_coord_t)(center.x + radius - tick_len), center.y };
        lv_point_t p2 = { (lv_coord_t)(center.x + radius), center.y };
        lv_point_transform(&p1, angle, 256, &center);
        lv_point_transform(&p2, angle, 256, &center);
        dsc.color = (i < lit_count) ? lit_color : dim_color;
        lv_draw_line(draw_ctx, &dsc, &p1, &p2);
    }
}

// LV_EVENT_DRAW_POST on a plain, empty lv_obj_t (pg->ring_obj) — draws the
// grill (outer) and/or meat (inner/solo) tick rings for one page. Reads
// state precomputed in refresh_page(), not the model directly, so this can
// fire on any LVGL-internal redraw without touching bbq_controller.
//
// Center is read from the object's own current on-screen coords rather
// than the fixed CX/CY — pg->ring_obj is a normal child of the scrolling
// page, so its coords already include the container's live scroll offset
// (same as the title/icon/labels, which are plain lv_obj children and so
// track the swipe for free). Hardcoding CX/CY here previously pinned the
// ticks to the true screen center regardless of scroll, so the ring didn't
// move with its page during a swipe even though everything else on it did.
static void ring_draw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_POST) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_BBQ_PAGES) return;
    bbq_page_t *pg = &s_pages[idx];
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);

    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target(e), &area);
    lv_point_t center = { (lv_coord_t)((area.x1 + area.x2) / 2),
                           (lv_coord_t)((area.y1 + area.y2) / 2) };

    // Meat is always the outer ring, grill (when also assigned) the inner
    // one. Either ring gets the bigger "solo" footprint when it's the only
    // one shown (including the grill-only case, which has no meat ring to
    // defer to).
    bool dual = pg->grill_shown && pg->meat_shown;

    if (pg->meat_shown) {
        lv_coord_t r   = dual ? RING_OUTER_R  : RING_SOLO_R;
        lv_coord_t len = dual ? RING_TICK_LEN : RING_SOLO_LEN;
        lv_color_t dim = pg->meat_alarm ? ALARM_DIM : MEAT_PALE;
        lv_color_t lit = pg->meat_alarm ? (s_blink_on ? ALARM_BRIGHT : ALARM_DIM) : MEAT_BRIGHT;
        draw_tick_ring(draw_ctx, center, r, len, lit, dim, pg->meat_lit);
    }
    if (pg->grill_shown) {
        lv_coord_t r   = dual ? RING_INNER_R  : RING_SOLO_R;
        lv_coord_t len = dual ? RING_TICK_LEN : RING_SOLO_LEN;
        lv_color_t dim = pg->grill_alarm ? ALARM_DIM : GRILL_PALE;
        lv_color_t lit = pg->grill_alarm ? (s_blink_on ? ALARM_BRIGHT : ALARM_DIM) : GRILL_BRIGHT;
        draw_tick_ring(draw_ctx, center, r, len, lit, dim, pg->grill_lit);
    }
}

// ---- Button callbacks --------------------------------------------------
//
// A touch that turns into a swipe fires a real scroll on this screen now
// (not LVGL GESTURE events — see the file banner comment), but a swipe
// starting on a button still fires LV_EVENT_CLICKED for that same touch.
// s_gesture_fired (set from scroll_begin_cb) suppresses that stray click;
// it auto-clears shortly after so a later, genuinely separate tap works.

static void probe_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bbq_mock_toggle_probe(idx);
}

static void config_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    bbq_view_t v;
    if (!bbq_view_at(s_index, &v)) return;
    bbq_setup_t s = { .grill_num = v.grill_num };
    if (v.has_meat)             { s.src = v.meat_src;  s.hw_id = v.meat_hw_id;  s.role = ROLE_MEAT;  }
    else if (v.grill_assigned)  { s.src = v.grill_src; s.hw_id = v.grill_hw_id; s.role = ROLE_GRILL; }
    else return;
    bbq_setup_set(&s);
    ui_navigate_to(SCREEN_BBQ_CONFIG);
    ui_bbq_config_begin();
}

static void home_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    ui_navigate_to(SCREEN_MENU);
}

static void add_meat_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    ui_navigate_to(SCREEN_BBQ_ADD);   // grill# -> sensor -> type wizard
}

// ---- Scroll settle / edge-exit -----------------------------------------

static void go_exit(void)
{
    s_gesture_fired = true;
    ui_navigate_to(SCREEN_MENU);
}

static void gesture_cb(lv_event_t *e)
{
    // Fallback for the rare case s_container has zero scrollable room in
    // EITHER direction (a single page, no add-slot) — see
    // ui_favourites.c's gesture_cb comment for why this normally can't
    // fire at a scrollable boundary otherwise.
    ui_handle_gesture(go_exit, go_exit, NULL, NULL);
}

static void scroll_end_cb(lv_event_t *e)
{
    if (lv_scr_act() != s_scr) return;   // stale/late event after navigating away
    if (s_page_count <= 0) return;
    lv_coord_t w = lv_obj_get_width(s_container);
    if (w <= 0) return;

    // Same empirically-derived sign convention as every other carousel in
    // this app (identical container/hardware setup) — x grows positive
    // scrolling forward here, not negative as lv_obj_get_scroll_x's usual
    // convention would suggest.
    lv_coord_t x = lv_obj_get_scroll_x(s_container);
    int p = (int)((x + w / 2) / w);
    if (p < 0) p = 0;
    if (p >= s_page_count) p = s_page_count - 1;

    show_index_impl(p, false);
}

static void scroll_begin_cb(lv_event_t *e)
{
    s_gesture_fired = true;
}

#define EDGE_EXIT_SWIPE_PX 80
static lv_coord_t s_press_x = 0;

static void container_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    s_press_x = pt.x;
}

static void container_released_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_coord_t dx = pt.x - s_press_x;

    if (s_index == 0 && dx > EDGE_EXIT_SWIPE_PX) {
        go_exit();
    } else if (s_page_count > 0 && s_index == s_page_count - 1 && dx < -EDGE_EXIT_SWIPE_PX) {
        go_exit();
    }
}

// ---- Live refresh -------------------------------------------------------
// Re-reads the model each second so gauges/readouts track incoming BLE
// temperatures — every page, not just the current one (see file banner:
// cheap enough given the small bounded page count here, unlike
// Favourites' art decodes).

static void refresh_page(bbq_page_t *pg, int idx)
{
    if (pg->is_add) return;

    bbq_view_t v;
    if (!bbq_view_at(idx, &v)) return;

    bool probe_mode = (bbq_source_get() == BBQ_SRC_PROBE);

    if (pg->title_lbl) {
        char buf[32];
        // Wireless-probe-only mode has no grill grouping — title by the
        // probe's bonded slot (stable identity, not view position) so the
        // same physical probe always reads "Probe 1"/"Probe 2".
        if (probe_mode) {
            int slot = bbq_probe_slot_of(v.meat_hw_id);
            snprintf(buf, sizeof(buf), "Probe %d", slot >= 0 ? slot + 1 : idx + 1);
        } else {
            snprintf(buf, sizeof(buf), "Grill %d", v.grill_num);
        }
        lv_label_set_text(pg->title_lbl, buf);
    }

    // BT dot: in probe mode, reflects THIS view's own probe link (each meat
    // has its own independent GATT connection) rather than "is any probe
    // connected" — otherwise probe A's icon stayed blue after it was turned
    // off, as long as probe B was still linked. Box mode still uses the
    // shared gateway link, since one box feeds every sensor on screen.
    if (pg->probe_icon) {
        bool link = probe_mode ? v.meat_present : bbq_link_up();
        lv_obj_set_style_text_color(pg->probe_icon, link ? BT_BLUE : COL_TEXT_DIM, 0);
    }

    bool meat_alarm  = v.has_meat && v.meat_alarm;
    bool grill_alarm = v.grill_assigned && v.grill_alarm;

    // Meat icon — only on a meat view; swapped for the alarm icon when that
    // probe has lost connection after being live. Smaller in the dual
    // (grill+meat) view, where it shares the centre stack with a second
    // readout row, than in the solo meat-only view.
    if (pg->meat_icon) {
        const lv_img_dsc_t *icon = (v.has_meat && !meat_alarm) ? meat_icon_for(v.meat_kind) : NULL;
        if (icon) {
            lv_img_set_src(pg->meat_icon, icon);
            lv_img_set_zoom(pg->meat_icon, v.grill_assigned ? ICON_ZOOM_DUAL : ICON_ZOOM_SOLO);
            lv_obj_clear_flag(pg->meat_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pg->meat_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (pg->alarm_icon) {
        if (meat_alarm) lv_obj_clear_flag(pg->alarm_icon, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag(pg->alarm_icon,   LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Numeral stack: no "Meat:"/"Grill:" name prefixes — matches the
    // design handoff's reference graphic. Meat is always the big bold
    // "primary" number (matches it always being the outer ring); grill,
    // when also assigned, is the smaller "secondary" number above it.
    // Each number is dim "- C" until its sensor reports, then bright in
    // its ring's colour — meat stays orange in both layouts, never white.
    bool dual = v.has_meat && v.grill_assigned;

    if (pg->val_secondary) {
        if (dual) {
            char b[12];
            if (v.grill_present || grill_alarm) snprintf(b, sizeof(b), "%d C", (int)v.grill_temp_c);
            else                                snprintf(b, sizeof(b), "- C");
            lv_label_set_text(pg->val_secondary, b);
            lv_color_t col = v.grill_present ? GRILL_BRIGHT : (grill_alarm ? ALARM_BRIGHT : COL_TEXT_DIM);
            lv_obj_set_style_text_color(pg->val_secondary, col, 0);
            lv_obj_clear_flag(pg->val_secondary, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pg->val_secondary, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (pg->val_primary) {
        // Grill-only (no meat probe on this slot) falls back to showing
        // grill's own reading in the primary spot, since meat's is absent.
        bool primary_is_meat = v.has_meat;
        bool show_primary    = v.has_meat || v.grill_assigned;
        if (show_primary) {
            char b[12];
            bool present = primary_is_meat ? (v.meat_present || meat_alarm) : (v.grill_present || grill_alarm);
            if (present) snprintf(b, sizeof(b), "%d C", (int)(primary_is_meat ? v.meat_temp_c : v.grill_temp_c));
            else         snprintf(b, sizeof(b), "- C");
            lv_label_set_text(pg->val_primary, b);
            lv_color_t bright = primary_is_meat ? MEAT_BRIGHT : GRILL_BRIGHT;
            bool alarm        = primary_is_meat ? meat_alarm  : grill_alarm;
            bool live         = primary_is_meat ? v.meat_present : v.grill_present;
            lv_color_t col = live ? bright : (alarm ? ALARM_BRIGHT : COL_TEXT_DIM);
            lv_obj_set_style_text_color(pg->val_primary, col, 0);
            lv_obj_clear_flag(pg->val_primary, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pg->val_primary, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align(pg->val_primary, LV_ALIGN_CENTER, 0, dual ? 88 : 55);
    }

    // Target caption — always shows the primary readout's target (meat's,
    // or grill's when there's no meat probe on this slot), even in the
    // dual view, which previously showed a "grill / meat" legend instead.
    if (pg->caption_lbl) {
        if (v.has_meat) {
            char b[24];
            snprintf(b, sizeof(b), "Target %d C", v.meat_target_c);
            lv_label_set_text(pg->caption_lbl, b);
            lv_obj_set_style_text_color(pg->caption_lbl, MEAT_BRIGHT, 0);
        } else if (v.grill_assigned) {
            char b[24];
            snprintf(b, sizeof(b), "Target %d C", v.grill_target_c);
            lv_label_set_text(pg->caption_lbl, b);
            lv_obj_set_style_text_color(pg->caption_lbl, GRILL_BRIGHT, 0);
        } else {
            lv_label_set_text(pg->caption_lbl, "");
        }
        lv_obj_align(pg->caption_lbl, LV_ALIGN_CENTER, 0, dual ? 128 : 100);
    }

    // ---- Gauge fill (segmented tick rings — actual drawing happens in
    // ring_draw_event_cb; this just recomputes the state it reads). Meat
    // is always the outer ring, grill the inner one (see ring_draw_event_cb).
    // Lit count uses the last-known reading during an alarm too, same as
    // the old set_arc_alarm's fill-to-last-known.
    pg->grill_shown = v.grill_assigned;
    pg->grill_alarm = grill_alarm;
    pg->grill_lit   = (v.grill_assigned && (grill_alarm || v.grill_present))
                       ? lit_count_for(v.grill_target_c, v.grill_temp_c) : 0;
    pg->meat_shown = v.has_meat;
    pg->meat_alarm = meat_alarm;
    pg->meat_lit    = (v.has_meat && (meat_alarm || v.meat_present))
                       ? lit_count_for(v.meat_target_c, v.meat_temp_c) : 0;
    if (pg->ring_obj) lv_obj_invalidate(pg->ring_obj);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_blink_on = !s_blink_on;
    if (!s_scr || lv_scr_act() != s_scr) return;

    int count = bbq_view_count();
    for (int i = 0; i < s_page_count && i < count; i++) {
        refresh_page(&s_pages[i], i);
    }
}

// ---- Page building -------------------------------------------------
// Rebuilds the scrolling row's pages to match the current view + add-slot
// count. Called whenever that count changes (sensors added/removed/mode
// switched) — mirrors ui_favourites.c's build_pages.

static void build_view_page(bbq_page_t *pg, lv_obj_t *page)
{
    // ---- Segmented tick-ring gauge: meat (outer, or solo) + grill (inner,
    // when also assigned) — see ring_draw_event_cb for the actual per-tick
    // drawing; this is just an empty, non-interactive canvas-sized object
    // for that draw callback to hang off. Registered in build_pages() once
    // this page's index is known (same pattern as probe_btn_cb below). No
    // end-of-arc labels in this design (dropped per design-owner decision —
    // the centre caption already shows target).
    pg->ring_obj = lv_obj_create(page);
    lv_obj_remove_style_all(pg->ring_obj);
    lv_obj_set_size(pg->ring_obj, 440, 440);
    lv_obj_center(pg->ring_obj);
    lv_obj_clear_flag(pg->ring_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ---- Title ----
    pg->title_lbl = lv_label_create(page);
    lv_obj_set_style_text_color(pg->title_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(pg->title_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(pg->title_lbl, LV_ALIGN_CENTER, 0, -114);
    lv_obj_clear_flag(pg->title_lbl, LV_OBJ_FLAG_CLICKABLE);

    // ---- Meat icon (reused from the Grill Config meat picker) — central,
    // sized in refresh_page() per ICON_ZOOM_SOLO/DUAL (view-dependent). ----
    pg->meat_icon = lv_img_create(page);
    lv_obj_align(pg->meat_icon, LV_ALIGN_CENTER, 0, -37);
    lv_obj_clear_flag(pg->meat_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pg->meat_icon, LV_OBJ_FLAG_HIDDEN);

    // ---- Numeral stack — below the icon, no name prefixes (matches the
    // design handoff's reference graphic): a small number above (grill,
    // dual view only) and a big bold number below (meat — or grill, in a
    // grill-only view with no meat probe), plus a small caption underneath.
    // Actual text/color/position is set live in refresh_page(), since it
    // depends on whether this view is solo or dual.
    pg->val_secondary = lv_label_create(page);
    lv_obj_set_style_text_align(pg->val_secondary, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(pg->val_secondary, &lv_font_montserrat_28, 0);
    lv_obj_align(pg->val_secondary, LV_ALIGN_CENTER, 0, 44);
    lv_obj_clear_flag(pg->val_secondary, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pg->val_secondary, LV_OBJ_FLAG_HIDDEN);

    pg->val_primary = lv_label_create(page);
    lv_obj_set_style_text_align(pg->val_primary, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(pg->val_primary, &lv_font_montserrat_48, 0);
    lv_obj_align(pg->val_primary, LV_ALIGN_CENTER, 0, 55);
    lv_obj_clear_flag(pg->val_primary, LV_OBJ_FLAG_CLICKABLE);

    pg->caption_lbl = lv_label_create(page);
    lv_obj_set_style_text_align(pg->caption_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(pg->caption_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(pg->caption_lbl, LV_ALIGN_CENTER, 0, 100);
    lv_obj_clear_flag(pg->caption_lbl, LV_OBJ_FLAG_CLICKABLE);

    // ---- Alarm icon (probe disconnected — flashes) ----
    // Replaces the meat icon in the same spot when that sensor has lost
    // connection after being live.
    pg->alarm_icon = lv_label_create(page);
    lv_label_set_text(pg->alarm_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(pg->alarm_icon, ALARM_BRIGHT, 0);
    lv_obj_set_style_text_font(pg->alarm_icon, &lv_font_montserrat_48, 0);
    lv_obj_align(pg->alarm_icon, LV_ALIGN_CENTER, 0, -37);
    lv_obj_clear_flag(pg->alarm_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pg->alarm_icon, LV_OBJ_FLAG_HIDDEN);

    // ---- Probe status toggle (mock connect/disconnect) — 50% bigger icon
    // (montserrat_20 -> 30), moved down to clear the numeral stack above it. ----
    lv_obj_t *probe_btn = lv_btn_create(page);
    lv_obj_set_size(probe_btn, 64, 64);
    lv_obj_align(probe_btn, LV_ALIGN_CENTER, 0, 165);
    lv_obj_set_style_radius(probe_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(probe_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(probe_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(probe_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(probe_btn, 0, 0);
    lv_obj_set_style_border_width(probe_btn, 0, 0);

    pg->probe_icon = lv_label_create(probe_btn);
    lv_label_set_text(pg->probe_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(pg->probe_icon, &lv_font_montserrat_30, 0);
    lv_obj_center(pg->probe_icon);
    // probe_btn_cb needs to know which page's index it's toggling — set
    // once the page's real index is known, in build_pages().
}

static void build_add_page(bbq_page_t *pg, lv_obj_t *page)
{
    pg->add_lbl = lv_label_create(page);
    lv_label_set_text(pg->add_lbl, "Add Meat");
    lv_obj_set_style_text_color(pg->add_lbl, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(pg->add_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(pg->add_lbl, LV_ALIGN_CENTER, 0, -100);
    lv_obj_clear_flag(pg->add_lbl, LV_OBJ_FLAG_CLICKABLE);

    pg->add_btn = lv_btn_create(page);
    lv_obj_set_size(pg->add_btn, 120, 120);
    lv_obj_align(pg->add_btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(pg->add_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pg->add_btn, COL_PANEL, 0);
    lv_obj_set_style_border_color(pg->add_btn, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(pg->add_btn, 2, 0);
    lv_obj_set_style_shadow_width(pg->add_btn, 0, 0);
    lv_obj_add_event_cb(pg->add_btn, add_meat_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_icon = lv_label_create(pg->add_btn);
    lv_label_set_text(add_icon, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(add_icon, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(add_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(add_icon);
}

static void build_pages(void)
{
    lv_obj_clean(s_container);
    memset(s_pages, 0, sizeof(s_pages));

    int count    = bbq_view_count();
    bool add_ok  = bbq_can_add_more();
    int total    = count + (add_ok ? 1 : 0);
    if (total > MAX_BBQ_PAGES) total = MAX_BBQ_PAGES;
    if (total < 1) total = 1;   // always at least the add-slot, even with nothing configured
    s_page_count = total;

    for (int i = 0; i < total; i++) {
        lv_obj_t *page = lv_obj_create(s_container);
        // Absolute position/size, not flex — see file banner comment.
        lv_obj_set_pos(page, i * LCD_H_RES, 0);
        lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        bbq_page_t *pg = &s_pages[i];
        pg->page = page;

        bool is_add = (i == count);   // only true for the slot beyond every real view
        pg->is_add = is_add;

        if (is_add) {
            build_add_page(pg, page);
        } else {
            build_view_page(pg, page);
            lv_obj_t *probe_btn = lv_obj_get_parent(pg->probe_icon);
            lv_obj_add_event_cb(probe_btn, probe_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(pg->ring_obj, ring_draw_event_cb, LV_EVENT_DRAW_POST, (void *)(intptr_t)i);
            refresh_page(pg, i);
        }
    }
}

// ---- Show ----------------------------------------------------------

static void show_index_impl(int idx, bool scroll_to)
{
    if (s_page_count <= 0) return;
    if (idx >= s_page_count) idx = s_page_count - 1;
    if (idx < 0) idx = 0;
    s_index = idx;
    if (!s_scr) return;

    if (scroll_to && s_pages[idx].page) {
        lv_obj_scroll_to_view(s_pages[idx].page, LV_ANIM_OFF);
    }

    // config_btn only makes sense on a real view, not the add-slot.
    if (s_config_btn) {
        if (s_pages[idx].is_add) lv_obj_add_flag(s_config_btn, LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_clear_flag(s_config_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- Screen lifecycle --------------------------------------------------

static void scr_loaded_cb(lv_event_t *e)
{
    int count   = bbq_view_count();
    bool add_ok = bbq_can_add_more();
    int total   = count + (add_ok ? 1 : 0);
    if (total < 1) total = 1;

    // Rebuild whenever the view/add-slot count has actually changed since
    // last time (sensors added/removed, source mode switched) — s_page_count
    // starts at -1, so the very first load always builds.
    if (total != s_page_count) {
        build_pages();
    }
    show_index_impl(s_index, true);
}

static void scr_del_cb(lv_event_t *e)
{
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    s_scr = s_container = s_config_btn = s_home_btn = NULL;
    s_page_count = -1;
    memset(s_pages, 0, sizeof(s_pages));
}

// ---- Create -------------------------------------------------------------

lv_obj_t *ui_bbq_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,    LV_EVENT_GESTURE,       NULL);
    lv_obj_add_event_cb(s_scr, scr_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,        NULL);

    s_container = lv_obj_create(s_scr);
    lv_obj_set_size(s_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_set_scrollbar_mode(s_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_container, LV_SCROLL_SNAP_CENTER);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_add_event_cb(s_container, scroll_end_cb,   LV_EVENT_SCROLL_END,   NULL);
    lv_obj_add_event_cb(s_container, scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_container, container_pressed_cb,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_container, container_released_cb, LV_EVENT_RELEASED, NULL);

    // ---- Config + Home row (bottom gap of the arcs) — shared fixed
    // overlays, not part of the scrolling content: config_btn always acts
    // on whichever page is current (s_index), and home behaves the same
    // regardless of page, so there's no need to duplicate either per page.
    s_config_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_config_btn, 50, 50);
    lv_obj_align(s_config_btn, LV_ALIGN_BOTTOM_MID, -40, -20);
    lv_obj_set_style_radius(s_config_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_config_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_config_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_config_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_config_btn, 0, 0);
    lv_obj_set_style_border_width(s_config_btn, 0, 0);
    lv_obj_add_event_cb(s_config_btn, config_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *config_icon = lv_label_create(s_config_btn);
    lv_label_set_text(config_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(config_icon, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(config_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(config_icon);

    s_home_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_home_btn, 50, 50);
    lv_obj_align(s_home_btn, LV_ALIGN_BOTTOM_MID, 40, -20);
    lv_obj_set_style_radius(s_home_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_home_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_home_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_home_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_home_btn, 0, 0);
    lv_obj_set_style_border_width(s_home_btn, 0, 0);
    lv_obj_add_event_cb(s_home_btn, home_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_icon = lv_label_create(s_home_btn);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(home_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(home_icon);

    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);

    build_pages();
    show_index_impl(s_index, true);
    return s_scr;
}
