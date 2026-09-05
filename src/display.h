#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include <stdbool.h>

// Initialise display hardware and LVGL. Must be called once at startup
// after tca9554_init().
esp_err_t display_init(void);

// Turn backlight on (full brightness) or off.
void display_set_backlight(bool on);

// Set backlight to a percentage (0-100). Compatible with lv_anim_exec_xcb_t.
void display_set_brightness(int pct);

// Acquire/release the LVGL mutex before calling any lv_* functions
// from outside the LVGL task.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// The RGB panel handle, for reading a frame buffer directly (see
// web_server.c's /screenshot dev endpoint). NULL before display_init().
esp_lcd_panel_handle_t display_get_panel(void);
