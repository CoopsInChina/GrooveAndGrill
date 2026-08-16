#pragma once

#include "lvgl.h"

lv_obj_t *ui_reboot_create(void);

// Sets the headline (e.g. "Sensor Mode Changed") and starts a 5s countdown
// to esp_restart(). Call this, then ui_navigate_to(SCREEN_REBOOT).
void ui_reboot_begin(const char *title);
