#include "ui_wifi_setup.h"
#include "ui_common.h"
#include "wifi_manager.h"
#include "app_config.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static lv_obj_t   *s_scr         = NULL;
static lv_obj_t   *s_status      = NULL;
static lv_obj_t   *s_qr          = NULL;
static lv_obj_t   *s_ap_hint     = NULL;
static lv_obj_t   *s_url_hint    = NULL;
static lv_obj_t   *s_btn_label   = NULL;
static lv_timer_t *s_poll_timer  = NULL;

static bool s_ap_started   = false;
static bool s_creds_saved  = false;

static void go_back(void) { ui_navigate_to(SCREEN_SETTINGS); }

static void back_btn_cb(lv_event_t *e) { go_back(); }

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(NULL, go_back, NULL, NULL);
}

// One-shot timer fires 2 s after "Connected!" is shown.
static void nav_back_timer_cb(lv_timer_t *t)
{
    ui_navigate_to(SCREEN_SETTINGS);
}

// 1 s polling timer — tracks AP stopping (via HTTP portal) and WiFi connect.
// Runs in LVGL task context, safe to call LVGL directly.
static void wifi_poll_cb(lv_timer_t *t)
{
    // Detect when HTTP portal saved creds and stopped the AP behind our back
    if (s_ap_started && !wifi_manager_ap_active()) {
        s_ap_started  = false;
        s_creds_saved = true;

        if (s_qr)      lv_obj_add_flag(s_qr,      LV_OBJ_FLAG_HIDDEN);
        if (s_ap_hint) lv_obj_add_flag(s_ap_hint,  LV_OBJ_FLAG_HIDDEN);
        if (s_url_hint)lv_obj_add_flag(s_url_hint, LV_OBJ_FLAG_HIDDEN);
        if (s_status) {
            lv_label_set_text(s_status, "Credentials saved.\nConnecting to WiFi...");
            lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_btn_label) lv_label_set_text(s_btn_label, "Setup WiFi");
    }

    // After creds saved, wait for STA to connect
    if (s_creds_saved && wifi_manager_is_connected()) {
        s_creds_saved = false;

        if (s_status) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Connected!\n%s\n\nReturning to Settings...",
                     wifi_manager_ssid());
            lv_label_set_text(s_status, buf);
        }

        // Stop polling, navigate back in 2 s so user can read the message
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
        lv_timer_t *nav = lv_timer_create(nav_back_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(nav, 1);
    }
}

// Clean up the poll timer when the screen object is deleted
static void scr_del_cb(lv_event_t *e)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
}

static void setup_btn_cb(lv_event_t *e)
{
    if (!s_ap_started) {
        wifi_manager_start_setup_ap();
        s_ap_started  = true;
        s_creds_saved = false;

        // Generate the QR code here (not at screen-create) to keep app_main stack lean.
        // qrcodegen uses ~8 KB of stack — safe inside the LVGL task via button press.
        if (s_qr) {
            const char *qr_str = "WIFI:S:" WIFI_AP_SSID ";T:nopass;;";
            lv_qrcode_update(s_qr, qr_str, strlen(qr_str));
        }

        if (s_status)   lv_obj_add_flag(s_status,   LV_OBJ_FLAG_HIDDEN);
        if (s_qr)       lv_obj_clear_flag(s_qr,     LV_OBJ_FLAG_HIDDEN);
        if (s_ap_hint)  lv_obj_clear_flag(s_ap_hint,  LV_OBJ_FLAG_HIDDEN);
        if (s_url_hint) lv_obj_clear_flag(s_url_hint, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_label) lv_label_set_text(s_btn_label, "Stop Setup");
    } else {
        wifi_manager_stop_setup_ap();
        s_ap_started  = false;
        s_creds_saved = false;

        if (s_qr)      lv_obj_add_flag(s_qr,       LV_OBJ_FLAG_HIDDEN);
        if (s_ap_hint) lv_obj_add_flag(s_ap_hint,  LV_OBJ_FLAG_HIDDEN);
        if (s_url_hint)lv_obj_add_flag(s_url_hint, LV_OBJ_FLAG_HIDDEN);

        if (s_status) {
            const char *ssid = wifi_manager_ssid();
            if (strlen(ssid) > 0) {
                char buf[80];
                snprintf(buf, sizeof(buf), "Connected:\n%s", ssid);
                lv_label_set_text(s_status, buf);
            } else {
                lv_label_set_text(s_status, "Not connected");
            }
            lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_btn_label) lv_label_set_text(s_btn_label, "Setup WiFi");
    }
}

lv_obj_t *ui_wifi_setup_create(void)
{
    // Reset state each time screen is (re)created
    s_ap_started  = false;
    s_creds_saved = false;

    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,  LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,  LV_EVENT_DELETE,  NULL);

    // Back button — top-left quadrant, inside the circle
    lv_obj_t *back_btn = lv_btn_create(s_scr);
    lv_obj_set_size(back_btn, 110, 36);
    lv_obj_align(back_btn, LV_ALIGN_CENTER, -80, -185);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Settings");
    lv_obj_set_style_text_color(back_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(back_lbl);

    // Title
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "WiFi Setup");
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 25, -185);

    // Status text (visible when AP not active)
    const char *ssid = wifi_manager_ssid();
    char status_buf[80];
    if (strlen(ssid) > 0)
        snprintf(status_buf, sizeof(status_buf), "Connected:\n%s", ssid);
    else
        snprintf(status_buf, sizeof(status_buf), "Not connected.\nPress Setup WiFi to begin.");

    s_status = lv_label_create(s_scr);
    lv_label_set_text(s_status, status_buf);
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status, 280);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -70);

    // QR code widget — content is generated lazily when the AP starts (see setup_btn_cb)
    // so that lv_qrcode_update's 8 KB stack usage doesn't hit app_main at screen-create.
    s_qr = lv_qrcode_create(s_scr, 170,
                             lv_color_make(0, 0, 0),
                             lv_color_make(255, 255, 255));
    lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

    // AP name hint
    s_ap_hint = lv_label_create(s_scr);
    lv_label_set_text(s_ap_hint, "Scan to join  " WIFI_AP_SSID);
    lv_obj_set_style_text_color(s_ap_hint, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_ap_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_ap_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ap_hint, LV_ALIGN_CENTER, 0, 105);
    lv_obj_add_flag(s_ap_hint, LV_OBJ_FLAG_HIDDEN);

    // URL fallback hint
    s_url_hint = lv_label_create(s_scr);
    lv_label_set_text(s_url_hint, "Or visit  http://" WIFI_AP_IP);
    lv_obj_set_style_text_color(s_url_hint, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_url_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_url_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_url_hint, LV_ALIGN_CENTER, 0, 128);
    lv_obj_add_flag(s_url_hint, LV_OBJ_FLAG_HIDDEN);

    // Setup / Stop button
    lv_obj_t *btn = lv_btn_create(s_scr);
    lv_obj_set_size(btn, 210, 54);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 162);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn, 27, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, setup_btn_cb, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(btn);
    lv_label_set_text(s_btn_label, "Setup WiFi");
    lv_obj_set_style_text_color(s_btn_label, COL_BG, 0);
    lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_16, 0);
    lv_obj_center(s_btn_label);

    // 1 s poll timer to detect portal-triggered AP stop and WiFi connect
    s_poll_timer = lv_timer_create(wifi_poll_cb, 1000, NULL);

    return s_scr;
}
