#include "ui_bbq.h"
#include "ui_common.h"
#include "lvgl.h"

static void go_menu(void) { ui_navigate_to(SCREEN_MENU); }

static void gesture_cb(lv_event_t *e)
{
    ui_handle_gesture(NULL, go_menu, NULL, NULL);
}

lv_obj_t *ui_bbq_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_screen_base_style(scr);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    ui_add_home_btn(scr);

    // Decorative outer ring in BBQ orange
    lv_obj_t *ring = lv_obj_create(scr);
    lv_obj_set_size(ring, 400, 400);
    lv_obj_center(ring);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Icon
    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(icon, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BBQ");
    lv_obj_set_style_text_color(title, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Coming soon");
    lv_obj_set_style_text_color(sub, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 55);

    return scr;
}
