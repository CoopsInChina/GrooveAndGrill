#pragma once

#include "lvgl.h"
#include <stdbool.h>

// ============================================================
// Colour palette — dark theme optimised for circular screen
// ============================================================
#define COL_BG          lv_color_hex(0x000000)
#define COL_ACCENT      lv_color_hex(0x1ED760)   // Sonos green
#define COL_ACCENT2     lv_color_hex(0xE87722)   // BBQ / warm orange
#define COL_TEXT        lv_color_hex(0xFFFFFF)
#define COL_TEXT_DIM    lv_color_hex(0x888888)
#define COL_PANEL       lv_color_hex(0x181818)
#define COL_BUTTON      lv_color_hex(0x2A2A2A)
#define COL_WARN        lv_color_hex(0xCC4444)

// Screen geometry
#define CX          240     // circle centre X
#define CY          240     // circle centre Y
#define CR          240     // visible radius
#define SAFE_R      210     // content safe zone radius

// ============================================================
// Screen IDs
// ============================================================
typedef enum {
    SCREEN_BOOT = 0,
    SCREEN_MENU,
    SCREEN_SONOS,
    SCREEN_FAVOURITES,
    SCREEN_VOLUME,
    SCREEN_SETTINGS,
    SCREEN_WIFI_SETUP,
    SCREEN_SPEAKER_SETUP,
    SCREEN_BBQ,
    SCREEN_BBQ_CONFIG,
    SCREEN_BBQ_DONENESS,
    SCREEN_WIDGETS,
    SCREEN_COUNT
} screen_id_t;

// Navigate to a screen (must hold display lock)
void ui_navigate_to(screen_id_t id);

// Invalidate a screen so it is recreated next visit
void ui_screen_invalidate(screen_id_t id);

// Non-blocking reconnecting banner (floats above all screens)
void ui_show_reconnecting_banner(void);
void ui_hide_reconnecting_banner(void);

// Call on any user touch to feed the screensaver watchdog
void ui_touch_activity(void);

// Gesture helper — pass NULL for directions you don't handle
typedef void (*gesture_cb_t)(void);
void ui_handle_gesture(gesture_cb_t on_left, gesture_cb_t on_right,
                        gesture_cb_t on_up,   gesture_cb_t on_down);

// Standard screen background setup
void ui_screen_base_style(lv_obj_t *scr);

// Add a standardised 60×60 home button at the top of any screen. Returns the
// button so callers can nudge its position (default align is CENTER, 0, -185).
lv_obj_t *ui_add_home_btn(lv_obj_t *scr);
