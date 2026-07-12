#pragma once

#include "lvgl.h"

lv_obj_t *ui_bbq_config_create(void);

// Call AFTER ui_navigate_to(SCREEN_BBQ_CONFIG) (screen creation is lazy —
// the label this writes into doesn't exist until the first navigation).
void ui_bbq_config_set_target(int grill_idx);
