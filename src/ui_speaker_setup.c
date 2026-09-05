#include "ui_speaker_setup.h"
#include "ui_common.h"
#include "sonos_controller.h"
#include "app_config.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define MAX_VISIBLE_SPEAKERS 3
#define SEARCH_STACK_WORDS   6144

static lv_obj_t *s_scr         = NULL;
static lv_obj_t *s_btns[MAX_VISIBLE_SPEAKERS] = {0};
static lv_obj_t *s_lbls[MAX_VISIBLE_SPEAKERS] = {0};
static lv_obj_t *s_status_lbl  = NULL;
static lv_obj_t *s_whirl       = NULL;
static lv_obj_t *s_search_btn  = NULL;
static int        s_offset     = 0;

// Discovery is a blocking ~8s SSDP scan (sonos_controller_discover) — must
// run off the LVGL task or the whole UI freezes for that long, same class
// of bug as the WiFi setup portal's httpd-self-deadlock freeze. A
// background task does the blocking work; a poll timer (LVGL task context)
// picks up completion and refreshes the list.
static volatile bool s_searching    = false;
static volatile bool s_search_done  = false;
static lv_timer_t   *s_search_poll  = NULL;

static void refresh_list(void);

static void go_back(void) { ui_navigate_to(SCREEN_SETTINGS); }

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(NULL, go_back, NULL, NULL);
}

static void search_task(void *arg)
{
    (void)arg;
    sonos_controller_discover(SONOS_DISCOVERY_TIMEOUT_MS);
    s_search_done = true;
    vTaskDelete(NULL);
}

static void search_poll_cb(lv_timer_t *t)
{
    if (!s_search_done) return;
    s_search_done = false;
    s_searching   = false;

    if (s_whirl)      lv_obj_add_flag(s_whirl, LV_OBJ_FLAG_HIDDEN);
    if (s_search_btn) lv_obj_clear_flag(s_search_btn, LV_OBJ_FLAG_HIDDEN);
    refresh_list();

    lv_timer_del(s_search_poll);
    s_search_poll = NULL;
}

static void search_btn_cb(lv_event_t *e)
{
    if (s_searching) return;
    s_searching = true;

    lv_label_set_text(s_status_lbl, "Searching...");
    if (s_search_btn) lv_obj_add_flag(s_search_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_whirl)       lv_obj_clear_flag(s_whirl, LV_OBJ_FLAG_HIDDEN);

    xTaskCreate(search_task, "sonos_search", SEARCH_STACK_WORDS, NULL, 4, NULL);
    if (!s_search_poll) s_search_poll = lv_timer_create(search_poll_cb, 300, NULL);
}

// Screen objects are cached/reused (see ui_common.c), but guard against a
// stray timer surviving a screen teardown anyway.
static void scr_del_cb(lv_event_t *e)
{
    if (s_search_poll) {
        lv_timer_del(s_search_poll);
        s_search_poll = NULL;
    }
}

static void refresh_list(void)
{
    sonos_speaker_t speakers[SONOS_MAX_SPEAKERS];
    int count = sonos_get_speakers(speakers, SONOS_MAX_SPEAKERS);

    if (count == 0) {
        lv_label_set_text(s_status_lbl, "No speakers found.\nEnsure Sonos is on\nyour WiFi network.");
        for (int i = 0; i < MAX_VISIBLE_SPEAKERS; i++) {
            if (s_btns[i]) lv_obj_add_flag(s_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%d speaker%s found", count, count == 1 ? "" : "s");
    lv_label_set_text(s_status_lbl, buf);

    for (int i = 0; i < MAX_VISIBLE_SPEAKERS; i++) {
        int idx = s_offset + i;
        if (idx < count && s_btns[i]) {
            lv_obj_clear_flag(s_btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_lbls[i], speakers[idx].name);
        } else if (s_btns[i]) {
            lv_obj_add_flag(s_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

typedef struct { int slot; } btn_ctx_t;
static btn_ctx_t s_ctx[MAX_VISIBLE_SPEAKERS];

static void connect_cb(lv_event_t *e)
{
    btn_ctx_t *ctx = (btn_ctx_t *)lv_event_get_user_data(e);
    int idx = s_offset + ctx->slot;

    // Show connecting indicator
    if (s_whirl) lv_obj_clear_flag(s_whirl, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);

    sonos_select_speaker(idx);

    if (s_whirl) lv_obj_add_flag(s_whirl, LV_OBJ_FLAG_HIDDEN);

    if (sonos_is_connected()) {
        ui_navigate_to(SCREEN_SETTINGS);
    }
    // else: stay, show error (future enhancement)
}

lv_obj_t *ui_speaker_setup_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,  LV_EVENT_DELETE,  NULL);
    ui_add_home_btn(s_scr);

    s_searching   = false;
    s_search_done = false;
    s_search_poll = NULL;

    // Title
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Speaker Setup");
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -105);

    // Status
    s_status_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_status_lbl, "Searching...");
    lv_obj_set_style_text_color(s_status_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_lbl, 260);
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, -65);

    // Speaker buttons (up to 3 visible at once)
    static const int y_positions[MAX_VISIBLE_SPEAKERS] = { -25, 30, 85 };

    for (int i = 0; i < MAX_VISIBLE_SPEAKERS; i++) {
        s_ctx[i].slot = i;

        s_btns[i] = lv_btn_create(s_scr);
        lv_obj_set_size(s_btns[i], 240, 44);
        lv_obj_align(s_btns[i], LV_ALIGN_CENTER, 0, y_positions[i]);
        lv_obj_set_style_bg_color(s_btns[i], COL_BUTTON, 0);
        lv_obj_set_style_bg_color(s_btns[i], COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(s_btns[i], 22, 0);
        lv_obj_set_style_shadow_width(s_btns[i], 0, 0);
        lv_obj_set_style_border_width(s_btns[i], 0, 0);
        lv_obj_add_event_cb(s_btns[i], connect_cb, LV_EVENT_CLICKED, &s_ctx[i]);
        lv_obj_add_flag(s_btns[i], LV_OBJ_FLAG_HIDDEN);

        s_lbls[i] = lv_label_create(s_btns[i]);
        lv_label_set_text(s_lbls[i], "");
        lv_obj_set_style_text_color(s_lbls[i], COL_TEXT, 0);
        lv_obj_center(s_lbls[i]);
    }

    // Connecting spinner (hidden until needed)
    s_whirl = lv_spinner_create(s_scr, 1000, 60);
    lv_obj_set_size(s_whirl, 60, 60);
    lv_obj_center(s_whirl);
    lv_obj_set_style_arc_color(s_whirl, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(s_whirl, LV_OBJ_FLAG_HIDDEN);

    // Search again
    s_search_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_search_btn, 200, 44);
    lv_obj_align(s_search_btn, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_style_bg_color(s_search_btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(s_search_btn, 22, 0);
    lv_obj_set_style_shadow_width(s_search_btn, 0, 0);
    lv_obj_set_style_border_width(s_search_btn, 0, 0);
    lv_obj_add_event_cb(s_search_btn, search_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *search_lbl = lv_label_create(s_search_btn);
    lv_label_set_text(search_lbl, "Search Again");
    lv_obj_set_style_text_color(search_lbl, COL_BG, 0);
    lv_obj_set_style_text_font(search_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(search_lbl);

    refresh_list();
    return s_scr;
}
