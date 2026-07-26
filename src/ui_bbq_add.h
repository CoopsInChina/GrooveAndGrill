#pragma once

#include "lvgl.h"

// The "Add Meat" wizard: pick grill # -> pick sensor -> pick sensor type
// (only when the grill has no ambient sensor yet), then hand off to the
// config/doneness screens which assign the chosen sensor. Resets to step 1
// automatically each time it is navigated to.
lv_obj_t *ui_bbq_add_create(void);
