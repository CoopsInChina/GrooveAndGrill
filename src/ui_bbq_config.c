#include "ui_bbq_config.h"
#include "ui_common.h"
#include "ui_bbq_doneness.h"
#include "ui_bbq.h"
#include "bbq_controller.h"
#include "meat_temps.h"
#include "img_meat_icons.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

// The slider works in 5C steps: its raw value is in units of GRILL_STEP_C, so
// each drag increment moves exactly 5C. Celsius = raw * GRILL_STEP_C.
#define GRILL_STEP_C  5
#define GRILL_MAX_C   600

static lv_obj_t *s_scr         = NULL;
static lv_obj_t *s_title_lbl   = NULL;
static lv_obj_t *s_slider_lbl  = NULL;
static lv_obj_t *s_slider      = NULL;
static lv_obj_t *s_temp_lbl    = NULL;
static lv_obj_t *s_confirm_btn = NULL;
static lv_obj_t *s_trash_btn   = NULL;
static lv_obj_t *s_meat_btns_obj[4] = {0};

// Pending allocation this screen completes (from bbq_setup).
static uint8_t      s_grill_num = 1;
static sensor_src_t s_src       = SRC_TC;
static uint8_t      s_hw_id     = 0;
static sensor_role_t s_role     = ROLE_GRILL;

static int slider_celsius(void)
{
    return (int)lv_slider_get_value(s_slider) * GRILL_STEP_C;
}

typedef struct {
    const char          *name;         // must match a MEAT_TYPES entry, except Chicken
    meat_kind_t          kind;
    const lv_img_dsc_t  *icon;
} meat_btn_info_t;

// Same icons are reused on the BBQ grill screen (see ui_bbq.c) via meat_kind_t.
static const meat_btn_info_t s_meat_btns[] = {
    { "Chicken", MEAT_KIND_CHICKEN, &img_meat_chicken },
    { "Lamb",    MEAT_KIND_LAMB,    &img_meat_lamb    },
    { "Pork",    MEAT_KIND_PORK,    &img_meat_pork    },
    { "Beef",    MEAT_KIND_BEEF,    &img_meat_beef    },
};

static void go_back(void) { ui_navigate_to(SCREEN_BBQ); }

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(NULL, go_back, NULL, NULL);
}

// Apply the current slider value to the right ambient sensor: for a grill-temp
// sensor that's the sensor being configured; for a meat, it's the grill's
// existing ambient sensor (if any).
static void apply_grill_temp(int c)
{
    if (s_role == ROLE_GRILL) {
        bbq_sensor_assign(s_src, s_hw_id, s_grill_num, ROLE_GRILL, MEAT_KIND_NONE, c);
    } else {
        sensor_src_t as; uint8_t ah;
        if (bbq_grill_has_ambient(s_grill_num, &as, &ah))
            bbq_sensor_assign(as, ah, s_grill_num, ROLE_GRILL, MEAT_KIND_NONE, c);
    }
}

static void slider_changed_cb(lv_event_t *e)
{
    // Live-update the label only; the target is persisted on confirm / meat
    // select so we don't hit NVS on every drag tick.
    if (s_temp_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d C", slider_celsius());
        lv_label_set_text(s_temp_lbl, buf);
    }
}

static void confirm_btn_cb(lv_event_t *e)
{
    // Grill-temp sensor: the slider already assigned it live, but confirm here
    // in case it was never dragged (defaults still apply).
    apply_grill_temp(slider_celsius());
    ui_bbq_set_index(bbq_view_index_for(s_src, s_hw_id));
    ui_navigate_to(SCREEN_BBQ);
}

static void trash_btn_cb(lv_event_t *e)
{
    // Remove this allocation (deletes the meat / frees the sensor).
    bbq_sensor_unassign(s_src, s_hw_id);
    ui_bbq_set_index(0);   // that view is gone — land somewhere sane
    ui_navigate_to(SCREEN_BBQ);
}

static int find_meat_type_index(const char *name)
{
    for (int i = 0; i < MEAT_TYPE_COUNT; i++) {
        if (strcmp(MEAT_TYPES[i].name, name) == 0) return i;
    }
    return -1;
}

static void meat_btn_cb(lv_event_t *e)
{
    const meat_btn_info_t *info = (const meat_btn_info_t *)lv_event_get_user_data(e);

    // Persist the grill ambient temp chosen on the slider (if this grill has an
    // ambient sensor) before moving on to the meat's own target.
    apply_grill_temp(slider_celsius());

    if (info->kind == MEAT_KIND_CHICKEN) {
        // Poultry has one food-safety target — no doneness preference to pick.
        bbq_sensor_assign(s_src, s_hw_id, s_grill_num, ROLE_MEAT,
                          MEAT_KIND_CHICKEN, CHICKEN_SAFE_TARGET_C);
        ui_bbq_set_index(bbq_view_index_for(s_src, s_hw_id));
        ui_navigate_to(SCREEN_BBQ);
        return;
    }

    int meat_idx = find_meat_type_index(info->name);
    if (meat_idx < 0) return;   // shouldn't happen — button names match MEAT_TYPES

    ui_navigate_to(SCREEN_BBQ_DONENESS);
    ui_bbq_doneness_begin(meat_idx, info->kind);
}

static lv_obj_t *make_meat_btn(lv_obj_t *parent, const meat_btn_info_t *info, int x, int y)
{
    // No box or text label — a transparent circular hit-area with a subtle
    // press highlight, holding just the icon.
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 124, 124);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, meat_btn_cb, LV_EVENT_CLICKED, (void *)info);

    // Icons at native 120px (no zoom).
    lv_obj_t *icon = lv_img_create(btn);
    lv_img_set_src(icon, info->icon);
    lv_obj_center(icon);

    return btn;
}

lv_obj_t *ui_bbq_config_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    ui_add_home_btn(s_scr);

    // Title carries the grill number + role, set in ui_bbq_config_begin().
    s_title_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_title_lbl, "Grill Config");
    lv_obj_set_style_text_color(s_title_lbl, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -140);

    // ---- Target grill temp ----
    s_slider_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_slider_lbl, "Target Grill Temp");
    lv_obj_set_style_text_color(s_slider_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_slider_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_slider_lbl, LV_ALIGN_CENTER, 0, -100);

    s_slider = lv_slider_create(s_scr);
    lv_obj_set_size(s_slider, 200, 12);
    lv_obj_align(s_slider, LV_ALIGN_CENTER, -30, -70);
    // Sliders bubble gestures to the parent by default, so a drag also gets
    // read as a screen swipe (see go_back()) and navigates away mid-drag.
    lv_obj_clear_flag(s_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_slider_set_range(s_slider, 0, GRILL_MAX_C / GRILL_STEP_C);   // 0..120, each = 5C
    lv_slider_set_value(s_slider, 200 / GRILL_STEP_C, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, COL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_temp_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_temp_lbl, "200 C");
    lv_obj_set_style_text_color(s_temp_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_temp_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_temp_lbl, LV_ALIGN_CENTER, 105, -70);

    // ---- Meat selection grid (2x2) — shown for meat sensors. ----
    s_meat_btns_obj[0] = make_meat_btn(s_scr, &s_meat_btns[0], -72, 10);
    s_meat_btns_obj[1] = make_meat_btn(s_scr, &s_meat_btns[1],  72, 10);
    s_meat_btns_obj[2] = make_meat_btn(s_scr, &s_meat_btns[2], -72, 150);
    s_meat_btns_obj[3] = make_meat_btn(s_scr, &s_meat_btns[3],  72, 150);

    // ---- Confirm button — shown for grill-temp sensors (no meat to tap). ----
    s_confirm_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_confirm_btn, 200, 50);
    lv_obj_align(s_confirm_btn, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_style_radius(s_confirm_btn, 12, 0);
    lv_obj_set_style_bg_color(s_confirm_btn, COL_ACCENT, 0);
    lv_obj_set_style_shadow_width(s_confirm_btn, 0, 0);
    lv_obj_set_style_border_width(s_confirm_btn, 0, 0);
    lv_obj_add_event_cb(s_confirm_btn, confirm_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_lbl = lv_label_create(s_confirm_btn);
    lv_label_set_text(confirm_lbl, "SET GRILL TEMP");
    lv_obj_set_style_text_color(confirm_lbl, COL_BG, 0);
    lv_obj_set_style_text_font(confirm_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(confirm_lbl);

    // ---- Delete allocation (trash) — bottom, only shown when editing an
    // already-allocated sensor. ----
    s_trash_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_trash_btn, 46, 46);
    lv_obj_align(s_trash_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(s_trash_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_trash_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_trash_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_trash_btn, COL_WARN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_trash_btn, 0, 0);
    lv_obj_set_style_border_width(s_trash_btn, 0, 0);
    lv_obj_add_event_cb(s_trash_btn, trash_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *trash_icon = lv_label_create(s_trash_btn);
    lv_label_set_text(trash_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(trash_icon, COL_WARN, 0);
    lv_obj_set_style_text_font(trash_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(trash_icon);

    return s_scr;
}

static void show_slider(bool on)
{
    lv_obj_t *w[] = { s_slider_lbl, s_slider, s_temp_lbl };
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
        if (!w[i]) continue;
        if (on) lv_obj_clear_flag(w[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(w[i],   LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_meat_grid(bool on)
{
    for (int i = 0; i < 4; i++) {
        if (!s_meat_btns_obj[i]) continue;
        if (on) lv_obj_clear_flag(s_meat_btns_obj[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_meat_btns_obj[i],   LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_bbq_config_begin(void)
{
    bbq_setup_t setup;
    if (!bbq_setup_get(&setup)) return;
    s_grill_num = setup.grill_num;
    s_src       = setup.src;
    s_hw_id     = setup.hw_id;
    s_role      = setup.role;

    bool is_grill = (s_role == ROLE_GRILL);
    sensor_src_t as; uint8_t ah;
    bool has_ambient = bbq_grill_has_ambient(s_grill_num, &as, &ah);

    if (s_title_lbl) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Grill %d %s", s_grill_num, is_grill ? "Temp" : "Meat");
        lv_label_set_text(s_title_lbl, buf);
    }

    // Grill-temp sensor: slider + confirm, no meat grid.
    // Meat sensor: meat grid; slider only when there's an ambient to tweak.
    show_slider(is_grill || has_ambient);
    show_meat_grid(!is_grill);
    if (s_confirm_btn) {
        if (is_grill) lv_obj_clear_flag(s_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_confirm_btn,   LV_OBJ_FLAG_HIDDEN);
    }

    // Trash only when editing a sensor that's already allocated (the ⚙ path),
    // not while first adding one through the wizard.
    bbq_sensor_t exist;
    bool allocated = bbq_sensor_get(s_src, s_hw_id, &exist) && exist.role != ROLE_UNASSIGNED;
    if (s_trash_btn) {
        if (allocated) lv_obj_clear_flag(s_trash_btn, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_trash_btn,   LV_OBJ_FLAG_HIDDEN);
    }

    // Seed the slider from whatever target is relevant (this grill-temp sensor,
    // or the grill's existing ambient), defaulting to 200C.
    int init_c = 200;
    bbq_sensor_t cur;
    if (is_grill) {
        if (bbq_sensor_get(s_src, s_hw_id, &cur) && cur.role == ROLE_GRILL && cur.target_c > 0)
            init_c = cur.target_c;
    } else if (has_ambient) {
        if (bbq_sensor_get(as, ah, &cur) && cur.target_c > 0)
            init_c = cur.target_c;
    }
    if (s_slider) lv_slider_set_value(s_slider, init_c / GRILL_STEP_C, LV_ANIM_OFF);
    if (s_temp_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d C", init_c);
        lv_label_set_text(s_temp_lbl, buf);
    }
}
