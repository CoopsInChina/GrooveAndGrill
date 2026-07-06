#include "ui_sonos_main.h"
#include "ui_common.h"
#include "sonos_controller.h"
#include "ui_art.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr         = NULL;
static lv_obj_t   *s_title       = NULL;
static lv_obj_t   *s_artist      = NULL;
static lv_obj_t   *s_prog_arc    = NULL;
static lv_obj_t   *s_speaker_lbl = NULL;
static lv_obj_t   *s_art_img     = NULL;
static lv_timer_t *s_poll_timer  = NULL;
static char        s_last_art_url[512] = {0};

// ---- Callbacks ----------------------------------------------------

static void go_favourites(void) { ui_navigate_to(SCREEN_FAVOURITES); }
static void go_volume(void)     { ui_navigate_to(SCREEN_VOLUME);     }

static bool s_gesture_fired = false;

static void gesture_cb(lv_event_t *e)
{
    s_gesture_fired = true;
    ui_handle_gesture(go_favourites, NULL, sonos_next, sonos_prev);
}

static void screen_tapped_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    sonos_toggle_play_pause();
}

static void poll_timer_cb(lv_timer_t *t)
{
    ui_sonos_main_update();
}

static void scr_del_cb(lv_event_t *e)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    s_art_img = NULL;
    s_last_art_url[0] = '\0';
}

// ---- Create --------------------------------------------------------

lv_obj_t *ui_sonos_main_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,      LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr, screen_tapped_cb, LV_EVENT_CLICKED,  NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,       LV_EVENT_DELETE,   NULL);

    // Home button — top centre, created first so art/arc don't cover it
    ui_add_home_btn(s_scr);

    // Active speaker label (below home btn)
    s_speaker_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_speaker_lbl, "");
    lv_obj_set_style_text_color(s_speaker_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_speaker_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_speaker_lbl, LV_ALIGN_CENTER, 0, -140);
    lv_obj_clear_flag(s_speaker_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Album art — centre; non-clickable so taps pass through to screen
    s_art_img = lv_img_create(s_scr);
    lv_obj_center(s_art_img);
    lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Progress arc — transparent background, accent indicator
    s_prog_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_prog_arc, 440, 440);
    lv_obj_center(s_prog_arc);
    lv_arc_set_range(s_prog_arc, 0, 100);
    lv_arc_set_value(s_prog_arc, 0);
    lv_arc_set_bg_angles(s_prog_arc, 0, 360);
    lv_arc_set_rotation(s_prog_arc, 270);
    lv_obj_set_style_arc_opa(s_prog_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_prog_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_prog_arc, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_prog_arc, 4, LV_PART_INDICATOR);
    lv_obj_remove_style(s_prog_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_prog_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_prog_arc, LV_OPA_TRANSP, 0);

    // Volume button — bottom centre
    lv_obj_t *vol_btn = lv_btn_create(s_scr);
    lv_obj_set_size(vol_btn, 60, 60);
    lv_obj_align(vol_btn, LV_ALIGN_CENTER, 0, 185);
    lv_obj_set_style_radius(vol_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(vol_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(vol_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(vol_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(vol_btn, 0, 0);
    lv_obj_set_style_border_width(vol_btn, 0, 0);
    lv_obj_add_event_cb(vol_btn, (lv_event_cb_t)go_volume, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vol_lbl = lv_label_create(vol_btn);
    lv_label_set_text(vol_lbl, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(vol_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(vol_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(vol_lbl);

    // Track title
    s_title = lv_label_create(s_scr);
    lv_label_set_text(s_title, "Connecting...");
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title, 300);
    lv_obj_set_style_text_color(s_title, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 110);
    lv_obj_clear_flag(s_title, LV_OBJ_FLAG_CLICKABLE);

    // Artist
    s_artist = lv_label_create(s_scr);
    lv_label_set_text(s_artist, "");
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_artist, 280);
    lv_obj_set_style_text_color(s_artist, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_artist, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_artist, LV_ALIGN_CENTER, 0, 137);
    lv_obj_clear_flag(s_artist, LV_OBJ_FLAG_CLICKABLE);

    // 2 s poll timer
    s_poll_timer = lv_timer_create(poll_timer_cb, 2000, NULL);

    return s_scr;
}

void ui_sonos_main_update(void)
{
    if (!s_scr) return;

    sonos_track_t t;
    sonos_get_track(&t);

    if (t.valid) {
        lv_label_set_text(s_title,  t.title[0]  ? t.title  : "Unknown");
        lv_label_set_text(s_artist, t.artist[0] ? t.artist : "");
        lv_arc_set_value(s_prog_arc, 0);   // progress parser pending

        if (t.album_art_url[0] &&
            strncmp(t.album_art_url, s_last_art_url, sizeof(s_last_art_url)) != 0) {
            strncpy(s_last_art_url, t.album_art_url, sizeof(s_last_art_url) - 1);
            ui_art_request(t.album_art_url);
        }
    } else {
        lv_label_set_text(s_title,  "Connecting...");
        lv_label_set_text(s_artist, "");
    }

    if (s_art_img && lv_scr_act() == s_scr) ui_art_update(s_art_img);

    lv_label_set_text(s_speaker_lbl, sonos_active_speaker_name());
}
