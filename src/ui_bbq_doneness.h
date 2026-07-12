#pragma once

#include "lvgl.h"
#include "bbq_controller.h"

lv_obj_t *ui_bbq_doneness_create(void);

// Call AFTER ui_navigate_to(SCREEN_BBQ_DONENESS) (screen creation is lazy).
// meat_type_idx indexes into MEAT_TYPES (see meat_temps.h) for the doneness
// levels; kind identifies the icon to carry over to the grill screen.
// grill_target_c carries over the value chosen on the Grill Config screen.
void ui_bbq_doneness_set_target(int grill_idx, int meat_type_idx, meat_kind_t kind,
                                 int grill_target_c);
