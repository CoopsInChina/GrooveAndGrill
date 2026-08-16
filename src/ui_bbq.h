#pragma once

#include "lvgl.h"

lv_obj_t *ui_bbq_create(void);

// Selects which carousel slot the BBQ screen shows next time it's entered.
// Pass a negative value to leave the current slot unchanged (a no-op) — used
// after the wizard/config confirms so the display lands on the meat that was
// just added/edited instead of wherever the carousel last happened to be.
void ui_bbq_set_index(int idx);
