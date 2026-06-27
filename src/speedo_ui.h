#pragma once

#include <stdint.h>
#include <stdbool.h>

// Build the LVGL speedometer screen.  Must be called once while holding
// the LVGL lock (display_lock / display_unlock).
void speedo_ui_create(void);

// Update the displayed speed value (mph).
// Thread-safe: acquires the LVGL lock internally.
void speedo_ui_set_speed(float mph);

// Show or hide the speedometer widgets.
// Must be called while holding the LVGL lock.
void speedo_ui_set_visible(bool visible);
