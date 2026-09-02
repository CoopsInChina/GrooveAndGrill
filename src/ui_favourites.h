#pragma once

#include "lvgl.h"

lv_obj_t *ui_favourites_create(void);

// Show a specific favourite index (0-based)
void ui_favourites_show_index(int index);

// Kicks off background decoding of every current favourite's art (each
// gets its own permanent slot in ui_art.c — see ui_art_request_blob).
// Safe to call before the screen itself has ever been created. Call once
// at boot (favourites are already loaded by then) and again whenever the
// favourites list changes, so art is typically already ready by the time
// the user actually opens Favourites or swipes to a given page.
void ui_favourites_prefetch_all(void);
