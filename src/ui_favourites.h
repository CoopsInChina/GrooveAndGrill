#pragma once

#include "lvgl.h"

lv_obj_t *ui_favourites_create(void);

// Show a specific favourite index (0-based)
void ui_favourites_show_index(int index);
