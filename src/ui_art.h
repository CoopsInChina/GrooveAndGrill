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

// Request art from a pre-downloaded JPEG blob (e.g. a favourite's art cached in PSRAM
// from SPIFFS), tagged with a caller-defined id in [0, MAX_FAVOURITES) — favourites
// uses its page index. Each tag gets its own PERMANENT decode slot (see ART_BLOB_SLOTS
// in ui_art.c): once decoded, it stays valid and instantly re-appliable indefinitely,
// with no re-decode, until ui_art_blob_invalidate(_all)() is called for it. Skips the
// network download; goes straight to decode. No-ops if this tag is already cached or
// already queued. jpeg must remain valid until the art task finishes decoding (shortly
// after this call; it's not queued for long).
void     ui_art_request_blob(int tag, const uint8_t *jpeg, size_t sz);

// Clears cached content for one tag (or all tags) — call when a favourite's art has
// actually changed (or the favourites list was restructured) so the next
// ui_art_request_blob for it triggers a genuine re-decode instead of reusing stale
// content.
void     ui_art_blob_invalidate(int tag);
void     ui_art_blob_invalidate_all(void);

// Call from LVGL task (e.g. poll timer).  Updates img_obj if new URL art is ready.
// Returns true when art was applied this call.
bool     ui_art_update(lv_obj_t *img_obj);

// Same as ui_art_update but for blob art tagged `tag` (see ui_art_request_blob).
// Idempotent and safe to call repeatedly — returns true and (re-)applies it to
// img_obj whenever that tag has cached content, false if it's still decoding/queued
// or was never requested.
bool     ui_art_update_blob(lv_obj_t *img_obj, int tag);

// Dominant colour sampled from the most recently decoded art (darkened).
// Returns 0x1a1a1a when no art has been decoded yet.
uint32_t ui_art_dominant_color(void);
