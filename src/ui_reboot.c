#include "ui_reboot.h"
#include "ui_common.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>

#define COUNTDOWN_START_S  5

static lv_obj_t   *s_scr       = NULL;
static lv_obj_t   *s_title_lbl = NULL;
static lv_obj_t   *s_count_lbl = NULL;
static lv_timer_t *s_timer     = NULL;
static int         s_remaining = COUNTDOWN_START_S;
static char        s_title_text[48] = "Rebooting";

static void set_count_text(void)
{
    if (!s_count_lbl) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Rebooting in %d...", s_remaining);
    lv_label_set_text(s_count_lbl, buf);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    s_remaining--;
    if (s_remaining <= 0) {
        if (s_count_lbl) lv_label_set_text(s_count_lbl, "Rebooting...");
        esp_restart();
        return;
    }
    set_count_text();
}

void ui_reboot_begin(const char *title)
{
    if (title) strlcpy(s_title_text, title, sizeof(s_title_text));
    s_remaining = COUNTDOWN_START_S;
    if (s_title_lbl) lv_label_set_text(s_title_lbl, s_title_text);
    set_count_text();

    // Guard against a second caller before the first reboot fires.
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_timer = lv_timer_create(tick_cb, 1000, NULL);
}

lv_obj_t *ui_reboot_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    // Deliberately no home button / gesture nav — this is a forced,
    // unavoidable transition, not a screen the user browses away from.

    s_title_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_title_lbl, s_title_text);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title_lbl, COL_ACCENT2, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_title_lbl, 320);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -30);

    s_count_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_count_lbl, "Rebooting in 5...");
    lv_obj_set_style_text_align(s_count_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_count_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_count_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_count_lbl, LV_ALIGN_CENTER, 0, 20);

    return s_scr;
}
