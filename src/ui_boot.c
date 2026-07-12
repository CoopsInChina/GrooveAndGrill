#include "ui_boot.h"
#include "ui_common.h"
#include "app_config.h"
#include "lvgl.h"
#include <stdio.h>

// Forward-declared in img_boot.c (auto-generated from assets/Boot.jpg, full 480x480 background)
LV_IMG_DECLARE(img_boot);

static lv_obj_t *s_scr       = NULL;
static lv_obj_t *s_status    = NULL;
static lv_obj_t *s_status_bg = NULL;

// Three status icons: [0]=WiFi  [1]=Sonos  [2]=Server
static lv_obj_t *s_icon[3] = {NULL, NULL, NULL};

static const char *ICON_SYMBOL[3] = { LV_SYMBOL_WIFI, LV_SYMBOL_AUDIO, LV_SYMBOL_DRIVE };

// Icon positions, matched to the artwork in assets/Boot.jpg: Sonos sits dead
// centre on the vinyl/speaker graphic, WiFi and Server flank the grill.
static const int ICON_X[3] = { -165, 0, 165 };
static const int ICON_Y[3] = { 6,    0, 6   };

static void make_icon(int idx)
{
    lv_obj_t *ic = lv_label_create(s_scr);
    lv_label_set_text(ic, ICON_SYMBOL[idx]);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
    lv_obj_align(ic, LV_ALIGN_CENTER, ICON_X[idx], ICON_Y[idx]);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    s_icon[idx] = ic;
}

lv_obj_t *ui_boot_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);

    // ---- Full-screen boot artwork ----
    lv_obj_t *bg = lv_img_create(s_scr);
    lv_img_set_src(bg, &img_boot);
    lv_obj_center(bg);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

    // ---- Status indicator icons ----
    make_icon(BOOT_ICON_WIFI);
    make_icon(BOOT_ICON_SONOS);
    make_icon(BOOT_ICON_SERVER);

    // ---- Status text — near the bottom, on a translucent backdrop so it
    // stays legible over the artwork underneath ----
    s_status_bg = lv_obj_create(s_scr);
    lv_obj_set_size(s_status_bg, 340, 34);
    lv_obj_align(s_status_bg, LV_ALIGN_CENTER, 0, 205);
    lv_obj_set_style_radius(s_status_bg, 8, 0);
    lv_obj_set_style_bg_color(s_status_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_status_bg, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_status_bg, 0, 0);
    lv_obj_clear_flag(s_status_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(s_status_bg);
    lv_label_set_text(s_status, "Starting...");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status, 320);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_center(s_status);

    // ---- Version — bottom-right corner, outside the circular artwork ----
    lv_obj_t *ver = lv_label_create(s_scr);
    lv_label_set_text(ver, "v" FIRMWARE_VERSION);
    lv_obj_set_style_text_color(ver, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_align(ver, LV_ALIGN_CENTER, 175, 195);

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
    (void)progress_pct;
    if (!s_scr) return;
    if (s_status) lv_label_set_text(s_status, msg);
    lv_refr_now(NULL);
}

void ui_boot_set_icon(int idx, int state)
{
    if (!s_scr || idx < 0 || idx >= 3) return;
    lv_color_t col;
    switch (state) {
        case BOOT_STATE_OK:   col = COL_ACCENT;                  break;
        case BOOT_STATE_FAIL: col = lv_color_hex(0xFF3333);      break;
        default:              col = lv_color_hex(0x444444);      break;
    }
    if (s_icon[idx]) lv_obj_set_style_text_color(s_icon[idx], col, 0);
    lv_refr_now(NULL);
}
