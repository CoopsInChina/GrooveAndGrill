#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Initialise display hardware and LVGL. Must be called once at startup
// after tca9554_init().
esp_err_t display_init(void);

// Turn backlight on (full brightness) or off.
void display_set_backlight(bool on);

// Acquire/release the LVGL mutex before calling any lv_* functions
// from outside the LVGL task.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);
