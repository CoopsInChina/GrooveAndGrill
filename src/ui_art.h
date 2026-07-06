#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

// Square pixel dimension of the decoded + scaled art buffer.
// Matches the art placeholder size in ui_sonos_main.c.
#define ART_SIZE  300

// Call once from app_main (after display_init, before navigate_to SCREEN_SONOS).
void     ui_art_init(void);

// Request album art for a raw Sonos URL.  Returns immediately; download is async.
// Pass "" or NULL to cancel any pending request.
void     ui_art_request(const char *raw_url);

// Request art from a pre-downloaded JPEG blob (e.g. art cached in PSRAM from SPIFFS).
// Skips the network download; goes straight to decode.  jpeg must remain valid until
// ui_art_update() returns true (i.e. until the art task finishes decoding).
void     ui_art_request_blob(const uint8_t *jpeg, size_t sz);

// Call from LVGL task (e.g. poll timer).  Updates img_obj if new URL art is ready.
// Returns true when art was applied this call.
bool     ui_art_update(lv_obj_t *img_obj);

// Same as ui_art_update but for blob art (decoded from ui_art_request_blob).
// Uses a separate descriptor — safe to call while the main screen uses ui_art_update.
bool     ui_art_update_blob(lv_obj_t *img_obj);

// Dominant colour sampled from the most recently decoded art (darkened).
// Returns 0x1a1a1a when no art has been decoded yet.
uint32_t ui_art_dominant_color(void);
