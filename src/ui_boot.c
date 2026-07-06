#include "ui_boot.h"
#include "ui_common.h"
#include "app_config.h"
#include "lvgl.h"
#include <stdio.h>

// Forward-declared in img_boot.c (auto-generated from Boot.png)
LV_IMG_DECLARE(img_boot);

static lv_obj_t *s_scr    = NULL;
static lv_obj_t *s_arc    = NULL;
static lv_obj_t *s_status = NULL;

// Three status indicator dots: [0]=WiFi  [1]=Sonos  [2]=Server
static lv_obj_t *s_dot[3]      = {NULL, NULL, NULL};
static lv_obj_t *s_dot_lbl[3]  = {NULL, NULL, NULL};

// Icon x-offsets: centred at 0, spaced 95px apart
static const int DOT_X[3] = { -95, 0, 95 };
static const char *DOT_NAME[3] = { "WiFi", "Sonos", "API" };

static void make_dot(int idx)
{
    // Filled circle used as indicator
    lv_obj_t *d = lv_obj_create(s_scr);
    lv_obj_set_size(d, 28, 28);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, COL_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(d, LV_ALIGN_CENTER, DOT_X[idx], 68);
    s_dot[idx] = d;

    lv_obj_t *lbl = lv_label_create(s_scr);
    lv_label_set_text(lbl, DOT_NAME[idx]);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, DOT_X[idx], 96);
    s_dot_lbl[idx] = lbl;
}

lv_obj_t *ui_boot_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);

    // ---- Logo ----
    lv_obj_t *logo = lv_img_create(s_scr);
    lv_img_set_src(logo, &img_boot);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -70);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);

    // ---- Version ----
    lv_obj_t *ver = lv_label_create(s_scr);
    lv_label_set_text(ver, "v" FIRMWARE_VERSION);
    lv_obj_set_style_text_color(ver, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 40);

    // ---- Progress arc (thin ring at screen edge) ----
    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, 440, 440);
    lv_obj_center(s_arc);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_rotation(s_arc, 270);

    lv_obj_set_style_arc_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 5, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, 0);

    // ---- Status indicator dots ----
    make_dot(BOOT_ICON_WIFI);
    make_dot(BOOT_ICON_SONOS);
    make_dot(BOOT_ICON_SERVER);

    // ---- Status text ----
    s_status = lv_label_create(s_scr);
    lv_label_set_text(s_status, "Starting...");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status, 300);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 130);

    lv_scr_load(s_scr);
    return s_scr;
}

void ui_boot_show(void)
{
    if (!s_scr)
        ui_boot_create();
    else
        lv_scr_load(s_scr);
}

void ui_boot_set_status(const char *msg, int progress_pct)
{
    if (!s_scr) return;
    if (s_status) lv_label_set_text(s_status, msg);
    if (s_arc)    lv_arc_set_value(s_arc, progress_pct);
    lv_refr_now(NULL);
}

void ui_boot_set_icon(int idx, int state)
{
    if (!s_scr || idx < 0 || idx >= 3) return;
    lv_color_t col;
    switch (state) {
        case BOOT_STATE_OK:   col = COL_ACCENT;                  break;
        case BOOT_STATE_FAIL: col = lv_color_hex(0xFF3333);      break;
        default:              col = COL_TEXT_DIM;                break;
    }
    if (s_dot[idx])     lv_obj_set_style_bg_color(s_dot[idx], col, 0);
    if (s_dot_lbl[idx]) lv_obj_set_style_text_color(s_dot_lbl[idx], col, 0);
    lv_refr_now(NULL);
}
