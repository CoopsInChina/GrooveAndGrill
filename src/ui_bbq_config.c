#include "ui_bbq_config.h"
#include "ui_common.h"
#include "ui_bbq_doneness.h"
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

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_title_lbl  = NULL;
static lv_obj_t *s_slider     = NULL;
static lv_obj_t *s_temp_lbl   = NULL;
static int       s_target_idx = 0;

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

static void slider_changed_cb(lv_event_t *e)
{
    int c = slider_celsius();
    // Apply the grill target immediately as the user drags.
    bbq_set_grill_target(s_target_idx, c);
    if (s_temp_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d C", c);
        lv_label_set_text(s_temp_lbl, buf);
    }
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
    int grill_target_c = slider_celsius();

    if (info->kind == MEAT_KIND_CHICKEN) {
        // Poultry has one food-safety target — no doneness preference to pick.
        bbq_set_targets(s_target_idx, grill_target_c, CHICKEN_SAFE_TARGET_C, MEAT_KIND_CHICKEN);
        ui_navigate_to(SCREEN_BBQ);
        return;
    }

    int meat_idx = find_meat_type_index(info->name);
    if (meat_idx < 0) return;   // shouldn't happen — button names match MEAT_TYPES

    ui_navigate_to(SCREEN_BBQ_DONENESS);
    ui_bbq_doneness_set_target(s_target_idx, meat_idx, info->kind, grill_target_c);
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

    // Title carries the grill number ("Grill N Config"), set in
    // ui_bbq_config_set_target(). Placeholder text until then.
    // Styling matches the settings/screensaver screens: green accent title,
    // white montserrat_20 control label, green slider, dim value label.
    s_title_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_title_lbl, "Grill Config");
    lv_obj_set_style_text_color(s_title_lbl, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -140);

    // ---- Target grill temp ----
    lv_obj_t *slider_lbl = lv_label_create(s_scr);
    lv_label_set_text(slider_lbl, "Target Grill Temp");
    lv_obj_set_style_text_color(slider_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(slider_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(slider_lbl, LV_ALIGN_CENTER, 0, -100);

    s_slider = lv_slider_create(s_scr);
    lv_obj_set_size(s_slider, 200, 12);
    lv_obj_align(s_slider, LV_ALIGN_CENTER, -30, -70);
    // Sliders bubble gestures to the parent by default, so a drag also
    // gets read as a screen swipe (see go_back() above) and navigates away
    // mid-drag. Stop that here.
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

    // ---- Meat selection grid (2x2). 120px icons with ~20px gaps in both
    // directions: centres 144 apart horizontally, 140 apart vertically. ----
    make_meat_btn(s_scr, &s_meat_btns[0], -72, 10);
    make_meat_btn(s_scr, &s_meat_btns[1],  72, 10);
    make_meat_btn(s_scr, &s_meat_btns[2], -72, 150);
    make_meat_btn(s_scr, &s_meat_btns[3],  72, 150);

    return s_scr;
}

void ui_bbq_config_set_target(int grill_idx)
{
    s_target_idx = grill_idx;
    if (s_title_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Grill %d Config", grill_idx + 1);
        lv_label_set_text(s_title_lbl, buf);
    }

    if (!s_slider) return;
    const bbq_grill_t *g = bbq_get_grill(grill_idx);
    // Reset to a sane default rather than leaving whatever another grill's
    // visit left the slider at — this screen is cached and reused.
    int grill_target_c = (g && g->configured) ? g->grill_target_c : 200;
    lv_slider_set_value(s_slider, grill_target_c / GRILL_STEP_C, LV_ANIM_OFF);
    if (s_temp_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d C", grill_target_c);
        lv_label_set_text(s_temp_lbl, buf);
    }
}
