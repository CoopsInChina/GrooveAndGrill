#include "speedo_ui.h"
#include "display.h"
#include "app_config.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "speedo_ui";

#define CANVAS_W    150
#define CANVAS_H    56
#define CANVAS_ZOOM 768   // 3x (256 = 1x in LVGL)
#define CELL_W      32    // fixed pixel slot per digit — centres each glyph independently
#define CELL_W_ONE  20    // narrower slot for "1" (significantly slimmer glyph)

static lv_obj_t   *s_canvas     = NULL;
static lv_obj_t   *s_unit_label = NULL;
static lv_color_t *s_canvas_buf = NULL;

// Written by any task, read by the LVGL timer inside the LVGL task.
// On Xtensa LX7, aligned 4-byte writes are single-instruction atomic.
static volatile float s_target_mph   = 0.0f;
static uint32_t       s_displayed_val = UINT32_MAX; // force first draw

static void canvas_draw_number(const char *txt)
{
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = &lv_font_montserrat_48;
    dsc.color = lv_color_white();
    dsc.align = LV_TEXT_ALIGN_CENTER;

    int len = (int)strlen(txt);
    int total_w = 0;
    for (int i = 0; i < len; i++) {
        total_w += (len == 3 && i == 0) ? CELL_W_ONE : CELL_W;
    }
    int x = (CANVAS_W - total_w) / 2;
    for (int i = 0; i < len; i++) {
        int cw = (len == 3 && i == 0) ? CELL_W_ONE : CELL_W;
        char ch[2] = { txt[i], '\0' };
        lv_canvas_draw_text(s_canvas, x, 2, cw, &dsc, ch);
        x += cw;
    }
}

// Runs inside lv_timer_handler (LVGL mutex already held).
// Reads the latest speed atomically and redraws only when the displayed
// integer value changes — keeps canvas writes at exactly the needed rate.
static void speed_timer_cb(lv_timer_t *timer)
{
    float mph = s_target_mph;
    if (mph < 0.0f)              mph = 0.0f;
    if (mph > SPEED_MAX_DISPLAY) mph = SPEED_MAX_DISPLAY;

    uint32_t val = (uint32_t)(mph + 0.5f);
    if (val == s_displayed_val) return;
    s_displayed_val = val;

    char buf[8];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)val);
    canvas_draw_number(buf);
    lv_obj_invalidate(s_canvas);
}

void speedo_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_canvas_buf = heap_caps_malloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    LV_ASSERT(s_canvas_buf != NULL);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);

    lv_img_set_zoom(s_canvas, CANVAS_ZOOM);
    lv_img_set_pivot(s_canvas, CANVAS_W / 2, CANVAS_H / 2);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, -20);

    canvas_draw_number("0");

    s_unit_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_unit_label, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_unit_label, lv_color_make(150, 150, 150), LV_PART_MAIN);
    lv_label_set_text(s_unit_label, "mp/h");
    lv_obj_align(s_unit_label, LV_ALIGN_CENTER, 0, 90);

    // 30 ms period: fast enough to catch every new value from the 20 Hz sim
    // without redundant canvas redraws between vsync events.
    lv_timer_create(speed_timer_cb, 30, NULL);

    ESP_LOGI(TAG, "Speedometer UI ready");
}

void speedo_ui_set_visible(bool visible)
{
    if (visible) {
        lv_obj_clear_flag(s_canvas,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_unit_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_canvas,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_unit_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// Non-blocking write — the sim task never waits for the LVGL mutex.
void speedo_ui_set_speed(float mph)
{
    if (mph < 0.0f)              mph = 0.0f;
    if (mph > SPEED_MAX_DISPLAY) mph = SPEED_MAX_DISPLAY;
    s_target_mph = mph;
}
