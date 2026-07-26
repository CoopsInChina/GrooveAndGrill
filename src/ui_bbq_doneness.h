#pragma once

#include "lvgl.h"
#include "bbq_controller.h"

lv_obj_t *ui_bbq_doneness_create(void);

// Call AFTER ui_navigate_to(SCREEN_BBQ_DONENESS) (screen creation is lazy).
// meat_type_idx indexes into MEAT_TYPES (see meat_temps.h) for the doneness
// levels; kind identifies the meat. The sensor to assign comes from the
// pending bbq_setup.
void ui_bbq_doneness_begin(int meat_type_idx, meat_kind_t kind);
