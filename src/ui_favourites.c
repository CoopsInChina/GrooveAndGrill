#include "ui_favourites.h"
#include "ui_common.h"
#include "ui_art.h"
#include "sonos_controller.h"
#include <stddef.h>
#include "wifi_manager.h"
#include "app_config.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr          = NULL;
static lv_obj_t   *s_add_btn      = NULL;
static lv_obj_t   *s_qr           = NULL;
static lv_obj_t   *s_qr_url       = NULL;
static lv_obj_t   *s_art_img      = NULL;
static lv_timer_t *s_refresh_timer = NULL;
static lv_timer_t *s_art_timer     = NULL;
static int         s_index         = 0;
static int         s_last_count    = -1;
static bool        s_gesture_fired = false;

// Page-dot indicator — one dot per favourite slot plus the add slot,
// matching the settings screen's carousel style.
static lv_obj_t *s_dots_cont            = NULL;
static lv_obj_t *s_dots[MAX_FAVOURITES + 1] = {0};
static int       s_dot_count            = 0;

static void rebuild_dots(int total)
{
    if (!s_dots_cont || total == s_dot_count) return;
    if (total > MAX_FAVOURITES + 1) total = MAX_FAVOURITES + 1;

    lv_obj_clean(s_dots_cont);
    for (int i = 0; i < total; i++) {
        lv_obj_t *dot = lv_obj_create(s_dots_cont);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, COL_BUTTON, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_hor(dot, 2, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_dots[i] = dot;
    }
    s_dot_count = total;
}

static void update_dots(int active)
{
    for (int i = 0; i < s_dot_count; i++) {
        if (s_dots[i])
            lv_obj_set_style_bg_color(s_dots[i], i == active ? COL_ACCENT : COL_BUTTON, 0);
    }
}

// ---- Navigation ----------------------------------------------------

static void go_next(void)
{
    s_gesture_fired = true;
    int count = sonos_device_fav_count();
    s_index++;
    if (s_index > count) {
        ui_navigate_to(SCREEN_SONOS);
        s_index    = 0;
        s_last_count = -1;  // scr_loaded_cb re-renders when we come back
        return;
    }
    ui_favourites_show_index(s_index);
}

static void go_prev(void)
{
    s_gesture_fired = true;
    if (s_index > 0) {
        s_index--;
        ui_favourites_show_index(s_index);
    } else {
        ui_navigate_to(SCREEN_SONOS);
    }
}

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(go_next, go_prev, NULL, NULL);
}

// Tap (not swipe) anywhere on the screen plays the current favourite.
static void screen_tap_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    if (s_index < sonos_device_fav_count()) {
        sonos_play_device_favourite(s_index);
        ui_navigate_to(SCREEN_SONOS);
    }
}

// ---- Add slot — show QR code when + pressed ------------------------

static void add_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    if (!s_qr || !s_qr_url) return;

    const char *ip = wifi_manager_ip();
    if (!ip || !ip[0]) return;

    static char url[64];
    snprintf(url, sizeof(url), "http://%s/setup", ip);

    lv_obj_add_flag(s_add_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qr,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qr_url, LV_OBJ_FLAG_HIDDEN);

    lv_qrcode_update(s_qr, url, strlen(url));
    lv_label_set_text(s_qr_url, url);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    int count = sonos_device_fav_count();
    if (count != s_last_count) {
        if (s_index >= count && count > 0) s_index = 0;
        ui_favourites_show_index(s_index);
    }
}

// Called when this screen finishes loading (animation complete).
// Re-renders the current index so returning from Sonos main feels instant.
static void scr_loaded_cb(lv_event_t *e)
{
    ui_favourites_show_index(s_index);
}

static void art_timer_cb(lv_timer_t *t)
{
    if (!s_art_img || lv_scr_act() != s_scr) return;
    if (!ui_art_update_blob(s_art_img)) return;
    // Only reveal art if we're on a real favourite — not the add slot.
    if (s_index < sonos_device_fav_count())
        lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
}

static void scr_del_cb(lv_event_t *e)
{
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    if (s_art_timer)     { lv_timer_del(s_art_timer);     s_art_timer     = NULL; }
    s_scr = NULL;
    s_add_btn = s_qr = s_qr_url = s_art_img = NULL;
    s_dots_cont = NULL;
    s_dot_count = 0;
    memset(s_dots, 0, sizeof(s_dots));
}

// ---- Create --------------------------------------------------------

lv_obj_t *ui_favourites_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,    LV_EVENT_GESTURE,       NULL);
    lv_obj_add_event_cb(s_scr, screen_tap_cb, LV_EVENT_CLICKED,       NULL);
    lv_obj_add_event_cb(s_scr, scr_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,        NULL);

    // Album art — full size (300×300), centered, behind everything
    s_art_img = lv_img_create(s_scr);
    lv_obj_center(s_art_img);
    lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // "Favourites" category label — near top, above art
    lv_obj_t *cat = lv_label_create(s_scr);
    lv_label_set_text(cat, "Favourites");
    lv_obj_set_style_text_color(cat, COL_ACCENT, 0);
    lv_obj_set_style_text_font(cat, &lv_font_montserrat_24, 0);
    lv_obj_align(cat, LV_ALIGN_CENTER, 0, -170);
    lv_obj_clear_flag(cat, LV_OBJ_FLAG_CLICKABLE);

    // Page-dot indicator — bottom of screen, one dot per slot
    s_dots_cont = lv_obj_create(s_scr);
    lv_obj_set_size(s_dots_cont, 260, 16);
    lv_obj_align(s_dots_cont, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_opa(s_dots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots_cont, 0, 0);
    lv_obj_set_style_pad_all(s_dots_cont, 0, 0);
    lv_obj_set_flex_flow(s_dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_dots_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // + button — fully centered, 120×120
    s_add_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_add_btn, 120, 120);
    lv_obj_center(s_add_btn);
    lv_obj_set_style_radius(s_add_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_add_btn, COL_PANEL, 0);
    lv_obj_set_style_border_color(s_add_btn, COL_ACCENT, 0);
    lv_obj_set_style_border_width(s_add_btn, 2, 0);
    lv_obj_set_style_shadow_width(s_add_btn, 0, 0);
    lv_obj_add_event_cb(s_add_btn, add_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_icon = lv_label_create(s_add_btn);
    lv_label_set_text(add_icon, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(add_icon, COL_ACCENT, 0);
    lv_obj_set_style_text_font(add_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(add_icon);

    // QR code — hidden until + pressed (180×180, centred)
    s_qr = lv_qrcode_create(s_scr, 180,
                             lv_color_make(0, 0, 0),
                             lv_color_make(255, 255, 255));
    lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

    // URL text below QR — hidden until + pressed
    s_qr_url = lv_label_create(s_scr);
    lv_label_set_text(s_qr_url, "");
    lv_obj_set_style_text_color(s_qr_url, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_qr_url, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_qr_url, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_qr_url, LV_ALIGN_CENTER, 0, 115);
    lv_obj_add_flag(s_qr_url, LV_OBJ_FLAG_HIDDEN);

    s_last_count    = -1;
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 3000, NULL);
    s_art_timer     = lv_timer_create(art_timer_cb,     500,  NULL);

    ui_favourites_show_index(0);

    // Home button last → topmost z-order. Nudged up a little for spacing.
    lv_obj_t *home_btn = ui_add_home_btn(s_scr);
    lv_obj_align(home_btn, LV_ALIGN_CENTER, 0, -198);
    return s_scr;
}

// ---- Show ----------------------------------------------------------

void ui_favourites_show_index(int index)
{
    s_index = index;
    if (!s_scr) return;

    int count    = sonos_device_fav_count();
    s_last_count = count;
    bool is_add  = (index >= count);

    rebuild_dots(count + 1);
    update_dots(index);

    // Always reset QR to hidden when changing slots
    if (s_qr)     lv_obj_add_flag(s_qr,     LV_OBJ_FLAG_HIDDEN);
    if (s_qr_url) lv_obj_add_flag(s_qr_url, LV_OBJ_FLAG_HIDDEN);

    if (is_add) {
        ui_art_request(NULL);
        if (s_art_img) lv_obj_add_flag(s_art_img,   LV_OBJ_FLAG_HIDDEN);
        if (s_add_btn) lv_obj_clear_flag(s_add_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        const uint8_t *art_data = sonos_device_fav_art_data(index);
        size_t         art_sz   = sonos_device_fav_art_size(index);

        if (art_sz > 0 && art_data) {
            ui_art_request_blob(art_data, art_sz);
            if (s_art_img) lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_art_request(NULL);
            if (s_art_img) lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
        }

        if (s_add_btn) lv_obj_add_flag(s_add_btn, LV_OBJ_FLAG_HIDDEN);
    }
}
