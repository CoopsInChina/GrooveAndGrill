#include "ui_widgets.h"
#include "ui_common.h"
#include "globals.h"
#include "weather.h"
#include "lvgl.h"
#include "esp_sntp.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_weathericons_64);
LV_FONT_DECLARE(lv_font_weathericons_32);

// ---- Weather Icons glyph strings (Wi-* font, U+F000–U+F0FF) -----------
#define WI_DAY_SUNNY         "\xEF\x80\x8D"   // U+F00D
#define WI_DAY_CLOUDY_HIGH   "\xEF\x81\xBD"   // U+F07D
#define WI_DAY_CLOUDY        "\xEF\x80\x82"   // U+F002
#define WI_CLOUDY            "\xEF\x80\x93"   // U+F013
#define WI_FOG               "\xEF\x80\x94"   // U+F014
#define WI_SPRINKLE          "\xEF\x80\x9C"   // U+F01C
#define WI_RAIN              "\xEF\x80\x99"   // U+F019
#define WI_SHOWERS           "\xEF\x80\x9A"   // U+F01A
#define WI_SLEET             "\xEF\x82\xB5"   // U+F0B5
#define WI_SNOW              "\xEF\x80\x9B"   // U+F01B
#define WI_SNOW_WIND         "\xEF\x81\xA4"   // U+F064
#define WI_THUNDERSTORM      "\xEF\x80\x9E"   // U+F01E

// wttr.in weather codes (113–395) → WI glyph
static const char *wmo_glyph(int code)
{
    if (code == 113)                             return WI_DAY_SUNNY;
    if (code == 116)                             return WI_DAY_CLOUDY_HIGH;
    if (code == 119 || code == 122)              return WI_CLOUDY;
    if (code >= 143 && code <= 260)              return WI_FOG;
    if (code >= 263 && code <= 284)              return WI_SPRINKLE;
    if (code == 311 || code == 314 ||
        code == 317 || code == 320 ||
        code == 362 || code == 365)              return WI_SLEET;
    if (code >= 293 && code <= 308)              return WI_RAIN;
    if (code >= 353 && code <= 359)              return WI_SHOWERS;
    if (code == 368)                             return WI_SNOW;
    if (code == 338 || code == 371)              return WI_SNOW_WIND;
    if (code >= 323 && code <= 335)              return WI_SNOW;
    if (code >= 386)                             return WI_THUNDERSTORM;
    return WI_CLOUDY;
}

// ---- State -------------------------------------------------------------

static lv_obj_t   *s_scr        = NULL;
static lv_obj_t   *s_time_lbl   = NULL;
static lv_obj_t   *s_date_lbl   = NULL;
static lv_obj_t   *s_wx_icon    = NULL;   // lv_font_weathericons_64
static lv_obj_t   *s_city_lbl   = NULL;
static lv_obj_t   *s_temp_lbl   = NULL;
static lv_obj_t   *s_cond_lbl   = NULL;
static lv_obj_t   *s_detail_lbl = NULL;
static lv_obj_t   *s_fc_hour[3] = {NULL};
static lv_obj_t   *s_fc_icon[3] = {NULL};
static lv_obj_t   *s_fc_temp[3] = {NULL};
static lv_timer_t *s_tick_timer = NULL;

// ---- Clock -------------------------------------------------------------

static void update_clock(void)
{
    if (!s_time_lbl) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return;
    if (!g_ntp_synced && esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
        g_ntp_synced = true;
    if (g_ntp_synced) {
        char tbuf[8], dbuf[20];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t->tm_hour, t->tm_min);
        strftime(dbuf, sizeof(dbuf), "%a %d %b", t);
        lv_label_set_text(s_time_lbl, tbuf);
        if (s_date_lbl) lv_label_set_text(s_date_lbl, dbuf);
    } else {
        lv_label_set_text(s_time_lbl, "--:--");
        if (s_date_lbl) lv_label_set_text(s_date_lbl, "Syncing...");
    }
}

// ---- Weather -----------------------------------------------------------

static void update_weather(void)
{
    weather_data_t w;
    weather_get(&w);

    if (w.valid) {
        if (s_wx_icon)    lv_label_set_text(s_wx_icon,    wmo_glyph(w.wmo));
        if (s_city_lbl)   lv_label_set_text(s_city_lbl,   w.location[0] ? w.location : "---");
        if (s_temp_lbl) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%d\xC2\xB0""C", w.temp_c);
            lv_label_set_text(s_temp_lbl, buf);
        }
        if (s_cond_lbl)   lv_label_set_text(s_cond_lbl,   w.condition[0] ? w.condition : "---");
        if (s_detail_lbl) {
            char buf[48];
            snprintf(buf, sizeof(buf), "H: %d%%   W: %d km/h", w.humidity, w.wind_kmh);
            lv_label_set_text(s_detail_lbl, buf);
        }
    } else {
        if (s_wx_icon)    lv_label_set_text(s_wx_icon,    WI_DAY_SUNNY);
        if (s_city_lbl)   lv_label_set_text(s_city_lbl,   "---");
        if (s_temp_lbl)   lv_label_set_text(s_temp_lbl,   "--\xC2\xB0""C");
        if (s_cond_lbl)   lv_label_set_text(s_cond_lbl,   "Loading...");
        if (s_detail_lbl) lv_label_set_text(s_detail_lbl, "");
    }

    if (w.hourly_valid) {
        for (int i = 0; i < 3; i++) {
            if (s_fc_hour[i]) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%dh", w.hourly[i].hour);
                lv_label_set_text(s_fc_hour[i], buf);
            }
            if (s_fc_icon[i]) lv_label_set_text(s_fc_icon[i], wmo_glyph(w.hourly[i].wmo));
            if (s_fc_temp[i]) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d\xC2\xB0", w.hourly[i].temp);
                lv_label_set_text(s_fc_temp[i], buf);
            }
        }
    }
}

// ---- Tick --------------------------------------------------------------

static void tick_cb(lv_timer_t *tmr)
{
    (void)tmr;
    update_clock();
    static int cnt = 0;
    if (++cnt >= 60) { cnt = 0; update_weather(); }
}

// ---- Events ------------------------------------------------------------

static void scr_tapped_cb(lv_event_t *e)
{
    (void)e;
    globals_touch_activity();
    ui_navigate_to(SCREEN_SONOS);
}

static void scr_del_cb(lv_event_t *e)
{
    (void)e;
    if (s_tick_timer) { lv_timer_del(s_tick_timer); s_tick_timer = NULL; }
    s_scr = s_time_lbl = s_date_lbl = NULL;
    s_wx_icon = s_city_lbl = s_temp_lbl = s_cond_lbl = s_detail_lbl = NULL;
    for (int i = 0; i < 3; i++) s_fc_hour[i] = s_fc_icon[i] = s_fc_temp[i] = NULL;
}

// ---- Helper ------------------------------------------------------------

static lv_obj_t *mk_lbl(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, lv_color_t col,
                          int x_ofs, int y_ofs)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l,  font, 0);
    lv_obj_set_style_text_color(l, col,  0);
    lv_obj_align(l, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// ---- Layout (480×480 circular display) --------------------------------
//
//   y_ofs  screen_y   element
//   -148     92       Clock HH:MM          montserrat_48  white
//   -106    134       Date Sat 05 Jul      montserrat_16  0xAAAAAA
//    -44    196       Condition icon       weathericons_64 0xAAAAAA
//     +8    248       City name            montserrat_20  0xAAAAAA
//    +36    276       Temperature  29°C    montserrat_36  0xAAAAAA
//    +76    316       Condition text       montserrat_16  0xAAAAAA
//    +92    332       H: 72%   W: 12 km/h  montserrat_12  0x888888
//
//   Hourly columns at x = -88, 0, +88:
//   +112    352       Hour label  15h      montserrat_14  0x999999
//   +130    370       Hour icon            weathericons_32 0xAAAAAA
//   +162    402       Hour temp  22°       montserrat_16  0xAAAAAA

lv_obj_t *ui_widgets_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, scr_tapped_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,  NULL);

    s_time_lbl = mk_lbl(s_scr, "--:--",
                          &lv_font_montserrat_48, lv_color_hex(0xFFFFFF), 0, -148);
    s_date_lbl = mk_lbl(s_scr, "",
                          &lv_font_montserrat_16, lv_color_hex(0xAAAAAA), 0, -106);
    s_wx_icon  = mk_lbl(s_scr, WI_DAY_SUNNY,
                          &lv_font_weathericons_64, lv_color_hex(0xAAAAAA), 0, -44);
    s_city_lbl = mk_lbl(s_scr, "---",
                          &lv_font_montserrat_20, lv_color_hex(0xAAAAAA), 0, +8);
    s_temp_lbl = mk_lbl(s_scr, "--\xC2\xB0""C",
                          &lv_font_montserrat_36, lv_color_hex(0xAAAAAA), 0, +36);
    s_cond_lbl = mk_lbl(s_scr, "",
                          &lv_font_montserrat_16, lv_color_hex(0xAAAAAA), 0, +76);
    s_detail_lbl = mk_lbl(s_scr, "",
                            &lv_font_montserrat_12, lv_color_hex(0x888888), 0, +92);

    static const int kX[3] = {-88, 0, 88};
    for (int i = 0; i < 3; i++) {
        s_fc_hour[i] = mk_lbl(s_scr, "---",
                                &lv_font_montserrat_14, lv_color_hex(0x999999), kX[i], +112);
        s_fc_icon[i] = mk_lbl(s_scr, WI_DAY_SUNNY,
                                &lv_font_weathericons_32, lv_color_hex(0xAAAAAA), kX[i], +130);
        s_fc_temp[i] = mk_lbl(s_scr, "--\xC2\xB0",
                                &lv_font_montserrat_16, lv_color_hex(0xAAAAAA), kX[i], +162);
    }

    update_clock();
    update_weather();
    s_tick_timer = lv_timer_create(tick_cb, 1000, NULL);
    return s_scr;
}
