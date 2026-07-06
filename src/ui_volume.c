#include "ui_volume.h"
#include "ui_common.h"
#include "sonos_controller.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t   *s_scr           = NULL;
static lv_obj_t   *s_arc           = NULL;
static lv_obj_t   *s_vol_lbl       = NULL;
static lv_timer_t *s_timeout_timer = NULL;
static int         s_volume        = 50;

static void go_sonos(void) { ui_navigate_to(SCREEN_SONOS); }
static void go_menu(void)  { ui_navigate_to(SCREEN_MENU);  }

static void reset_timeout(void)
{
    if (s_timeout_timer) lv_timer_reset(s_timeout_timer);
}

static void update_display(void)
{
    if (s_arc)     lv_arc_set_value(s_arc, s_volume);
    if (s_vol_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", s_volume);
        lv_label_set_text(s_vol_lbl, buf);
    }
}

static void arc_changed_cb(lv_event_t *e)
{
    s_volume = lv_arc_get_value(s_arc);
    sonos_set_volume((uint8_t)s_volume);
    if (s_vol_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", s_volume);
        lv_label_set_text(s_vol_lbl, buf);
    }
    reset_timeout();
}

static void adjust_volume(int delta)
{
    s_volume += delta;
    if (s_volume < 0)   s_volume = 0;
    if (s_volume > 100) s_volume = 100;
    sonos_set_volume((uint8_t)s_volume);
    update_display();
    reset_timeout();
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    switch (dir) {
        case LV_DIR_TOP:    adjust_volume(+5);  break;
        case LV_DIR_BOTTOM: adjust_volume(-5);  break;
        case LV_DIR_RIGHT:  go_sonos();         break;
        default: break;
    }
}

static void any_touch_cb(lv_event_t *e)
{
    reset_timeout();
}

static void timeout_cb(lv_timer_t *t)
{
    go_sonos();
}

static void scr_del_cb(lv_event_t *e)
{
    if (s_timeout_timer) {
        lv_timer_del(s_timeout_timer);
        s_timeout_timer = NULL;
    }
    s_scr     = NULL;
    s_arc     = NULL;
    s_vol_lbl = NULL;
}

lv_obj_t *ui_volume_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,   LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr, any_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,   LV_EVENT_DELETE,  NULL);

    // Volume arc — draggable or press-to-set; 270° open at bottom
    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, 380, 380);
    lv_obj_center(s_arc);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, s_volume);
    lv_arc_set_bg_angles(s_arc, 135, 405);
    lv_arc_set_rotation(s_arc, 0);
    lv_obj_set_style_arc_color(s_arc, COL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 20, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_arc, COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 6, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_arc, arc_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // "VOLUME" label
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "VOLUME");
    lv_obj_set_style_text_color(title, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -55);

    // Volume number
    s_vol_lbl = lv_label_create(s_scr);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_volume);
    lv_label_set_text(s_vol_lbl, buf);
    lv_obj_set_style_text_color(s_vol_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_vol_lbl, &lv_font_montserrat_48, 0);
    lv_obj_center(s_vol_lbl);

    // Swipe hint
    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, LV_SYMBOL_UP "  " LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(hint, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 55);

    // Home/menu button
    lv_obj_t *menu_btn = lv_btn_create(s_scr);
    lv_obj_set_size(menu_btn, 44, 44);
    lv_obj_align(menu_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(menu_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(menu_btn, COL_BUTTON, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_add_event_cb(menu_btn, (lv_event_cb_t)go_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *menu_lbl = lv_label_create(menu_btn);
    lv_label_set_text(menu_lbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(menu_lbl, COL_TEXT_DIM, 0);
    lv_obj_center(menu_lbl);

    // 10 s inactivity → return to music screen
    s_timeout_timer = lv_timer_create(timeout_cb, 10000, NULL);
    lv_timer_set_repeat_count(s_timeout_timer, 1);

    return s_scr;
}

void ui_volume_update(void)
{
    sonos_track_t t;
    sonos_get_track(&t);
    if (t.valid) {
        s_volume = t.volume;
        update_display();
    }
}
