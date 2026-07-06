#include "ui_speaker_setup.h"
#include "ui_common.h"
#include "sonos_controller.h"
#include "lvgl.h"
#include <stdio.h>

#define MAX_VISIBLE_SPEAKERS 3

static lv_obj_t *s_scr         = NULL;
static lv_obj_t *s_btns[MAX_VISIBLE_SPEAKERS] = {0};
static lv_obj_t *s_lbls[MAX_VISIBLE_SPEAKERS] = {0};
static lv_obj_t *s_status_lbl  = NULL;
static lv_obj_t *s_whirl       = NULL;
static int        s_offset     = 0;

static void go_back(void) { ui_navigate_to(SCREEN_SETTINGS); }

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(NULL, go_back, NULL, NULL);
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
    ui_add_home_btn(s_scr);

    // Title
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Speaker Setup");
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -100);

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

    refresh_list();
    return s_scr;
}
