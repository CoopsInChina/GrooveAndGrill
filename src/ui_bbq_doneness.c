#include "ui_bbq_doneness.h"
#include "ui_common.h"
#include "bbq_controller.h"
#include "meat_temps.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_slider     = NULL;
static lv_obj_t *s_level_lbl  = NULL;

static int         s_grill_idx      = 0;
static int         s_meat_type_idx  = 0;
static meat_kind_t s_meat_kind      = MEAT_KIND_NONE;
static int         s_grill_target_c = 0;

static void go_back(void) { ui_navigate_to(SCREEN_BBQ_CONFIG); }

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(NULL, go_back, NULL, NULL);
}

static void update_level_lbl(void)
{
    if (!s_level_lbl || s_meat_type_idx < 0 || s_meat_type_idx >= MEAT_TYPE_COUNT) return;
    const meat_type_t *mt = &MEAT_TYPES[s_meat_type_idx];
    int idx = lv_slider_get_value(s_slider);
    if (idx < 0) idx = 0;
    if (idx >= mt->level_count) idx = mt->level_count - 1;

    char buf[48];
    snprintf(buf, sizeof(buf), "%s\n%d C", mt->levels[idx].label, mt->levels[idx].target_c);
    lv_label_set_text(s_level_lbl, buf);
}

static void slider_changed_cb(lv_event_t *e)
{
    update_level_lbl();
}

static void confirm_btn_cb(lv_event_t *e)
{
    if (s_meat_type_idx < 0 || s_meat_type_idx >= MEAT_TYPE_COUNT) return;
    const meat_type_t *mt = &MEAT_TYPES[s_meat_type_idx];
    int idx = lv_slider_get_value(s_slider);
    if (idx < 0) idx = 0;
    if (idx >= mt->level_count) idx = mt->level_count - 1;

    bbq_set_targets(s_grill_idx, s_grill_target_c, mt->levels[idx].target_c, s_meat_kind);
    ui_navigate_to(SCREEN_BBQ);
}

lv_obj_t *ui_bbq_doneness_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    ui_add_home_btn(s_scr);

    // Styling matches the settings/screensaver screens: green accent title,
    // green slider, dim... (the level readout stays white for emphasis).
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "How would you like\nthat cooked?");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -110);

    s_slider = lv_slider_create(s_scr);
    lv_obj_set_size(s_slider, 260, 12);
    lv_obj_align(s_slider, LV_ALIGN_CENTER, 0, -30);
    // See ui_bbq_config.c — sliders bubble gestures to the parent by
    // default, which turns a drag into a phantom screen-swipe navigation.
    lv_obj_clear_flag(s_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(s_slider, COL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_level_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_align(s_level_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_level_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_level_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_level_lbl, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *confirm_btn = lv_btn_create(s_scr);
    lv_obj_set_size(confirm_btn, 200, 50);
    lv_obj_align(confirm_btn, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_style_radius(confirm_btn, 12, 0);
    lv_obj_set_style_bg_color(confirm_btn, COL_ACCENT, 0);
    lv_obj_set_style_shadow_width(confirm_btn, 0, 0);
    lv_obj_set_style_border_width(confirm_btn, 0, 0);
    lv_obj_add_event_cb(confirm_btn, confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, "CONFIRM");
    lv_obj_set_style_text_color(confirm_lbl, COL_BG, 0);
    lv_obj_set_style_text_font(confirm_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(confirm_lbl);

    return s_scr;
}

void ui_bbq_doneness_set_target(int grill_idx, int meat_type_idx, meat_kind_t kind,
                                 int grill_target_c)
{
    s_grill_idx      = grill_idx;
    s_meat_type_idx  = meat_type_idx;
    s_meat_kind      = kind;
    s_grill_target_c = grill_target_c;

    if (meat_type_idx < 0 || meat_type_idx >= MEAT_TYPE_COUNT || !s_slider) return;
    const meat_type_t *mt = &MEAT_TYPES[meat_type_idx];

    lv_slider_set_range(s_slider, 0, mt->level_count - 1);
    lv_slider_set_value(s_slider, mt->level_count - 1, LV_ANIM_OFF);   // default: most well-done
    update_level_lbl();
}
