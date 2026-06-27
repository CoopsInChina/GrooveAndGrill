#pragma once

#include "esp_err.h"

// Load saved calibration from NVS (call once at startup, after nvs_flash_init).
esp_err_t cal_init(void);

// Get the current offset percentage (e.g. -3.5 means display 3.5% slower).
float cal_get_offset_pct(void);

// Set and immediately persist a new offset percentage.
// Clamped to ±CAL_MAX_OFFSET_PCT defined in app_config.h.
esp_err_t cal_set_offset_pct(float pct);

// Apply calibration to a raw mph value and return the display value.
// display_mph = raw_mph * (1.0 + offset_pct / 100.0)
float cal_apply(float raw_mph);
