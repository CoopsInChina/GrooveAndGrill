#pragma once
#include "lvgl.h"

// Initialise the CST820 touch controller and register it as an LVGL
// pointer input device. Call after display_init() and inside display_lock().
void cst820_init(void);
