#include "ui_common.h"
#include "globals.h"
#include "ui_boot.h"
#include "ui_menu.h"
#include "ui_sonos_main.h"
#include "ui_favourites.h"
#include "ui_volume.h"
#include "ui_settings.h"
#include "ui_wifi_setup.h"
#include "ui_speaker_setup.h"
#include "ui_bbq.h"
#include "ui_bbq_config.h"
#include "ui_bbq_doneness.h"
#include "ui_widgets.h"

#include "esp_log.h"

static const char *TAG = "ui_common";

// ---- Screen registry ------------------------------------------------

static lv_obj_t *s_screens[SCREEN_COUNT] = {0};
static screen_id_t s_current = SCREEN_BOOT;
static lv_obj_t *s_banner = NULL;

typedef lv_obj_t *(*screen_create_fn_t)(void);

static screen_create_fn_t s_create_fns[SCREEN_COUNT] = {
    [SCREEN_BOOT]          = ui_boot_create,
    [SCREEN_MENU]          = ui_menu_create,
    [SCREEN_SONOS]         = ui_sonos_main_create,
    [SCREEN_FAVOURITES]    = ui_favourites_create,
    [SCREEN_VOLUME]        = ui_volume_create,
    [SCREEN_SETTINGS]      = ui_settings_create,
    [SCREEN_WIFI_SETUP]    = ui_wifi_setup_create,
    [SCREEN_SPEAKER_SETUP] = ui_speaker_setup_create,
    [SCREEN_BBQ]           = ui_bbq_create,
    [SCREEN_BBQ_CONFIG]    = ui_bbq_config_create,
    [SCREEN_BBQ_DONENESS]  = ui_bbq_doneness_create,
    [SCREEN_WIDGETS]       = ui_widgets_create,
};

void ui_navigate_to(screen_id_t id)
{
    if (id >= SCREEN_COUNT) return;

    bool first_visit = !s_screens[id];
    if (!s_screens[id]) {
        s_screens[id] = s_create_fns[id]();
        if (!s_screens[id]) {
            ESP_LOGE(TAG, "Failed to create screen %d", id);
            return;
        }
    }

    lv_scr_load_anim(s_screens[id], LV_SCR_LOAD_ANIM_FADE_ON, 250, 0, false);
    s_current = id;

    // Screens are cached forever once created (see s_screens[] above), so
    // LVGL's own object/style pool only trends downward as new screens get
    // first-visited. That pool (CONFIG_LV_MEM_SIZE_KILOBYTES) is separate
    // from the general ESP-IDF heap — general DRAM/PSRAM free doesn't move
    // when LVGL objects are created, so log LVGL's own pool instead.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Navigate -> screen %d (%s) — LVGL pool: %u%% used, %u%% frag, "
             "free: %u bytes (biggest block: %u)",
             id, first_visit ? "first visit, just created" : "cached",
             mon.used_pct, mon.frag_pct, (unsigned)mon.free_size,
             (unsigned)mon.free_biggest_size);
}

void ui_screen_invalidate(screen_id_t id)
{
    if (id >= SCREEN_COUNT) return;
    if (s_screens[id] && s_current != id) {
        lv_obj_del(s_screens[id]);
        s_screens[id] = NULL;
    }
}

// ---- Reconnecting banner --------------------------------------------

void ui_show_reconnecting_banner(void)
{
    if (s_banner) return;

    s_banner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_banner, 220, 44);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_color(s_banner, COL_WARN, 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_banner, 22, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(s_banner);
    lv_label_set_text(lbl, "Reconnecting...");
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_center(lbl);
}

void ui_hide_reconnecting_banner(void)
{
    if (!s_banner) return;
    lv_obj_del(s_banner);
    s_banner = NULL;
}

// ---- Screensaver watchdog ------------------------------------------

void ui_touch_activity(void)
{
    lv_disp_trig_activity(NULL);
    globals_touch_activity();   // resets autodim timer and wakes screen if dimmed
}

// ---- Gesture helper ------------------------------------------------

void ui_handle_gesture(gesture_cb_t on_left, gesture_cb_t on_right,
                        gesture_cb_t on_up,   gesture_cb_t on_down)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    switch (dir) {
        case LV_DIR_LEFT:   if (on_left)  on_left();  break;
        case LV_DIR_RIGHT:  if (on_right) on_right(); break;
        case LV_DIR_TOP:    if (on_up)    on_up();    break;
        case LV_DIR_BOTTOM: if (on_down)  on_down();  break;
        default: break;
    }
}

// ---- Screen base style ---------------------------------------------

void ui_screen_base_style(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- Home button helper ---------------------------------------------

static void home_btn_cb(lv_event_t *e) { ui_navigate_to(SCREEN_MENU); }

lv_obj_t *ui_add_home_btn(lv_obj_t *scr)
{
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 60, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -185);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, home_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);

    return btn;
}
