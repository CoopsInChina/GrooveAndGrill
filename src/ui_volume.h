#pragma once

#include "lvgl.h"

lv_obj_t *ui_volume_create(void);

// Sync display to current Sonos volume
void ui_volume_update(void);
