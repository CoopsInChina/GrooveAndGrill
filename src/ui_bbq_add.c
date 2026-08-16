#include "ui_bbq_add.h"
#include "ui_common.h"
#include "ui_bbq_config.h"
#include "bbq_controller.h"
#include "ble_probe.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef enum { STEP_GRILL, STEP_SENSOR, STEP_TYPE } step_t;

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_prompt     = NULL;
static lv_obj_t *s_grill_box  = NULL;
static lv_obj_t *s_sensor_box = NULL;
static lv_obj_t *s_type_box   = NULL;
static lv_timer_t *s_refresh_timer = NULL;

static uint8_t      s_grill_num   = 1;
static sensor_src_t s_src         = SRC_TC;
static uint8_t      s_hw_id       = 0;
static step_t       s_step        = STEP_GRILL;

static void show_step(step_t step);
static void build_sensor_list(void);

static void go_back(void)          { ui_navigate_to(SCREEN_BBQ); }
static void gesture_cb(lv_event_t *e) { ui_handle_gesture(NULL, go_back, NULL, NULL); }
static void home_btn_cb(lv_event_t *e) { ui_navigate_to(SCREEN_MENU); }

// Hand the chosen sensor + role to the config screen.
static void to_config(sensor_role_t role)
{
    bbq_setup_t s = { .grill_num = s_grill_num, .src = s_src, .hw_id = s_hw_id, .role = role };
    bbq_setup_set(&s);
    ui_navigate_to(SCREEN_BBQ_CONFIG);
    ui_bbq_config_begin();
}

// ---- Live refresh — the sensor step shows a live pool (a probe can connect
// or drop while the wizard sits open), so rebuild it periodically rather than
// only on entry. ----
static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_scr || lv_scr_act() != s_scr) return;
    if (s_step != STEP_SENSOR) return;
    build_sensor_list();
}

// ---- Pill button helper (box-mode sensor/type lists) --------------------
static lv_obj_t *make_pill(lv_obj_t *parent, const char *txt, lv_color_t txt_col,
                           lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 300, 52);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_set_style_border_color(b, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, txt_col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

// ---- Grid button helper (box-mode grill picker, 2x2) ---------------------
static lv_obj_t *make_grid_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 130, 90);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_set_style_border_color(b, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    return b;
}

// ---- Probe slot helper (probe-only mode sensor step) ---------------------
// A fixed slot per possible direct-probe connection: dim/inert placeholder
// ("Searching…") until a free probe is actually connected, then bright with
// its live temperature and tappable.
static lv_obj_t *make_probe_slot(lv_obj_t *parent, bool active, const char *line1,
                                 const char *line2, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 130, 150);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (active) {
        lv_obj_set_style_bg_color(b, COL_PANEL, 0);
        lv_obj_set_style_border_color(b, COL_ACCENT2, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(b, COL_TEXT_DIM, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *icon = lv_label_create(b);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(icon, active ? COL_ACCENT2 : COL_TEXT_DIM, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *l1 = lv_label_create(b);
    lv_label_set_text(l1, line1);
    lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l1, active ? COL_TEXT : COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_16, 0);
    lv_obj_align(l1, LV_ALIGN_BOTTOM_MID, 0, -34);

    lv_obj_t *l2 = lv_label_create(b);
    lv_label_set_text(l2, line2);
    lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l2, active ? COL_TEXT : COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
    lv_obj_align(l2, LV_ALIGN_BOTTOM_MID, 0, -12);

    return b;
}

// ---- Step 3: sensor type ----------------------------------------------
static void type_btn_cb(lv_event_t *e)
{
    sensor_role_t role = (sensor_role_t)(intptr_t)lv_event_get_user_data(e);
    to_config(role);
}

// ---- Step 2: sensor selection -----------------------------------------
static void sensor_btn_cb(lv_event_t *e)
{
    intptr_t enc = (intptr_t)lv_event_get_user_data(e);
    s_src   = (sensor_src_t)((enc >> 8) & 0xFF);
    s_hw_id = (uint8_t)(enc & 0xFF);

    // Wireless-probe-only mode has no ambient/grill-temp sensors — every
    // probe is a meat, so skip the type question entirely.
    if (bbq_source_get() == BBQ_SRC_PROBE) { to_config(ROLE_MEAT); return; }

    // If this grill already has an ambient (grill-temp) sensor, the new one
    // must be a meat; otherwise let the user choose the type.
    if (bbq_grill_has_ambient(s_grill_num, NULL, NULL)) to_config(ROLE_MEAT);
    else                                                show_step(STEP_TYPE);
}

static void build_sensor_list_box_mode(void)
{
    lv_obj_set_flex_flow(s_sensor_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_sensor_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_sensor_box, 10, 0);
    lv_obj_set_style_pad_column(s_sensor_box, 0, 0);

    int count = bbq_sensor_count(), wnum = 0, shown = 0;
    for (int i = 0; i < count; i++) {
        bbq_sensor_t s;
        if (!bbq_sensor_at(i, &s)) continue;
        // Number every wireless sensor so the label is stable, but only offer
        // sensors that aren't already allocated to a grill/role.
        int this_wnum = (s.src == SRC_PROBE) ? ++wnum : 0;
        if (s.role != ROLE_UNASSIGNED) continue;

        char name[56];
        if (s.src == SRC_TC) snprintf(name, sizeof(name), "Wired Temp Sensor %d", s.hw_id + 1);
        else                 snprintf(name, sizeof(name), "Wireless Temp Sensor %d", this_wnum);
        if (!s.present) strncat(name, "  (offline)", sizeof(name) - strlen(name) - 1);

        intptr_t enc = ((intptr_t)s.src << 8) | s.hw_id;
        make_pill(s_sensor_box, name, s.present ? COL_TEXT : COL_TEXT_DIM,
                  sensor_btn_cb, (void *)enc);
        shown++;
    }
    if (!shown) {
        lv_obj_t *l = lv_label_create(s_sensor_box);
        lv_label_set_text(l, "No free sensors");
        lv_obj_set_style_text_color(l, COL_TEXT_DIM, 0);
    }
}

static void build_sensor_list_probe_mode(void)
{
    lv_obj_set_flex_flow(s_sensor_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sensor_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_sensor_box, 24, 0);
    lv_obj_set_style_pad_row(s_sensor_box, 0, 0);

    // Fixed UI slots (stable per physical probe — see bbq_probe_slot_of), not
    // "whichever free probe happens to be found first": once a probe has ever
    // been seen it keeps its position here, even while off or already bonded.
    for (int slot = 0; slot < MAX_DIRECT_PROBES; slot++) {
        uint8_t hw_id;
        if (!bbq_probe_slot_get(slot, &hw_id)) {
            make_probe_slot(s_sensor_box, false, "Wireless", "Searching...", NULL, NULL);
            continue;
        }
        bbq_sensor_t s;
        bool have = bbq_sensor_get(SRC_PROBE, hw_id, &s);
        if (have && s.role != ROLE_UNASSIGNED) {
            // Already bonded to a meat — shown so the slot stays visible/
            // "occupied", but it's not offered again here.
            make_probe_slot(s_sensor_box, false, "Wireless", "Assigned", NULL, NULL);
        } else if (have && s.present) {
            char temp_buf[16];
            snprintf(temp_buf, sizeof(temp_buf), "%.1f C", s.temp_c);
            intptr_t enc = ((intptr_t)SRC_PROBE << 8) | hw_id;
            make_probe_slot(s_sensor_box, true, "Wireless", temp_buf, sensor_btn_cb, (void *)enc);
        } else {
            // Bonded to this slot previously but not connected right now.
            make_probe_slot(s_sensor_box, false, "Wireless", "Not connected", NULL, NULL);
        }
    }
}

static void build_sensor_list(void)
{
    lv_obj_clean(s_sensor_box);
    if (bbq_source_get() == BBQ_SRC_PROBE) build_sensor_list_probe_mode();
    else                                   build_sensor_list_box_mode();
}

// ---- Step 1: grill number (box mode only) ------------------------------
static void grill_btn_cb(lv_event_t *e)
{
    s_grill_num = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    show_step(STEP_SENSOR);
}

// ---- Step switching ----------------------------------------------------
static void show_step(step_t step)
{
    s_step = step;
    if (step == STEP_SENSOR) build_sensor_list();

    if (s_prompt) {
        const char *p = (step == STEP_GRILL)  ? "Which grill?" :
                        (step == STEP_SENSOR) ? "Select a sensor" : "Sensor type?";
        lv_label_set_text(s_prompt, p);
    }
    if (s_grill_box)  { if (step == STEP_GRILL)  lv_obj_clear_flag(s_grill_box,  LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_grill_box,  LV_OBJ_FLAG_HIDDEN); }
    if (s_sensor_box) { if (step == STEP_SENSOR) lv_obj_clear_flag(s_sensor_box, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_sensor_box, LV_OBJ_FLAG_HIDDEN); }
    if (s_type_box)   { if (step == STEP_TYPE)   lv_obj_clear_flag(s_type_box,   LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_type_box,   LV_OBJ_FLAG_HIDDEN); }
}

// Picks the correct starting step for the active source. Called both at
// create time (so the very first frame — visible during the screen's 250ms
// fade-in — is already right, instead of flashing the box-mode grill step
// before scr_loaded_cb corrects it) and on every re-entry (source can change
// via Settings between visits).
static void init_step(void)
{
    if (bbq_source_get() == BBQ_SRC_PROBE) {
        // No grill grouping in probe-only mode — go straight to sensor pick.
        s_grill_num = 1;
        show_step(STEP_SENSOR);
    } else {
        show_step(STEP_GRILL);
    }
}

static void scr_loaded_cb(lv_event_t *e) { init_step(); }

static void scr_del_cb(lv_event_t *e)
{
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    s_scr = s_prompt = s_grill_box = s_sensor_box = s_type_box = NULL;
}

// ---- Container helper --------------------------------------------------
static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 340, 280);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 45);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 10, 0);
    return box;
}

// 2x2 grid container for the box-mode grill picker (wraps 130px buttons two
// per row inside a 300px-wide box).
static lv_obj_t *make_grid_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 300, 210);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 16, 0);
    lv_obj_set_style_pad_row(box, 16, 0);
    return box;
}

lv_obj_t *ui_bbq_add_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,    LV_EVENT_GESTURE,       NULL);
    lv_obj_add_event_cb(s_scr, scr_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,        NULL);

    // Bottom-centered home button on this screen (the wizard's own controls
    // fill the top/middle, unlike other screens' top-corner placement).
    lv_obj_t *home_btn = lv_btn_create(s_scr);
    lv_obj_set_size(home_btn, 50, 50);
    lv_obj_align(home_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(home_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(home_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(home_btn, 0, 0);
    lv_obj_set_style_border_width(home_btn, 0, 0);
    lv_obj_add_event_cb(home_btn, home_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_btn);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(home_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(home_icon);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Add Meat");
    lv_obj_set_style_text_color(title, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -140);

    s_prompt = lv_label_create(s_scr);
    lv_label_set_text(s_prompt, "Which grill?");
    lv_obj_set_style_text_color(s_prompt, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_prompt, &lv_font_montserrat_20, 0);
    lv_obj_align(s_prompt, LV_ALIGN_CENTER, 0, -100);

    // Step 1 — grill numbers (box mode only). 2x2 grid: 1,2 top row; 3,4 bottom.
    s_grill_box = make_grid_box(s_scr);
    for (int g = 1; g <= MAX_GRILLS; g++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Grill %d", g);
        make_grid_btn(s_grill_box, buf, grill_btn_cb, (void *)(intptr_t)g);
    }

    // Step 2 — sensor list. Column of pills (box mode) or a row of probe
    // slots (probe mode); build_sensor_list() picks the layout.
    s_sensor_box = make_box(s_scr);

    // Step 3 — sensor type (box mode only).
    s_type_box = make_box(s_scr);
    lv_obj_clear_flag(s_type_box, LV_OBJ_FLAG_SCROLLABLE);
    make_pill(s_type_box, "Grill Temp", COL_TEXT, type_btn_cb, (void *)(intptr_t)ROLE_GRILL);
    make_pill(s_type_box, "Meat",       COL_TEXT, type_btn_cb, (void *)(intptr_t)ROLE_MEAT);

    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);

    init_step();
    return s_scr;
}
