#pragma once

#include "lvgl.h"
#include <stdbool.h>

#define BOOT_ICON_WIFI   0
#define BOOT_ICON_SONOS  1
#define BOOT_ICON_SERVER 2

// icon state values for ui_boot_set_icon
#define BOOT_STATE_GREY  0   // not yet checked
#define BOOT_STATE_OK    1   // connected / found
#define BOOT_STATE_FAIL  2   // failed

lv_obj_t *ui_boot_create(void);
void      ui_boot_show(void);
void      ui_boot_set_status(const char *msg, int progress_pct);
void      ui_boot_set_icon(int idx, int state);
