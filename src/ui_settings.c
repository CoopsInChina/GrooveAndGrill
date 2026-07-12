#include "ui_settings.h"
#include "ui_common.h"
#include "app_config.h"
#include "globals.h"
#include "wifi_manager.h"
#include "sonos_controller.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

// Settings carousel: About | WiFi | Speaker Setup | OTA | Screensaver (stub)
// Swipe left = next page, swipe right = prev page (or back to menu from About)

#define PAGE_COUNT  5
#define PAGE_WIFI           0
#define PAGE_SPEAKER_SETUP  1
#define PAGE_OTA            2
#define PAGE_SCREENSAVER    3
#define PAGE_ABOUT          4

static lv_obj_t *s_scr    = NULL;
static int        s_page  = 0;

// Screensaver page — updated by slider callbacks
static lv_obj_t *s_dim_val_lbl = NULL;
static lv_obj_t *s_ss_val_lbl  = NULL;

static lv_obj_t *s_pages[PAGE_COUNT] = {0};
static lv_obj_t *s_dots[PAGE_COUNT]  = {0};

static void show_page(int p)
{
    if (p < 0 || p >= PAGE_COUNT) return;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_pages[i]) {
            if (i == p) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        // Update dot colour to reflect active page
        if (s_dots[i]) {
            lv_obj_set_style_bg_color(s_dots[i],
                i == p ? COL_ACCENT : COL_BUTTON, 0);
        }
    }
    s_page = p;
}

static void go_next(void)
{
    if (s_page < PAGE_COUNT - 1) show_page(s_page + 1);
    else                          show_page(0);
}

static void go_prev(void)
{
    if (s_page > 0) show_page(s_page - 1);
    else            show_page(PAGE_COUNT - 1);
}

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(go_next, go_prev, NULL, NULL);
}

// ---- Page builders -------------------------------------------------

static lv_obj_t *make_page(lv_obj_t *parent, const char *title)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_center(p);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(p);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -100);

    return p;
}

static void build_about_page(lv_obj_t *p)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Groove & Grill\n\nv%s", FIRMWARE_VERSION);
    lv_obj_t *info = lv_label_create(p);
    lv_label_set_text(info, buf);
    lv_obj_set_style_text_color(info, COL_TEXT, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, 280);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *thanks = lv_label_create(p);
    lv_label_set_text(thanks, "Special thanks to AngryAngShanghai for the graphical assets");
    lv_obj_set_style_text_color(thanks, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(thanks, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(thanks, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(thanks, 280);
    lv_obj_align(thanks, LV_ALIGN_CENTER, 0, 65);
}

static void wifi_btn_cb(lv_event_t *e)
{
    ui_navigate_to(SCREEN_WIFI_SETUP);
}

static void build_wifi_page(lv_obj_t *p)
{
    const char *ssid = wifi_manager_ssid();
    char buf[96];
    if (strlen(ssid) > 0)
        snprintf(buf, sizeof(buf), "Connected:\n%s", ssid);
    else
        snprintf(buf, sizeof(buf), "Not connected");

    lv_obj_t *status = lv_label_create(p);
    lv_label_set_text(status, buf);
    lv_obj_set_style_text_color(status, COL_TEXT, 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(status, 260);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *btn = lv_btn_create(p);
    lv_obj_set_size(btn, 180, 48);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Setup WiFi");
    lv_obj_set_style_text_color(lbl, COL_BG, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
}

static void speaker_btn_cb(lv_event_t *e)
{
    ui_navigate_to(SCREEN_SPEAKER_SETUP);
}

static void build_speaker_page(lv_obj_t *p)
{
    const char *spkr = sonos_active_speaker_name();
    char buf[96];
    if (spkr && spkr[0])
        snprintf(buf, sizeof(buf), "Active:\n%s", spkr);
    else
        snprintf(buf, sizeof(buf), "No speaker found");

    lv_obj_t *info = lv_label_create(p);
    lv_label_set_text(info, buf);
    lv_obj_set_style_text_color(info, COL_TEXT, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, 260);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *btn = lv_btn_create(p);
    lv_obj_set_size(btn, 180, 48);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, speaker_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Choose Speaker");
    lv_obj_set_style_text_color(lbl, COL_BG, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
}

static void build_ota_page(lv_obj_t *p)
{
    lv_obj_t *info = lv_label_create(p);
    lv_label_set_text(info, "OTA update\n\nComing soon");
    lv_obj_set_style_text_color(info, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, 260);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 20);
}

// ---- Screensaver page callbacks ----------------------------------------

static void dim_switch_cb(lv_event_t *e)
{
    g_dim_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    globals_save_display();
}

static void dim_slider_cb(lv_event_t *e)
{
    g_autodim_sec = (int)lv_slider_get_value(lv_event_get_target(e));
    if (s_dim_val_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ds", g_autodim_sec);
        lv_label_set_text(s_dim_val_lbl, buf);
    }
    globals_save_display();
}

static void ss_switch_cb(lv_event_t *e)
{
    g_screensaver_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    globals_save_display();
}

static void ss_slider_cb(lv_event_t *e)
{
    int minutes = (int)lv_slider_get_value(lv_event_get_target(e));
    g_screensaver_sec = minutes * 60;
    if (s_ss_val_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d min", minutes);
        lv_label_set_text(s_ss_val_lbl, buf);
    }
    globals_save_display();
}

// ---- Screensaver page helpers ------------------------------------------

static void ss_make_row(lv_obj_t *parent, const char *text, int cy,
                         bool checked, lv_event_cb_t sw_cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 280, 38);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, cy);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_BUTTON, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_TEXT,   LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t *ss_make_slider(lv_obj_t *parent, int min, int max, int val,
                                  int cy, lv_event_cb_t sl_cb)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, 260, 12);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, cy);
    // Sliders bubble gestures to the parent by default, so a drag also gets
    // read as a screen swipe and flips to the next/prev settings page mid-drag.
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, COL_PANEL,  LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, sl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sl;
}

static lv_obj_t *ss_make_val_label(lv_obj_t *parent, const char *text, int cy)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, cy);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// ---- Screensaver page build --------------------------------------------

static void build_screensaver_page(lv_obj_t *p)
{
    // Dim on/off + timer (10 – 120 s)
    ss_make_row(p, "Dim", -55, g_dim_enabled, dim_switch_cb);
    ss_make_slider(p, 10, 120, g_autodim_sec, -15, dim_slider_cb);
    char dim_buf[16];
    snprintf(dim_buf, sizeof(dim_buf), "%ds", g_autodim_sec);
    s_dim_val_lbl = ss_make_val_label(p, dim_buf, +8);

    // Screensaver on/off + timer (1 – 30 min)
    ss_make_row(p, "Screensaver", +45, g_screensaver_enabled, ss_switch_cb);
    ss_make_slider(p, 1, 30, g_screensaver_sec / 60, +85, ss_slider_cb);
    char ss_buf[16];
    snprintf(ss_buf, sizeof(ss_buf), "%d min", g_screensaver_sec / 60);
    s_ss_val_lbl = ss_make_val_label(p, ss_buf, +108);
}

// ---- Create --------------------------------------------------------

lv_obj_t *ui_settings_create(void)
{
    s_page = 0;
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    // Order matches the PAGE_* indices above (About moved to the end).
    static const char *titles[PAGE_COUNT] = {
        "WiFi", "Speaker", "OTA Update", "Screensaver", "About"
    };

    s_pages[PAGE_ABOUT]         = make_page(s_scr, titles[PAGE_ABOUT]);
    s_pages[PAGE_WIFI]          = make_page(s_scr, titles[PAGE_WIFI]);
    s_pages[PAGE_SPEAKER_SETUP] = make_page(s_scr, titles[PAGE_SPEAKER_SETUP]);
    s_pages[PAGE_OTA]           = make_page(s_scr, titles[PAGE_OTA]);
    s_pages[PAGE_SCREENSAVER]   = make_page(s_scr, titles[PAGE_SCREENSAVER]);

    build_about_page(s_pages[PAGE_ABOUT]);
    build_wifi_page(s_pages[PAGE_WIFI]);
    build_speaker_page(s_pages[PAGE_SPEAKER_SETUP]);
    build_ota_page(s_pages[PAGE_OTA]);
    build_screensaver_page(s_pages[PAGE_SCREENSAVER]);

    // Page dots indicator — saved so show_page() can recolour them
    lv_obj_t *dots_cont = lv_obj_create(s_scr);
    lv_obj_set_size(dots_cont, 140, 16);
    lv_obj_align(dots_cont, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(dots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots_cont, 0, 0);
    lv_obj_set_style_pad_all(dots_cont, 0, 0);
    lv_obj_set_flex_flow(dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(dots_cont);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, i == 0 ? COL_ACCENT : COL_BUTTON, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_hor(dot, 4, 0);
        s_dots[i] = dot;
    }

    // Home button LAST → highest z-order, sits above full-screen page containers.
    ui_add_home_btn(s_scr);

    show_page(PAGE_WIFI);   // land on the first page (About is now last)
    return s_scr;
}
