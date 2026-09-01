#include "ui_settings.h"
#include "ui_common.h"
#include "app_config.h"
#include "board_config.h"
#include "globals.h"
#include "wifi_manager.h"
#include "sonos_controller.h"
#include "bbq_controller.h"
#include "ui_reboot.h"
#include "ota_update.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

// Settings carousel: WiFi | Speaker | BBQ Source | OTA | Screensaver | About
// Real horizontal scroll-snap paging (ported from the Favourites carousel —
// see ui_favourites.c for the underlying LVGL touch-dispatch lessons this
// reuses: absolute page positioning rather than flex, page containers
// non-clickable/non-scrollable so the container itself resolves as the
// touch target, SCROLL_ELASTIC off + press/release edge detection rather
// than relying on LVGL's own GESTURE recognition, and a scroll_end_cb
// guard against re-entrant lv_obj_scroll_to_view calls). Wraps at the
// edges (About -> WiFi and back) rather than exiting, matching the
// screen's previous hide/show-based behaviour.
#define PAGE_COUNT  6
#define PAGE_WIFI           0
#define PAGE_SPEAKER_SETUP  1
#define PAGE_BBQ_SOURCE     2
#define PAGE_OTA            3
#define PAGE_SCREENSAVER    4
#define PAGE_ABOUT          5

static lv_obj_t *s_scr       = NULL;
static lv_obj_t *s_container = NULL;
static int        s_page  = 0;
static bool       s_gesture_fired = false;

// Screensaver page — updated by slider callbacks
static lv_obj_t *s_dim_val_lbl = NULL;
static lv_obj_t *s_ss_val_lbl  = NULL;

static lv_obj_t *s_pages[PAGE_COUNT] = {0};
static lv_obj_t *s_dots[PAGE_COUNT]  = {0};

static void update_dots(int active)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_dots[i])
            lv_obj_set_style_bg_color(s_dots[i], i == active ? COL_ACCENT : COL_BUTTON, 0);
    }
}

// scroll_to: false when called from scroll_end_cb — the container is
// already physically there (see ui_favourites.c's show_index_impl for why
// re-scrolling in that case is actively harmful, not just redundant: it
// re-triggers scroll_begin/end, cascading into a feedback loop).
static void show_page_impl(int p, bool scroll_to)
{
    if (p < 0 || p >= PAGE_COUNT) return;
    s_page = p;
    update_dots(p);
    if (scroll_to && s_pages[p]) {
        lv_obj_scroll_to_view(s_pages[p], LV_ANIM_OFF);
    }
}

static void show_page(int p) { show_page_impl(p, true); }

// A horizontal swipe over a button fires CLICKED too (same touch); the flag
// suppresses that stray click. But swiping is how you move BETWEEN pages on
// this screen (it doesn't navigate away), so without an expiry the flag sat
// there until whatever click happened next — often a later, genuinely
// separate tap on the page you just swiped to, which then silently ate the
// user's first real press (reported as "needs pressing twice"). Auto-clear
// shortly after: long enough to catch the same-touch phantom click, short
// enough that a deliberate tap afterwards always goes through.
static void gesture_clear_cb(lv_timer_t *t) { (void)t; s_gesture_fired = false; }

static void mark_gesture_fired(void)
{
    s_gesture_fired = true;
    lv_timer_t *t = lv_timer_create(gesture_clear_cb, 400, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// ---- Scroll settle / edge-wrap -------------------------------------

// Set by wrap_to() while its animation is in flight — see that function's
// comment for the full trick this implements (temporarily relocate the
// target page next to the current one, animate the short one-page hop,
// then snap positions back to normal once it visually lands).
static bool s_wrapping    = false;
static int  s_wrap_target = -1;
static void wrap_to(int target);

// container_released_cb records an edge-swipe as PENDING rather than
// calling wrap_to() directly. Reason: LVGL's own natural throw/settle
// animation (bringing the container cleanly to rest on the boundary
// page) isn't created until slightly AFTER the RELEASED event is
// dispatched — calling wrap_to() synchronously from that handler starts
// our animation before LVGL's own settle animation even exists on the
// container, so when that settle animation gets created moments later it
// can steal/overwrite the same slot, and what actually plays is LVGL's
// unrelated settle animation instead of ours — by the time it fires
// scroll_end, wrap_to's cleanup forces everything into place instantly,
// which read as "wraps correctly but with no animation". Deferring to
// here, inside the NORMAL settle's own scroll_end, guarantees that
// settle has fully finished before wrap_to ever touches the container.
static bool s_pending_wrap        = false;
static int  s_pending_wrap_target = -1;

static void scroll_end_cb(lv_event_t *e)
{
    if (lv_scr_act() != s_scr) return;   // stale/late event after navigating away

    if (s_wrapping) {
        s_wrapping = false;
        // The animation just landed on s_pages[s_wrap_target] sitting at
        // its TEMPORARY slot (immediately adjacent to where we started) —
        // visually indistinguishable from its real slot from here, since
        // both the page's own position and the scroll offset move
        // together in this same tick, before LVGL's next render pass.
        lv_obj_set_pos(s_pages[s_wrap_target], s_wrap_target * LCD_H_RES, 0);
        lv_obj_scroll_to_view(s_pages[s_wrap_target], LV_ANIM_OFF);
        s_page = s_wrap_target;
        update_dots(s_page);
        return;
    }

    lv_coord_t w = lv_obj_get_width(s_container);
    if (w <= 0) return;

    // Same empirically-derived sign convention as ui_favourites.c's
    // scroll_end_cb (identical container/hardware setup) — x grows
    // positive scrolling forward here, not negative as
    // lv_obj_get_scroll_x's usual convention would suggest.
    lv_coord_t x = lv_obj_get_scroll_x(s_container);
    int p = (int)((x + w / 2) / w);
    if (p < 0) p = 0;
    if (p >= PAGE_COUNT) p = PAGE_COUNT - 1;

    show_page_impl(p, false);

    if (s_pending_wrap) {
        s_pending_wrap = false;
        wrap_to(s_pending_wrap_target);
    }
}

static void scroll_begin_cb(lv_event_t *e)
{
    mark_gesture_fired();
}

// Wrap-around edge detection, independent of LVGL's own GESTURE system —
// see ui_favourites.c's gesture_cb comment for why that doesn't work here:
// SCROLL_ELASTIC (or even just "has room in the other direction") claims
// the touch as a scroll before a GESTURE event can ever fire at a
// boundary. Track raw press/release x instead.
#define EDGE_WRAP_SWIPE_PX 80
static lv_coord_t s_press_x = 0;

static void container_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    s_press_x = pt.x;
}

// A literal scroll_to_view(ANIM_ON) here would animate straight across
// every page in between (About -> WiFi visibly sweeps past Screensaver,
// OTA, BBQ, Speaker) — a fast, disorienting blur, not a wrap. Real
// circular scrolling isn't something LVGL's linear container does
// natively, so this fakes it: relocate the target page to sit ONE page-
// width beyond whichever edge we're leaving from (immediately adjacent
// to the current page, not its usual absolute slot), animate that short
// hop like a completely normal swipe, then let scroll_end_cb's
// s_wrapping branch snap it back to its real slot the instant the
// animation lands — imperceptible, since the page is already sitting in
// the same screen position at that moment.
static void wrap_to(int target)
{
    if (target < 0 || target >= PAGE_COUNT) return;
    if (!s_pages[target]) return;
    mark_gesture_fired();

    // scroll_to_view internally deletes any pending scroll animation on
    // the container BEFORE starting its own — and if one still existed
    // (very plausible: releasing a swipe that both lands on the boundary
    // page and immediately registers as a wrap can catch that page's own
    // settle-snap animation still finishing), deleting it fires a
    // synchronous SCROLL_END right there, before the real animated
    // scroll below even starts. s_wrapping was already true by then, so
    // scroll_end_cb treated that premature event as "landed" and did the
    // whole snap-back instantly — the wrap jumped correctly but with no
    // visible animation, since it was actually done before this
    // function even returned. Clearing it here, before arming
    // s_wrapping, means that internal delete finds nothing to fire on.
    lv_anim_del(s_container, NULL);

    int temp_index = (target == 0) ? PAGE_COUNT : -1;
    lv_obj_set_pos(s_pages[target], temp_index * LCD_H_RES, 0);

    s_wrapping    = true;
    s_wrap_target = target;
    lv_obj_scroll_to_view(s_pages[target], LV_ANIM_ON);
}

static void container_released_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_coord_t dx = pt.x - s_press_x;

    // Deferred to scroll_end_cb, not called directly — see s_pending_wrap's
    // comment for why.
    if (s_page == 0 && dx > EDGE_WRAP_SWIPE_PX) {
        s_pending_wrap        = true;
        s_pending_wrap_target = PAGE_COUNT - 1;
    } else if (s_page == PAGE_COUNT - 1 && dx < -EDGE_WRAP_SWIPE_PX) {
        s_pending_wrap        = true;
        s_pending_wrap_target = 0;
    }
}

static lv_obj_t *make_page(lv_obj_t *parent, int index, const char *title)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, index * LCD_H_RES, 0);
    lv_obj_set_size(p, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    // Non-clickable/non-scrollable: touch dispatch should resolve to
    // s_container (the real scroll target), not this intermediate page —
    // see ui_favourites.c's build_pages comment for the full explanation
    // (every LVGL object defaults to CLICKABLE=true, which without this
    // makes the page itself the resolved "pressed object" instead).
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(p);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -100);

    return p;
}

static void build_about_page(lv_obj_t *p)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Groove & Grill\n\nv%s", FIRMWARE_VERSION);
    lv_obj_t *info = lv_label_create(p);
    lv_label_set_text(info, buf);
    lv_obj_set_style_text_color(info, COL_TEXT, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, 280);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *thanks = lv_label_create(p);
    lv_label_set_text(thanks, "Special thanks to AngryAngShanghai for the graphical assets");
    lv_obj_set_style_text_color(thanks, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(thanks, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(thanks, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(thanks, 280);
    lv_obj_align(thanks, LV_ALIGN_CENTER, 0, 65);
}

static void wifi_btn_cb(lv_event_t *e)
{
    ui_navigate_to(SCREEN_WIFI_SETUP);
}

static void build_wifi_page(lv_obj_t *p)
{
    const char *ssid = wifi_manager_ssid();
    char buf[96];
    if (strlen(ssid) > 0)
        snprintf(buf, sizeof(buf), "Connected:\n%s", ssid);
    else
        snprintf(buf, sizeof(buf), "Not connected");

    lv_obj_t *status = lv_label_create(p);
    lv_label_set_text(status, buf);
    lv_obj_set_style_text_color(status, COL_TEXT, 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(status, 260);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *btn = lv_btn_create(p);
    lv_obj_set_size(btn, 180, 48);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Setup WiFi");
    lv_obj_set_style_text_color(lbl, COL_BG, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
}

static void speaker_btn_cb(lv_event_t *e)
{
    ui_navigate_to(SCREEN_SPEAKER_SETUP);
}

static void build_speaker_page(lv_obj_t *p)
{
    const char *spkr = sonos_active_speaker_name();
    char buf[96];
    if (spkr && spkr[0])
        snprintf(buf, sizeof(buf), "Active:\n%s", spkr);
    else
        snprintf(buf, sizeof(buf), "No speaker found");

    lv_obj_t *info = lv_label_create(p);
    lv_label_set_text(info, buf);
    lv_obj_set_style_text_color(info, COL_TEXT, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, 260);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *btn = lv_btn_create(p);
    lv_obj_set_size(btn, 180, 48);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, speaker_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Choose Speaker");
    lv_obj_set_style_text_color(lbl, COL_BG, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
}

// ---- OTA page ------------------------------------------------------
// Manual only: Check for Update -> (if newer) Update Now -> progress ->
// hands off to the shared reboot-countdown screen. Only released builds
// (pushed to the `release` branch) are ever offered — see README.

static lv_obj_t   *s_ota_info_lbl = NULL;
static lv_obj_t   *s_ota_btn      = NULL;
static lv_obj_t   *s_ota_btn_lbl  = NULL;
static lv_timer_t *s_ota_timer    = NULL;
static bool        s_ota_can_update = false;

static void ota_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    if (s_ota_can_update) ota_start_async();
    else                  ota_check_async();
}

static void ota_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_scr || lv_scr_act() != s_scr || s_page != PAGE_OTA) return;

    ota_status_t st;
    ota_get_status(&st);

    if (st.state == OTA_DONE_OK) {
        ui_reboot_begin("Firmware Updated");
        ui_navigate_to(SCREEN_REBOOT);
        return;
    }

    char buf[96];
    const char *btn_text = "CHECK FOR UPDATE";
    bool busy = false;
    s_ota_can_update = false;

    switch (st.state) {
        case OTA_CHECKING:
            snprintf(buf, sizeof(buf), "Current: v%s\n\nChecking...", FIRMWARE_VERSION);
            btn_text = "CHECKING..."; busy = true;
            break;
        case OTA_UP_TO_DATE:
            snprintf(buf, sizeof(buf), "Current: v%s\n\nUp to date", FIRMWARE_VERSION);
            break;
        case OTA_UPDATE_AVAILABLE:
            snprintf(buf, sizeof(buf), "Current: v%s\n\nUpdate available: v%s",
                     FIRMWARE_VERSION, st.latest_version);
            btn_text = "UPDATE NOW"; s_ota_can_update = true;
            break;
        case OTA_CHECK_FAILED:
            snprintf(buf, sizeof(buf), "Current: v%s\n\n%s", FIRMWARE_VERSION, st.error);
            btn_text = "RETRY CHECK";
            break;
        case OTA_UPDATING:
            if (st.image_size > 0)
                snprintf(buf, sizeof(buf), "Updating... %d%%\n\nDo not power off",
                         (int)((int64_t)st.bytes_read * 100 / st.image_size));
            else
                snprintf(buf, sizeof(buf), "Updating... %d KB\n\nDo not power off",
                         st.bytes_read / 1024);
            btn_text = "UPDATING..."; busy = true;
            break;
        case OTA_DONE_FAIL:
            snprintf(buf, sizeof(buf), "Current: v%s\n\nUpdate failed: %s", FIRMWARE_VERSION, st.error);
            btn_text = "RETRY";
            break;
        case OTA_IDLE:
        default:
            snprintf(buf, sizeof(buf), "Current: v%s", FIRMWARE_VERSION);
            break;
    }

    if (s_ota_info_lbl) lv_label_set_text(s_ota_info_lbl, buf);
    if (s_ota_btn_lbl)  lv_label_set_text(s_ota_btn_lbl, btn_text);
    if (s_ota_btn) {
        if (busy) lv_obj_add_state(s_ota_btn, LV_STATE_DISABLED);
        else      lv_obj_clear_state(s_ota_btn, LV_STATE_DISABLED);
    }
}

static void build_ota_page(lv_obj_t *p)
{
    s_ota_info_lbl = lv_label_create(p);
    char buf[32];
    snprintf(buf, sizeof(buf), "Current: v%s", FIRMWARE_VERSION);
    lv_label_set_text(s_ota_info_lbl, buf);
    lv_obj_set_style_text_color(s_ota_info_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_ota_info_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_ota_info_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_ota_info_lbl, 280);
    lv_obj_align(s_ota_info_lbl, LV_ALIGN_CENTER, 0, -20);

    s_ota_btn = lv_btn_create(p);
    lv_obj_set_size(s_ota_btn, 200, 48);
    lv_obj_align(s_ota_btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(s_ota_btn, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(s_ota_btn, COL_BUTTON, LV_STATE_DISABLED);
    lv_obj_set_style_radius(s_ota_btn, 24, 0);
    lv_obj_set_style_shadow_width(s_ota_btn, 0, 0);
    lv_obj_set_style_border_width(s_ota_btn, 0, 0);
    lv_obj_add_event_cb(s_ota_btn, ota_btn_cb, LV_EVENT_CLICKED, NULL);

    s_ota_btn_lbl = lv_label_create(s_ota_btn);
    lv_label_set_text(s_ota_btn_lbl, "CHECK FOR UPDATE");
    lv_obj_set_style_text_color(s_ota_btn_lbl, COL_BG, 0);
    lv_obj_set_style_text_font(s_ota_btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_ota_btn_lbl);

    if (!s_ota_timer) s_ota_timer = lv_timer_create(ota_poll_cb, 500, NULL);
}

// ---- BBQ source page (BBQ Box observer  vs  direct wireless probe) ------
// Changing source brings up a different BLE stack, so we persist + reboot.

static void source_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }  // ignore swipe-clicks
    bbq_source_t chosen = (bbq_source_t)(intptr_t)lv_event_get_user_data(e);
    if (chosen == bbq_source_get()) return;                    // already selected

    bbq_source_set(chosen);   // persisted; picked up on next boot

    ui_reboot_begin("Sensor Mode Changed");
    ui_navigate_to(SCREEN_REBOOT);
}

static void clear_setup_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    bbq_clear_all();
    lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target(e), 0);
    if (lbl) lv_label_set_text(lbl, "Cleared");
}

static void make_source_btn(lv_obj_t *p, const char *text, bool active, int cy,
                            bbq_source_t val)
{
    lv_obj_t *btn = lv_btn_create(p);
    lv_obj_set_size(btn, 230, 52);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, cy);
    lv_obj_set_style_radius(btn, 26, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (active) {
        lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(btn, COL_TEXT_DIM, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
    }
    lv_obj_add_event_cb(btn, source_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)val);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, active ? COL_BG : COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
}

static void build_bbq_source_page(lv_obj_t *p)
{
    bbq_source_t cur = bbq_source_get();

    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint, "Read temperatures from");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, -50);

    make_source_btn(p, "BBQ Box",        cur == BBQ_SRC_BOX,   10, BBQ_SRC_BOX);
    make_source_btn(p, "Wireless Probe", cur == BBQ_SRC_PROBE, 75, BBQ_SRC_PROBE);

    // Clear-all: wipe leftover allocations to get a clean "Add Meat only" state.
    lv_obj_t *clr = lv_btn_create(p);
    lv_obj_set_size(clr, 200, 40);
    lv_obj_align(clr, LV_ALIGN_CENTER, 0, 140);
    lv_obj_set_style_radius(clr, 20, 0);
    lv_obj_set_style_bg_opa(clr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(clr, COL_WARN, 0);
    lv_obj_set_style_border_width(clr, 1, 0);
    lv_obj_set_style_shadow_width(clr, 0, 0);
    lv_obj_add_event_cb(clr, clear_setup_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cl = lv_label_create(clr);
    lv_label_set_text(cl, "Clear cook setup");
    lv_obj_set_style_text_color(cl, COL_WARN, 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);
}

// ---- Screensaver page callbacks ----------------------------------------

static void dim_switch_cb(lv_event_t *e)
{
    g_dim_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    globals_save_display();
}

static void dim_slider_cb(lv_event_t *e)
{
    g_autodim_sec = (int)lv_slider_get_value(lv_event_get_target(e));
    if (s_dim_val_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ds", g_autodim_sec);
        lv_label_set_text(s_dim_val_lbl, buf);
    }
    globals_save_display();
}

static void ss_switch_cb(lv_event_t *e)
{
    g_screensaver_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    globals_save_display();
}

static void ss_slider_cb(lv_event_t *e)
{
    int minutes = (int)lv_slider_get_value(lv_event_get_target(e));
    g_screensaver_sec = minutes * 60;
    if (s_ss_val_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d min", minutes);
        lv_label_set_text(s_ss_val_lbl, buf);
    }
    globals_save_display();
}

// ---- Screensaver page helpers ------------------------------------------

static void ss_make_row(lv_obj_t *parent, const char *text, int cy,
                         bool checked, lv_event_cb_t sw_cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 280, 38);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, cy);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    // Same reasoning as ss_make_slider's SCROLL_CHAIN clear — a switch
    // supports a small drag-to-toggle too, which without this could get
    // claimed as a page swipe instead.
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLL_CHAIN);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_BUTTON, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_TEXT,   LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t *ss_make_slider(lv_obj_t *parent, int min, int max, int val,
                                  int cy, lv_event_cb_t sl_cb)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, 260, 12);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, cy);
    // Paging is real LVGL scrolling now, not GESTURE events (see
    // ui_favourites.c) — a slider isn't itself scrollable, so without this
    // its own drag search would walk straight up to s_container and get
    // claimed as a page swipe instead of moving the slider. SCROLL_CHAIN
    // is what stops that upward search (the GESTURE_BUBBLE clear this
    // used to be doesn't apply to real scroll dispatch at all).
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, COL_PANEL,  LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, sl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sl;
}

static lv_obj_t *ss_make_val_label(lv_obj_t *parent, const char *text, int cy)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, cy);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// ---- Screensaver page build --------------------------------------------

static void build_screensaver_page(lv_obj_t *p)
{
    // Dim on/off + timer (10 – 120 s)
    ss_make_row(p, "Dim", -55, g_dim_enabled, dim_switch_cb);
    ss_make_slider(p, 10, 120, g_autodim_sec, -15, dim_slider_cb);
    char dim_buf[16];
    snprintf(dim_buf, sizeof(dim_buf), "%ds", g_autodim_sec);
    s_dim_val_lbl = ss_make_val_label(p, dim_buf, +8);

    // Screensaver on/off + timer (1 – 30 min)
    ss_make_row(p, "Screensaver", +45, g_screensaver_enabled, ss_switch_cb);
    ss_make_slider(p, 1, 30, g_screensaver_sec / 60, +85, ss_slider_cb);
    char ss_buf[16];
    snprintf(ss_buf, sizeof(ss_buf), "%d min", g_screensaver_sec / 60);
    s_ss_val_lbl = ss_make_val_label(p, ss_buf, +108);
}

// ---- Create --------------------------------------------------------

lv_obj_t *ui_settings_create(void)
{
    s_page = 0;
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);

    // Horizontal scroll-snap row — see ui_favourites.c's ui_favourites_create
    // for the full reasoning behind each of these flags/handlers.
    s_container = lv_obj_create(s_scr);
    lv_obj_set_size(s_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_set_scrollbar_mode(s_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_container, LV_SCROLL_SNAP_CENTER);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_add_event_cb(s_container, scroll_end_cb,   LV_EVENT_SCROLL_END,   NULL);
    lv_obj_add_event_cb(s_container, scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_container, container_pressed_cb,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_container, container_released_cb, LV_EVENT_RELEASED, NULL);

    static const char *titles[PAGE_COUNT] = {
        "WiFi", "Speaker", "BBQ Source", "OTA Update", "Screensaver", "About"
    };

    for (int i = 0; i < PAGE_COUNT; i++) {
        s_pages[i] = make_page(s_container, i, titles[i]);
    }

    build_wifi_page(s_pages[PAGE_WIFI]);
    build_speaker_page(s_pages[PAGE_SPEAKER_SETUP]);
    build_bbq_source_page(s_pages[PAGE_BBQ_SOURCE]);
    build_ota_page(s_pages[PAGE_OTA]);
    build_screensaver_page(s_pages[PAGE_SCREENSAVER]);
    build_about_page(s_pages[PAGE_ABOUT]);

    // Page dots indicator — saved so show_page() can recolour them
    lv_obj_t *dots_cont = lv_obj_create(s_scr);
    lv_obj_set_size(dots_cont, 140, 16);
    lv_obj_align(dots_cont, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(dots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots_cont, 0, 0);
    lv_obj_set_style_pad_all(dots_cont, 0, 0);
    lv_obj_set_flex_flow(dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(dots_cont);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, i == 0 ? COL_ACCENT : COL_BUTTON, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_hor(dot, 4, 0);
        s_dots[i] = dot;
    }

    // Home button LAST → highest z-order, sits above full-screen page containers.
    ui_add_home_btn(s_scr);

    show_page(PAGE_WIFI);   // land on the first page (About is now last)
    return s_scr;
}
