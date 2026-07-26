#pragma once

#include "lvgl.h"

lv_obj_t *ui_bbq_config_create(void);

// Configure the screen from the pending bbq_setup (set by the Add Meat wizard
// or the ⚙ button on a cook view). Call AFTER ui_navigate_to(SCREEN_BBQ_CONFIG)
// — the widgets are created lazily on first navigation.
void ui_bbq_config_begin(void);
