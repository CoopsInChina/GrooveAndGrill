#include "ui_bbq_add.h"
#include "ui_common.h"
#include "ui_bbq_config.h"
#include "bbq_controller.h"
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

static uint8_t      s_grill_num = 1;
static sensor_src_t s_src       = SRC_TC;
static uint8_t      s_hw_id     = 0;

static void show_step(step_t step);

static void go_back(void)          { ui_navigate_to(SCREEN_BBQ); }
static void gesture_cb(lv_event_t *e) { ui_handle_gesture(NULL, go_back, NULL, NULL); }

// Hand the chosen sensor + role to the config screen.
static void to_config(sensor_role_t role)
{
    bbq_setup_t s = { .grill_num = s_grill_num, .src = s_src, .hw_id = s_hw_id, .role = role };
    bbq_setup_set(&s);
    ui_navigate_to(SCREEN_BBQ_CONFIG);
    ui_bbq_config_begin();
}

// ---- Pill button helper ------------------------------------------------
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
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, txt_col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
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

    // If this grill already has an ambient (grill-temp) sensor, the new one
    // must be a meat; otherwise let the user choose the type.
    if (bbq_grill_has_ambient(s_grill_num, NULL, NULL)) to_config(ROLE_MEAT);
    else                                                show_step(STEP_TYPE);
}

static void build_sensor_list(void)
{
    lv_obj_clean(s_sensor_box);

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

// ---- Step 1: grill number ---------------------------------------------
static void grill_btn_cb(lv_event_t *e)
{
    s_grill_num = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    show_step(STEP_SENSOR);
}

// ---- Step switching ----------------------------------------------------
static void show_step(step_t step)
{
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

static void scr_loaded_cb(lv_event_t *e) { show_step(STEP_GRILL); }
static void scr_del_cb(lv_event_t *e)
{
    s_scr = s_prompt = s_grill_box = s_sensor_box = s_type_box = NULL;
}

// ---- Container helper --------------------------------------------------
static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 340, 300);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 45);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 10, 0);
    return box;
}

lv_obj_t *ui_bbq_add_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,    LV_EVENT_GESTURE,       NULL);
    lv_obj_add_event_cb(s_scr, scr_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,        NULL);
    ui_add_home_btn(s_scr);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Add Meat");
    lv_obj_set_style_text_color(title, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -150);

    s_prompt = lv_label_create(s_scr);
    lv_label_set_text(s_prompt, "Which grill?");
    lv_obj_set_style_text_color(s_prompt, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_prompt, &lv_font_montserrat_20, 0);
    lv_obj_align(s_prompt, LV_ALIGN_CENTER, 0, -110);

    // Step 1 — grill numbers.
    s_grill_box = make_box(s_scr);
    lv_obj_clear_flag(s_grill_box, LV_OBJ_FLAG_SCROLLABLE);
    for (int g = 1; g <= MAX_GRILLS; g++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Grill %d", g);
        make_pill(s_grill_box, buf, COL_TEXT, grill_btn_cb, (void *)(intptr_t)g);
    }

    // Step 2 — sensor list (scrollable, rebuilt on entry).
    s_sensor_box = make_box(s_scr);

    // Step 3 — sensor type.
    s_type_box = make_box(s_scr);
    lv_obj_clear_flag(s_type_box, LV_OBJ_FLAG_SCROLLABLE);
    make_pill(s_type_box, "Grill Temp", COL_TEXT, type_btn_cb, (void *)(intptr_t)ROLE_GRILL);
    make_pill(s_type_box, "Meat",       COL_TEXT, type_btn_cb, (void *)(intptr_t)ROLE_MEAT);

    show_step(STEP_GRILL);
    return s_scr;
}
