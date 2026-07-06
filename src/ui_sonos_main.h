#pragma once

#include "lvgl.h"

lv_obj_t *ui_sonos_main_create(void);

// Refresh track info from sonos_controller
void ui_sonos_main_update(void);
