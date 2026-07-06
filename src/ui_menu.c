#include "ui_menu.h"
#include "ui_common.h"
#include "ui_images.h"
#include "lvgl.h"

// Two main icons (Music + BBQ) side-by-side at screen centre, 120×120.
// Settings button at the bottom, same size as home button on other screens.
#define BTN_SIZE    120
#define BTN_OFFSET   80   // half of centre-to-centre distance → 40px gap between icons

typedef struct {
    const lv_img_dsc_t *icon;
    screen_id_t         target;
} menu_item_t;

static const menu_item_t ITEMS[] = {
    { &img_music, SCREEN_SONOS },
    { &img_bbq,   SCREEN_BBQ   },
};
#define ITEM_COUNT  2

static void btn_cb(lv_event_t *e)
{
    screen_id_t *target = (screen_id_t *)lv_event_get_user_data(e);
    ui_navigate_to(*target);
}

static void go_settings(void) { ui_navigate_to(SCREEN_SETTINGS); }

lv_obj_t *ui_menu_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_screen_base_style(scr);

    static screen_id_t targets[ITEM_COUNT];

    // Music (left) and BBQ (right), vertically centred on screen
    int offsets[ITEM_COUNT] = { -BTN_OFFSET, +BTN_OFFSET };

    for (int i = 0; i < ITEM_COUNT; i++) {
        targets[i] = ITEMS[i].target;

        lv_obj_t *btn = lv_btn_create(scr);
        lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
        lv_obj_set_pos(btn, CX + offsets[i] - BTN_SIZE / 2,
                            CY              - BTN_SIZE / 2);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, &targets[i]);

        lv_obj_t *img = lv_img_create(btn);
        lv_img_set_src(img, ITEMS[i].icon);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }

    // Settings button — bottom of screen, same style as home button on other screens
    lv_obj_t *set_btn = lv_btn_create(scr);
    lv_obj_set_size(set_btn, 60, 60);
    lv_obj_align(set_btn, LV_ALIGN_CENTER, 0, 185);
    lv_obj_set_style_radius(set_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(set_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(set_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(set_btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(set_btn, 0, 0);
    lv_obj_set_style_border_width(set_btn, 0, 0);
    lv_obj_add_event_cb(set_btn, (lv_event_cb_t)go_settings, LV_EVENT_CLICKED, NULL);

    lv_obj_t *set_img = lv_img_create(set_btn);
    lv_img_set_src(set_img, &img_settings);
    lv_obj_center(set_img);
    lv_obj_clear_flag(set_img, LV_OBJ_FLAG_CLICKABLE);

    return scr;
}
