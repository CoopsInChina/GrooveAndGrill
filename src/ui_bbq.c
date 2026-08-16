#include "ui_bbq.h"
#include "ui_common.h"
#include "ui_bbq_config.h"
#include "ui_bbq_add.h"
#include "bbq_controller.h"
#include "img_meat_icons.h"
#include "lvgl.h"
#include <stddef.h>
#include <stdio.h>

// Grill temp (outer ring) and meat temp (inner ring) semantic colours.
// Bright = reached/current, pale (dim) = remaining headroom to target —
// picked for a dark theme so "pale" reads as a muted background track
// rather than a washed-out light tint.
static const lv_color_t GRILL_BRIGHT = LV_COLOR_MAKE(0x1E, 0xD7, 0x60);
static const lv_color_t GRILL_PALE   = LV_COLOR_MAKE(0x1B, 0x3B, 0x24);
static const lv_color_t MEAT_BRIGHT  = LV_COLOR_MAKE(0xE5, 0x39, 0x35);
static const lv_color_t MEAT_PALE    = LV_COLOR_MAKE(0x4A, 0x20, 0x20);
static const lv_color_t BT_BLUE      = LV_COLOR_MAKE(0x21, 0x96, 0xF3);
static const lv_color_t ALARM_BRIGHT = LV_COLOR_MAKE(0xFF, 0x45, 0x00);
static const lv_color_t ALARM_DIM    = LV_COLOR_MAKE(0x40, 0x18, 0x10);

static lv_obj_t *s_scr           = NULL;
static lv_obj_t *s_title_lbl     = NULL;
static lv_obj_t *s_outer_arc     = NULL;   // grill temp gauge
static lv_obj_t *s_inner_arc     = NULL;   // meat temp gauge
// Readout is 4 labels so the colons line up (right-aligned names / left-aligned
// values) and each value can take its ring's colour.
static lv_obj_t *s_meat_name     = NULL;   // "Meat:"
static lv_obj_t *s_meat_val      = NULL;   // "25 C"  (red, matches meat ring)
static lv_obj_t *s_grill_name    = NULL;   // "Grill:"
static lv_obj_t *s_grill_val     = NULL;   // "350 C" (green, matches grill ring)
static lv_obj_t *s_meat_icon     = NULL;   // reused from the Grill Config meat picker
static lv_obj_t *s_noprobe_lbl   = NULL;
static lv_obj_t *s_add_probe_btn = NULL;
static lv_obj_t *s_alarm_icon    = NULL;
static lv_obj_t *s_probe_btn     = NULL;
static lv_obj_t *s_probe_icon    = NULL;
static lv_obj_t *s_config_btn    = NULL;
static lv_obj_t *s_home_btn      = NULL;
static lv_obj_t *s_lbl_end_left  = NULL;   // "0 C"
static lv_obj_t *s_lbl_end_grill = NULL;   // outer arc end value
static lv_obj_t *s_lbl_end_meat  = NULL;   // inner arc end value
static lv_obj_t *s_add_grill_lbl = NULL;
static lv_obj_t *s_add_grill_btn = NULL;

static lv_timer_t *s_refresh_timer = NULL;
static int         s_index         = 0;
static bool        s_gesture_fired = false;
static bool        s_blink_on      = false;   // alarm flash phase, toggled ~1Hz

static void show_index(int idx);

void ui_bbq_set_index(int idx)
{
    if (idx >= 0) s_index = idx;
}

// ---- Navigation ------------------------------------------------------

static void go_next(void)
{
    s_gesture_fired = true;
    // Slots: [0..count-1] are cook views (one per meat / ambient-only grill),
    // then the "Add Meat" slot — but only while more can actually be added
    // (e.g. probe mode caps at MAX_DIRECT_PROBES). Swiping past the last
    // slot goes home.
    int count     = bbq_view_count();
    int last_slot = bbq_can_add_more() ? count : count - 1;
    if (s_index < last_slot) {
        show_index(s_index + 1);
    } else {
        ui_navigate_to(SCREEN_MENU);
    }
}

static void go_prev(void)
{
    s_gesture_fired = true;
    if (s_index > 0) {
        show_index(s_index - 1);
    } else {
        ui_navigate_to(SCREEN_MENU);
    }
}

static void gesture_cb(lv_event_t *e)
{
    s_gesture_fired = true;
    ui_handle_gesture(go_next, go_prev, NULL, NULL);
}

// ---- Button callbacks --------------------------------------------------
//
// A touch that turns into a swipe fires LV_EVENT_GESTURE *and*, if it started
// on a button, LV_EVENT_CLICKED — so a swipe over a button would both navigate
// and trigger the button (e.g. swiping off the add-grill slot added a grill
// AND went to the menu). The gesture fires before the same-touch click, so the
// guard below skips that click; it resets the flag so a later real tap works.

static void add_probe_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    bbq_mock_connect_probe(s_index);
    show_index(s_index);
}

static void probe_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    bbq_mock_toggle_probe(s_index);
    show_index(s_index);
}

static void config_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    // Edit the sensor behind this view: its meat, or (for an ambient-only
    // view) its grill sensor.
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

// ---- Live refresh -----------------------------------------------------
// Re-reads the model each second so gauges/readouts track incoming BLE
// temperatures. Only redraws while the BBQ screen is actually on top.

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_blink_on = !s_blink_on;
    if (!s_scr || lv_scr_act() != s_scr) return;
    show_index(s_index);
}

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

// ---- Gauge update ------------------------------------------------------

static void set_arc_neutral(lv_obj_t *arc)
{
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_color(arc, COL_BUTTON, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
}

static void set_arc_no_reading(lv_obj_t *arc, int target, lv_color_t pale)
{
    lv_arc_set_range(arc, 0, target > 0 ? target : 1);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_color(arc, pale, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
}

static void set_arc_live(lv_obj_t *arc, int target, float current,
                          lv_color_t pale, lv_color_t bright)
{
    int max = target > 0 ? target : 1;
    int val = (int)current;
    if (val > max) val = max;
    if (val < 0)   val = 0;
    lv_arc_set_range(arc, 0, max);
    lv_arc_set_value(arc, val);
    lv_obj_set_style_arc_color(arc, pale, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, bright, LV_PART_INDICATOR);
}

// Lost-connection alarm: fills to the last-known reading, alternating the
// indicator between bright/dim warning colours each refresh tick (~1Hz).
static void set_arc_alarm(lv_obj_t *arc, int target, float last_known, bool blink_on)
{
    int max = target > 0 ? target : 1;
    int val = (int)last_known;
    if (val > max) val = max;
    if (val < 0)   val = 0;
    lv_arc_set_range(arc, 0, max);
    lv_arc_set_value(arc, val);
    lv_obj_set_style_arc_color(arc, ALARM_DIM, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, blink_on ? ALARM_BRIGHT : ALARM_DIM, LV_PART_INDICATOR);
}

// ---- Screen lifecycle --------------------------------------------------

static void scr_loaded_cb(lv_event_t *e)
{
    show_index(s_index);
}

static void scr_del_cb(lv_event_t *e)
{
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    s_scr = s_title_lbl = s_outer_arc = s_inner_arc = NULL;
    s_meat_name = s_meat_val = s_grill_name = s_grill_val = NULL;
    s_meat_icon = s_noprobe_lbl = s_add_probe_btn = s_alarm_icon = NULL;
    s_probe_btn = s_probe_icon = s_config_btn = s_home_btn = NULL;
    s_lbl_end_left = s_lbl_end_grill = s_lbl_end_meat = NULL;
    s_add_grill_lbl = s_add_grill_btn = NULL;
}

// ---- Create -------------------------------------------------------------

lv_obj_t *ui_bbq_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,    LV_EVENT_GESTURE,       NULL);
    lv_obj_add_event_cb(s_scr, scr_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,        NULL);

    // ---- Concentric gauges: outer = grill temp, inner = meat temp ----
    // Ring widths are ~75% wider than the first pass (22/18 -> 38/32). For a
    // zero gap between the bands, the inner arc's outer edge must meet the
    // outer arc's inner edge: with the LVGL arc band centred on radius
    // (size/2 - pad) and equal padding, inner size = outer size - Wo - Wi
    // = 440 - 38 - 32 = 370.
    s_outer_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_outer_arc, 440, 440);
    lv_obj_center(s_outer_arc);
    lv_arc_set_bg_angles(s_outer_arc, 135, 405);
    lv_arc_set_rotation(s_outer_arc, 0);
    lv_obj_set_style_arc_width(s_outer_arc, 38, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_outer_arc, 38, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_outer_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_outer_arc, false, LV_PART_INDICATOR);
    lv_obj_remove_style(s_outer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_outer_arc, LV_OPA_TRANSP, 0);

    s_inner_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_inner_arc, 370, 370);
    lv_obj_center(s_inner_arc);
    lv_arc_set_bg_angles(s_inner_arc, 135, 405);
    lv_arc_set_rotation(s_inner_arc, 0);
    lv_obj_set_style_arc_width(s_inner_arc, 32, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_inner_arc, 32, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_inner_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_inner_arc, false, LV_PART_INDICATOR);
    lv_obj_remove_style(s_inner_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_inner_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_inner_arc, LV_OPA_TRANSP, 0);

    // ---- Arc end labels — sit at the bottom ends of each ring. Both rings
    // start at 0 at the bottom-left (135 deg); their max values sit at the
    // bottom-right ends (45 deg), grill (outer) further out than meat (inner).
    s_lbl_end_left = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_end_left, "0 C");
    lv_obj_set_style_text_color(s_lbl_end_left, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_end_left, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_end_left, LV_ALIGN_CENTER, -100, 140);
    lv_obj_clear_flag(s_lbl_end_left, LV_OBJ_FLAG_CLICKABLE);

    // Grill max sits at the outer ring's bottom-right end (further out/down);
    // meat max at the inner ring's end (up-left of it) — the two line up along
    // the ring-end diagonal.
    s_lbl_end_grill = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lbl_end_grill, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_end_grill, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_end_grill, LV_ALIGN_CENTER, 116, 148);
    lv_obj_clear_flag(s_lbl_end_grill, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_end_meat = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lbl_end_meat, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_end_meat, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_end_meat, LV_ALIGN_CENTER, 98, 118);
    lv_obj_clear_flag(s_lbl_end_meat, LV_OBJ_FLAG_CLICKABLE);

    // ---- Title ----
    s_title_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_title_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -114);
    lv_obj_clear_flag(s_title_lbl, LV_OBJ_FLAG_CLICKABLE);

    // ---- Meat icon (reused from the Grill Config meat picker) — central,
    // 20% bigger than native 120px (zoom 256 = 100%, so 307 = 120%). ----
    s_meat_icon = lv_img_create(s_scr);
    lv_img_set_zoom(s_meat_icon, 307);
    lv_obj_align(s_meat_icon, LV_ALIGN_CENTER, 0, -37);
    lv_obj_clear_flag(s_meat_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_meat_icon, LV_OBJ_FLAG_HIDDEN);

    // ---- Readouts — below the icon, montserrat_28 (matches the title).
    // Names right-aligned and values left-aligned around a shared colon
    // column at screen centre, so the colons line up; each value takes its
    // ring's colour.
    s_meat_name = lv_label_create(s_scr);
    lv_label_set_text(s_meat_name, "Meat:");
    lv_obj_set_width(s_meat_name, 120);
    lv_obj_set_style_text_align(s_meat_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_meat_name, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_meat_name, &lv_font_montserrat_28, 0);
    lv_obj_align(s_meat_name, LV_ALIGN_CENTER, -60, 50);
    lv_obj_clear_flag(s_meat_name, LV_OBJ_FLAG_CLICKABLE);

    s_meat_val = lv_label_create(s_scr);
    lv_obj_set_width(s_meat_val, 120);
    lv_obj_set_style_text_align(s_meat_val, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_meat_val, MEAT_BRIGHT, 0);
    lv_obj_set_style_text_font(s_meat_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_meat_val, LV_ALIGN_CENTER, 68, 50);
    lv_obj_clear_flag(s_meat_val, LV_OBJ_FLAG_CLICKABLE);

    s_grill_name = lv_label_create(s_scr);
    lv_label_set_text(s_grill_name, "Grill:");
    lv_obj_set_width(s_grill_name, 120);
    lv_obj_set_style_text_align(s_grill_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_grill_name, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_grill_name, &lv_font_montserrat_28, 0);
    lv_obj_align(s_grill_name, LV_ALIGN_CENTER, -60, 87);
    lv_obj_clear_flag(s_grill_name, LV_OBJ_FLAG_CLICKABLE);

    s_grill_val = lv_label_create(s_scr);
    lv_obj_set_width(s_grill_val, 120);
    lv_obj_set_style_text_align(s_grill_val, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_grill_val, GRILL_BRIGHT, 0);
    lv_obj_set_style_text_font(s_grill_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_grill_val, LV_ALIGN_CENTER, 68, 87);
    lv_obj_clear_flag(s_grill_val, LV_OBJ_FLAG_CLICKABLE);

    // ---- No-probe state: label + add button ----
    s_noprobe_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_noprobe_lbl, "No Probe");
    lv_obj_set_style_text_color(s_noprobe_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_noprobe_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_noprobe_lbl, LV_ALIGN_CENTER, 0, 50);
    lv_obj_clear_flag(s_noprobe_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_add_probe_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_add_probe_btn, 60, 60);
    lv_obj_align(s_add_probe_btn, LV_ALIGN_CENTER, 0, 105);
    lv_obj_set_style_radius(s_add_probe_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_add_probe_btn, COL_PANEL, 0);
    lv_obj_set_style_border_color(s_add_probe_btn, COL_TEXT, 0);
    lv_obj_set_style_border_width(s_add_probe_btn, 2, 0);
    lv_obj_set_style_shadow_width(s_add_probe_btn, 0, 0);
    lv_obj_add_event_cb(s_add_probe_btn, add_probe_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_probe_icon = lv_label_create(s_add_probe_btn);
    lv_label_set_text(add_probe_icon, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(add_probe_icon, COL_TEXT, 0);
    lv_obj_set_style_text_font(add_probe_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(add_probe_icon);

    // ---- Alarm icon (probe disconnected — flashes) ----
    // Alarm icon — replaces the meat icon in the same spot when that sensor
    // has lost connection after being live.
    s_alarm_icon = lv_label_create(s_scr);
    lv_label_set_text(s_alarm_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(s_alarm_icon, ALARM_BRIGHT, 0);
    lv_obj_set_style_text_font(s_alarm_icon, &lv_font_montserrat_48, 0);
    lv_obj_align(s_alarm_icon, LV_ALIGN_CENTER, 0, -37);
    lv_obj_clear_flag(s_alarm_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_alarm_icon, LV_OBJ_FLAG_HIDDEN);

    // ---- Probe status toggle (mock connect/disconnect) — 50% bigger icon
    // (montserrat_20 -> 30), moved down to clear the taller readout. ----
    s_probe_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_probe_btn, 64, 64);
    lv_obj_align(s_probe_btn, LV_ALIGN_CENTER, 0, 145);
    lv_obj_set_style_radius(s_probe_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_probe_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_probe_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_probe_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_probe_btn, 0, 0);
    lv_obj_set_style_border_width(s_probe_btn, 0, 0);
    lv_obj_add_event_cb(s_probe_btn, probe_btn_cb, LV_EVENT_CLICKED, NULL);

    s_probe_icon = lv_label_create(s_probe_btn);
    lv_label_set_text(s_probe_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_probe_icon, &lv_font_montserrat_30, 0);
    lv_obj_center(s_probe_icon);

    // ---- Config + Home row (bottom gap of the arcs) ----
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

    // ---- Add-Grill slot (shown only when index == grill count) ----
    s_add_grill_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_add_grill_lbl, "Add Meat");
    lv_obj_set_style_text_color(s_add_grill_lbl, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(s_add_grill_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(s_add_grill_lbl, LV_ALIGN_CENTER, 0, -100);
    lv_obj_clear_flag(s_add_grill_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_add_grill_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_add_grill_btn, 120, 120);
    lv_obj_align(s_add_grill_btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(s_add_grill_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_add_grill_btn, COL_PANEL, 0);
    lv_obj_set_style_border_color(s_add_grill_btn, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(s_add_grill_btn, 2, 0);
    lv_obj_set_style_shadow_width(s_add_grill_btn, 0, 0);
    lv_obj_add_event_cb(s_add_grill_btn, add_meat_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_grill_icon = lv_label_create(s_add_grill_btn);
    lv_label_set_text(add_grill_icon, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(add_grill_icon, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(add_grill_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(add_grill_icon);

    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);

    show_index(0);
    return s_scr;
}

// ---- Show ---------------------------------------------------------------

static void show_index(int idx)
{
    s_index = idx;
    if (!s_scr) return;

    int count   = bbq_view_count();
    bool is_add = (idx == count) && bbq_can_add_more();

    // View widgets (gauges, end labels, config + BT). The "Add Meat" slot hides
    // them; a cook view shows them.
    lv_obj_t *view_only[] = {
        s_outer_arc, s_inner_arc, s_lbl_end_left, s_lbl_end_grill, s_lbl_end_meat,
        s_config_btn, s_probe_btn,
    };
    for (size_t i = 0; i < sizeof(view_only) / sizeof(view_only[0]); i++) {
        if (!view_only[i]) continue;
        if (is_add) lv_obj_add_flag(view_only[i],   LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(view_only[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *add_only[] = { s_add_grill_lbl, s_add_grill_btn };
    for (size_t i = 0; i < sizeof(add_only) / sizeof(add_only[0]); i++) {
        if (!add_only[i]) continue;
        if (is_add) lv_obj_clear_flag(add_only[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(add_only[i],   LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *readout[] = { s_meat_name, s_meat_val, s_grill_name, s_grill_val };

    // Retired legacy mock controls — always hidden now.
    if (s_add_probe_btn) lv_obj_add_flag(s_add_probe_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_noprobe_lbl)   lv_obj_add_flag(s_noprobe_lbl,   LV_OBJ_FLAG_HIDDEN);

    if (is_add) {
        if (s_title_lbl)  lv_obj_add_flag(s_title_lbl,  LV_OBJ_FLAG_HIDDEN);
        if (s_meat_icon)  lv_obj_add_flag(s_meat_icon,  LV_OBJ_FLAG_HIDDEN);
        if (s_alarm_icon) lv_obj_add_flag(s_alarm_icon, LV_OBJ_FLAG_HIDDEN);
        for (size_t i = 0; i < sizeof(readout) / sizeof(readout[0]); i++)
            if (readout[i]) lv_obj_add_flag(readout[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    bbq_view_t v;
    if (!bbq_view_at(idx, &v)) return;

    bool probe_mode = (bbq_source_get() == BBQ_SRC_PROBE);

    if (s_title_lbl) {
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
        lv_label_set_text(s_title_lbl, buf);
        lv_obj_clear_flag(s_title_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // BT dot: in probe mode, reflects THIS view's own probe link (each meat
    // has its own independent GATT connection) rather than "is any probe
    // connected" — otherwise probe A's icon stayed blue after it was turned
    // off, as long as probe B was still linked. Box mode still uses the
    // shared gateway link, since one box feeds every sensor on screen.
    if (s_probe_icon) {
        bool link = probe_mode ? v.meat_present : bbq_link_up();
        lv_obj_set_style_text_color(s_probe_icon, link ? BT_BLUE : COL_TEXT_DIM, 0);
    }

    bool meat_alarm  = v.has_meat && v.meat_alarm;
    bool grill_alarm = v.grill_assigned && v.grill_alarm;

    // Meat icon — only on a meat view; swapped for the alarm icon when that
    // probe has lost connection after being live.
    if (s_meat_icon) {
        const lv_img_dsc_t *icon = (v.has_meat && !meat_alarm) ? meat_icon_for(v.meat_kind) : NULL;
        if (icon) {
            lv_img_set_src(s_meat_icon, icon);
            lv_obj_clear_flag(s_meat_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meat_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_alarm_icon) {
        if (meat_alarm) lv_obj_clear_flag(s_alarm_icon, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag(s_alarm_icon,   LV_OBJ_FLAG_HIDDEN);
    }

    // Readout: the Meat line shows on a meat view; the Grill line shows whenever
    // this grill has an ambient sensor assigned. Each value is dim "- C" until
    // its sensor reports, then bright in its ring colour.
    if (s_meat_name)  { if (v.has_meat)      lv_obj_clear_flag(s_meat_name, LV_OBJ_FLAG_HIDDEN);  else lv_obj_add_flag(s_meat_name, LV_OBJ_FLAG_HIDDEN); }
    if (s_meat_val)   { if (v.has_meat)      lv_obj_clear_flag(s_meat_val,  LV_OBJ_FLAG_HIDDEN);  else lv_obj_add_flag(s_meat_val,  LV_OBJ_FLAG_HIDDEN); }
    if (s_grill_name) { if (v.grill_assigned) lv_obj_clear_flag(s_grill_name, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_grill_name, LV_OBJ_FLAG_HIDDEN); }
    if (s_grill_val)  { if (v.grill_assigned) lv_obj_clear_flag(s_grill_val,  LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_grill_val,  LV_OBJ_FLAG_HIDDEN); }

    if (v.has_meat && s_meat_val) {
        char b[12];
        // Alarm keeps showing the last known reading instead of "- C".
        if (v.meat_present || meat_alarm) snprintf(b, sizeof(b), "%d C", (int)v.meat_temp_c);
        else                              snprintf(b, sizeof(b), "- C");
        lv_label_set_text(s_meat_val, b);
        lv_color_t col = v.meat_present ? MEAT_BRIGHT : (meat_alarm ? ALARM_BRIGHT : COL_TEXT_DIM);
        lv_obj_set_style_text_color(s_meat_val, col, 0);
    }
    if (v.grill_assigned && s_grill_val) {
        char b[12];
        if (v.grill_present || grill_alarm) snprintf(b, sizeof(b), "%d C", (int)v.grill_temp_c);
        else                                snprintf(b, sizeof(b), "- C");
        lv_label_set_text(s_grill_val, b);
        lv_color_t col = v.grill_present ? GRILL_BRIGHT : (grill_alarm ? ALARM_BRIGHT : COL_TEXT_DIM);
        lv_obj_set_style_text_color(s_grill_val, col, 0);
    }

    // End labels carry each ring's target (blank when that ring is unused).
    if (s_lbl_end_grill) {
        if (v.grill_assigned) { char b[16]; snprintf(b, sizeof(b), "%d C", v.grill_target_c); lv_label_set_text(s_lbl_end_grill, b); }
        else                  lv_label_set_text(s_lbl_end_grill, "");
    }
    if (s_lbl_end_meat) {
        if (v.has_meat) { char b[16]; snprintf(b, sizeof(b), "%d C", v.meat_target_c); lv_label_set_text(s_lbl_end_meat, b); }
        else            lv_label_set_text(s_lbl_end_meat, "");
    }

    // ---- Gauge fill ----
    // Outer = grill ambient.
    if (!v.grill_assigned)     set_arc_neutral(s_outer_arc);
    else if (grill_alarm)      set_arc_alarm(s_outer_arc, v.grill_target_c, v.grill_temp_c, s_blink_on);
    else if (!v.grill_present) set_arc_no_reading(s_outer_arc, v.grill_target_c, GRILL_PALE);
    else                       set_arc_live(s_outer_arc, v.grill_target_c, v.grill_temp_c, GRILL_PALE, GRILL_BRIGHT);
    // Inner = this meat.
    if (!v.has_meat)          set_arc_neutral(s_inner_arc);
    else if (meat_alarm)      set_arc_alarm(s_inner_arc, v.meat_target_c, v.meat_temp_c, s_blink_on);
    else if (!v.meat_present) set_arc_no_reading(s_inner_arc, v.meat_target_c, MEAT_PALE);
    else                      set_arc_live(s_inner_arc, v.meat_target_c, v.meat_temp_c, MEAT_PALE, MEAT_BRIGHT);
}
