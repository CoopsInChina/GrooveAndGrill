#include "ui_favourites.h"
#include "ui_common.h"
#include "ui_art.h"
#include "sonos_controller.h"
#include <stddef.h>
#include "wifi_manager.h"
#include "app_config.h"
#include "board_config.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr          = NULL;
static lv_obj_t   *s_container    = NULL;   // horizontal scroll-snap row of pages
static lv_timer_t *s_refresh_timer = NULL;
static lv_timer_t *s_art_timer     = NULL;
static int         s_index         = 0;
static int         s_last_count    = -1;
// Which way the current page was arrived at (+1 = swiped forward, art
// arrives from the right; -1 = swiped back, from the left) — used as a
// fallback slide-in direction for reveal_art_if_ready, for whichever
// page's decode is still the one most recently landed on when its art
// finally becomes ready (usually already in progress by arrival — see
// request_art_for's proactive neighbour pre-fetch below).
static int         s_enter_dir     = 1;
static bool        s_gesture_fired = false;

// One page per favourite slot, plus one add-slot page. Both immediate
// neighbours of the current page get their art proactively requested as
// soon as you land on a page (see request_art_for) — ui_art.c gives each
// favourite index its own PERMANENT decode slot (favourites are rarely
// added/changed, so there's no eviction to worry about — see ui_art.h),
// so the incoming page's art usually has a real chance of being ready
// before a swipe even settles, and instantly re-appears with no re-decode
// on any later revisit.
#define MAX_PAGES (MAX_FAVOURITES + 1)
static lv_obj_t *s_page[MAX_PAGES]      = {0};
static lv_obj_t *s_page_art[MAX_PAGES]  = {0};   // NULL on the add-slot page
static lv_obj_t *s_add_btn      = NULL;          // lives on the last page
static lv_obj_t *s_qr           = NULL;
static lv_obj_t *s_qr_url       = NULL;
static int       s_page_count   = 0;

// Page-dot indicator — one dot per favourite slot plus the add slot,
// matching the settings screen's carousel style.
static lv_obj_t *s_dots_cont            = NULL;
static lv_obj_t *s_dots[MAX_FAVOURITES + 1] = {0};
static int       s_dot_count            = 0;

static void rebuild_dots(int total)
{
    if (!s_dots_cont || total == s_dot_count) return;
    if (total > MAX_FAVOURITES + 1) total = MAX_FAVOURITES + 1;

    lv_obj_clean(s_dots_cont);
    for (int i = 0; i < total; i++) {
        lv_obj_t *dot = lv_obj_create(s_dots_cont);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, COL_BUTTON, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_hor(dot, 2, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_dots[i] = dot;
    }
    s_dot_count = total;
}

static void update_dots(int active)
{
    for (int i = 0; i < s_dot_count; i++) {
        if (s_dots[i])
            lv_obj_set_style_bg_color(s_dots[i], i == active ? COL_ACCENT : COL_BUTTON, 0);
    }
}

// ---- Page building ---------------------------------------------------
// Rebuilds the scrolling row's pages to match the current favourite count.
// Called whenever the count changes (add/remove) — mirrors rebuild_dots.

static void add_btn_cb(lv_event_t *e);
static void show_index_impl(int index, bool scroll_to);
static void request_art_for(int idx);

static void build_pages(int count)
{
    lv_obj_clean(s_container);
    memset(s_page,     0, sizeof(s_page));
    memset(s_page_art, 0, sizeof(s_page_art));
    s_add_btn = s_qr = s_qr_url = NULL;

    int total = count + 1;   // + the add-slot page
    if (total > MAX_PAGES) total = MAX_PAGES;
    s_page_count = total;

    for (int i = 0; i < total; i++) {
        lv_obj_t *page = lv_obj_create(s_container);
        // Absolute position/size, not flex: LVGL flex-row with LV_PCT(100)
        // children has real edge cases for exactly this "full-width pages
        // side by side" pattern — this matches LVGL's own official
        // horizontal snap-gallery example (lv_example_scroll_3), which
        // positions pages explicitly rather than via flex.
        lv_obj_set_pos(page, i * LCD_H_RES, 0);
        lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_page[i] = page;

        bool is_add = (i == count);
        if (!is_add) {
            lv_obj_t *art = lv_img_create(page);
            lv_obj_center(art);
            lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(art, LV_OBJ_FLAG_HIDDEN);   // shown once its art decodes
            // radius alone doesn't clip an lv_img's pixel content — needs
            // clip_corner too, or the square art just overlaps a rounded
            // (invisible) background rect.
            lv_obj_set_style_radius(art, 24, 0);
            lv_obj_set_style_clip_corner(art, true, 0);
            s_page_art[i] = art;
        } else {
            // + button — fully centered, 120x120
            s_add_btn = lv_btn_create(page);
            lv_obj_set_size(s_add_btn, 120, 120);
            lv_obj_center(s_add_btn);
            lv_obj_set_style_radius(s_add_btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s_add_btn, COL_PANEL, 0);
            lv_obj_set_style_border_color(s_add_btn, COL_ACCENT, 0);
            lv_obj_set_style_border_width(s_add_btn, 2, 0);
            lv_obj_set_style_shadow_width(s_add_btn, 0, 0);
            lv_obj_add_event_cb(s_add_btn, add_btn_cb, LV_EVENT_CLICKED, NULL);

            lv_obj_t *add_icon = lv_label_create(s_add_btn);
            lv_label_set_text(add_icon, LV_SYMBOL_PLUS);
            lv_obj_set_style_text_color(add_icon, COL_ACCENT, 0);
            lv_obj_set_style_text_font(add_icon, &lv_font_montserrat_28, 0);
            lv_obj_center(add_icon);

            // QR code — hidden until + pressed (180x180, centred)
            s_qr = lv_qrcode_create(page, 180,
                                     lv_color_make(0, 0, 0),
                                     lv_color_make(255, 255, 255));
            lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, 10);
            lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

            s_qr_url = lv_label_create(page);
            lv_label_set_text(s_qr_url, "");
            lv_obj_set_style_text_color(s_qr_url, COL_TEXT_DIM, 0);
            lv_obj_set_style_text_font(s_qr_url, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_align(s_qr_url, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(s_qr_url, LV_ALIGN_CENTER, 0, 115);
            lv_obj_add_flag(s_qr_url, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---- Navigation ------------------------------------------------------
// Paging itself is real LVGL scrolling now (see scroll_end_cb) — these
// just handle the "swipe past the first/last page" exit-to-Sonos case.

static void go_exit(void)
{
    s_gesture_fired = true;
    ui_navigate_to_anim(SCREEN_SONOS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    s_index = 0;
    // NOTE: previously also forced s_last_count = -1 here to make
    // scr_loaded_cb "re-render" on return — but that forces build_pages()
    // to run again even though the real favourite count hasn't changed,
    // destroying and recreating every page's art object. The fresh page 0
    // object then never got a new art request, because s_blob_req_index
    // (tracking which index's decode is in flight) wasn't reset by the
    // rebuild and still said "already requested index 0" — so the
    // brand-new, blank object stayed blank. Just don't force it; the real
    // page objects persist correctly on their own.
}

static void gesture_cb(lv_event_t *e)
{
    // Kept as a fallback for the rare case s_container has zero scrollable
    // room in EITHER direction (only when there's just a single page) —
    // GESTURE only fires when nothing claimed the touch as a scroll. See
    // container_released_cb below for the real edge-exit detection: LVGL's
    // scroll engine claims this container for ANY drag as long as it has
    // room in AT LEAST ONE direction (true almost always — only the exact
    // first/last page has zero room on ONE side), which blocks GESTURE
    // from ever firing here even when that specific drag direction can't
    // move at all.
    ui_handle_gesture(go_exit, go_exit, NULL, NULL);
}

// LVGL's scroll/gesture arbitration doesn't work for this carousel shape
// (see gesture_cb's comment) — track raw press/release x manually instead,
// entirely independent of whether LVGL decided to claim the touch as a
// scroll.
#define EDGE_EXIT_SWIPE_PX 80
static lv_coord_t s_press_x = 0;

static void container_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_x = p.x;
}

static void container_released_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_coord_t dx = p.x - s_press_x;

    if (s_index == 0 && dx > EDGE_EXIT_SWIPE_PX) {
        go_exit();
    } else if (s_page_count > 0 && s_index == s_page_count - 1 && dx < -EDGE_EXIT_SWIPE_PX) {
        go_exit();
    }
}

// Tap (not swipe) anywhere on the current page plays that favourite.
static void screen_tap_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    if (s_index < sonos_device_fav_count()) {
        sonos_play_device_favourite(s_index);
        ui_navigate_to_anim(SCREEN_SONOS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }
}

// ---- Add slot — show QR code when + pressed ---------------------------

static void add_btn_cb(lv_event_t *e)
{
    if (s_gesture_fired) { s_gesture_fired = false; return; }
    if (!s_qr || !s_qr_url || !s_add_btn) return;

    const char *ip = wifi_manager_ip();
    if (!ip || !ip[0]) return;

    static char url[64];
    snprintf(url, sizeof(url), "http://%s/setup", ip);

    lv_obj_add_flag(s_add_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qr,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qr_url, LV_OBJ_FLAG_HIDDEN);

    lv_qrcode_update(s_qr, url, strlen(url));
    lv_label_set_text(s_qr_url, url);
}

// ---- Scroll settle: figure out which page we landed on -----------------

static void scroll_end_cb(lv_event_t *e)
{
    // A late/stale scroll_end (elastic bounce-back, leftover momentum)
    // can fire after the screen's already been navigated away from —
    // without this guard it silently overwrites s_index with whatever it
    // computes, so the NEXT time this screen loads it jumps to some
    // unrelated favourite instead of where you actually left off.
    if (lv_scr_act() != s_scr) return;
    if (s_page_count == 0) return;
    lv_coord_t w = lv_obj_get_width(s_container);
    if (w <= 0) return;

    // Empirically (logged) x grows positive as you scroll forward through
    // pages here, not negative as lv_obj_get_scroll_x's usual convention
    // would suggest — the earlier -x version stayed permanently clamped
    // to index 0 for any forward swipe, which was the actual "swipe
    // doesn't work" symptom.
    lv_coord_t x   = lv_obj_get_scroll_x(s_container);
    int index = (int)((x + w / 2) / w);
    if (index < 0) index = 0;
    if (index >= s_page_count) index = s_page_count - 1;

    ESP_LOGI("fav", "scroll_end: x=%d w=%d page_count=%d -> index=%d (was %d)",
             (int)x, (int)w, s_page_count, index, s_index);

    // false: the container is already physically here — see show_index_impl.
    show_index_impl(index, false);
}

static void scroll_begin_cb(lv_event_t *e)
{
    ESP_LOGI("fav", "scroll_begin fired on %p (s_container=%p)",
             lv_event_get_target(e), s_container);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    int count = sonos_device_fav_count();
    if (count != s_last_count) {
        if (s_index >= count && count > 0) s_index = 0;
        ui_favourites_show_index(s_index);
    }
}

// Called when this screen finishes loading (animation complete). Always
// starts back at the first favourite rather than remembering where you
// left off — visiting Favourites should be predictable, not stateful.
static void scr_loaded_cb(lv_event_t *e)
{
    ui_favourites_show_index(0);
}

#define ART_SLIDE_IN_PX 70
#define ART_SLIDE_IN_MS 220

static void art_slide_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)obj, (lv_coord_t)v, 0);
}

// Requests blob art for a real favourite page (not the add slot, not out
// of range). Cheap to call speculatively — ui_art_request_blob no-ops if
// this tag is already cached or already queued. Bounded by s_page_count,
// which is only valid once the carousel has actually been built — for
// requesting art before that (boot time), see ui_favourites_prefetch_all.
static void request_art_for(int idx)
{
    if (idx < 0 || idx >= s_page_count) return;
    if (idx >= sonos_device_fav_count()) return;   // add slot or stale range

    const uint8_t *art_data = sonos_device_fav_art_data(idx);
    size_t         art_sz   = sonos_device_fav_art_size(idx);
    if (art_sz > 0 && art_data) {
        ui_art_request_blob(idx, art_data, art_sz);
    }
}

// Public: request every current favourite's art, independent of whether
// the carousel screen has ever been built (s_page_count may still be 0 —
// this is what makes it safe to call at boot, before the user has ever
// opened Favourites).
void ui_favourites_prefetch_all(void)
{
    int count = sonos_device_fav_count();
    if (count > MAX_FAVOURITES) count = MAX_FAVOURITES;
    for (int i = 0; i < count; i++) {
        const uint8_t *art_data = sonos_device_fav_art_data(i);
        size_t         art_sz   = sonos_device_fav_art_size(i);
        if (art_sz > 0 && art_data) {
            ui_art_request_blob(i, art_data, art_sz);
        }
    }
}

// Reveals art for page `idx` with the slide-in animation, if a decode is
// ready for it. Called for the current page AND its immediate neighbours
// (see art_timer_cb) — revealing an off-screen neighbour is harmless (it's
// not visible yet), and revealing one that's currently PARTIALLY visible
// mid-drag is exactly what gives the "new art slides in with the swipe"
// effect: since ui_art_request_blob was already called for it back when
// we arrived at its neighbour (see show_index_impl), it often finishes
// decoding while the page is still moving, so it can look like it's
// riding along with the real scroll rather than popping in after landing.
static void reveal_art_if_ready(int idx)
{
    if (idx < 0 || idx >= s_page_count) return;
    lv_obj_t *art = s_page_art[idx];
    if (!art) return;
    // ui_art_update_blob is idempotent now (a favourite's cached art never
    // goes stale — see ui_art.h) and returns true on every call once
    // content exists, not just the first. Without this guard, art_timer_cb
    // re-triggered the slide-in animation on the current (and neighbour)
    // pages every 500ms tick forever, with no interaction at all — visibly
    // sliding back and forth continuously.
    if (!lv_obj_has_flag(art, LV_OBJ_FLAG_HIDDEN)) return;
    if (!ui_art_update_blob(art, idx)) return;

    lv_obj_set_style_translate_x(art, s_enter_dir * ART_SLIDE_IN_PX, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, art);
    lv_anim_set_exec_cb(&a, art_slide_anim_cb);
    lv_anim_set_values(&a, s_enter_dir * ART_SLIDE_IN_PX, 0);
    lv_anim_set_time(&a, ART_SLIDE_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void art_timer_cb(lv_timer_t *t)
{
    if (lv_scr_act() != s_scr) return;
    reveal_art_if_ready(s_index - 1);
    reveal_art_if_ready(s_index);
    reveal_art_if_ready(s_index + 1);
}

static void scr_del_cb(lv_event_t *e)
{
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    if (s_art_timer)     { lv_timer_del(s_art_timer);     s_art_timer     = NULL; }
    s_scr = NULL;
    s_container = NULL;
    s_add_btn = s_qr = s_qr_url = NULL;
    s_dots_cont = NULL;
    s_dot_count = 0;
    s_page_count = 0;
    memset(s_dots,     0, sizeof(s_dots));
    memset(s_page,     0, sizeof(s_page));
    memset(s_page_art, 0, sizeof(s_page_art));
}

// ---- Create ------------------------------------------------------------

lv_obj_t *ui_favourites_create(void)
{
    s_scr = lv_obj_create(NULL);
    ui_screen_base_style(s_scr);
    lv_obj_add_event_cb(s_scr, gesture_cb,    LV_EVENT_GESTURE,       NULL);
    lv_obj_add_event_cb(s_scr, scr_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, scr_del_cb,    LV_EVENT_DELETE,        NULL);

    // Horizontal scroll-snap row — one page per favourite + the add slot.
    // SCROLL_ONE keeps a single swipe to exactly one page (no multi-page
    // flinging). Every LVGL object with a parent defaults to
    // GESTURE_BUBBLE=true (lv_obj.c's constructor), so a swipe at the
    // first/last page (which the container can't consume further)
    // bubbles all the way up through page -> s_container -> s_scr — a
    // parentless root screen never gets that flag by default, so s_scr is
    // where it actually lands. gesture_cb is registered there, not here.
    s_container = lv_obj_create(s_scr);
    lv_obj_set_size(s_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_set_scrollbar_mode(s_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_container, LV_SCROLL_SNAP_CENTER);
    // SCROLL_ELASTIC is on by default and, critically, LVGL skips GESTURE
    // events entirely whenever a scroll_obj has claimed the touch (even
    // just for an elastic rubber-band bounce at a boundary) — meaning an
    // edge swipe never reached gesture_cb at all. Disabling elastic lets a
    // true boundary swipe (no real content left to scroll) fall through
    // to gesture recognition instead, which is what go_exit relies on.
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_add_event_cb(s_container, scroll_end_cb,   LV_EVENT_SCROLL_END,   NULL);
    lv_obj_add_event_cb(s_container, scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    // CLICKED (unlike GESTURE) doesn't bubble by default — LV_OBJ_FLAG_
    // EVENT_BUBBLE is a separate, opt-in flag nothing here sets. It fires
    // on the actual pressed object, which resolves to s_container (page/
    // art are non-clickable, s_container is the nearest clickable
    // ancestor) — registering this on s_scr instead meant tap-to-play
    // never fired at all.
    lv_obj_add_event_cb(s_container, screen_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_container, container_pressed_cb,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_container, container_released_cb, LV_EVENT_RELEASED, NULL);

    // "Favourites" category label — near top, above art, fixed (not part
    // of the scrolling row — same on every page).
    lv_obj_t *cat = lv_label_create(s_scr);
    lv_label_set_text(cat, "Favourites");
    lv_obj_set_style_text_color(cat, COL_ACCENT, 0);
    lv_obj_set_style_text_font(cat, &lv_font_montserrat_24, 0);
    lv_obj_align(cat, LV_ALIGN_CENTER, 0, -170);
    lv_obj_clear_flag(cat, LV_OBJ_FLAG_CLICKABLE);

    // Page-dot indicator — bottom of screen, one dot per slot, fixed
    s_dots_cont = lv_obj_create(s_scr);
    lv_obj_set_size(s_dots_cont, 260, 16);
    lv_obj_align(s_dots_cont, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_opa(s_dots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots_cont, 0, 0);
    lv_obj_set_style_pad_all(s_dots_cont, 0, 0);
    lv_obj_set_flex_flow(s_dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_dots_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_last_count    = -1;
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 3000, NULL);
    s_art_timer     = lv_timer_create(art_timer_cb,     500,  NULL);

    ui_favourites_show_index(0);

    // Home button last → topmost z-order. Nudged up a little for spacing.
    lv_obj_t *home_btn = ui_add_home_btn(s_scr);
    lv_obj_align(home_btn, LV_ALIGN_CENTER, 0, -198);
    return s_scr;
}

// ---- Show ----------------------------------------------------------

// scroll_to: false when called from scroll_end_cb — the container is
// already physically at this index (that's what just fired scroll_end),
// so re-scrolling it is not just redundant but actively harmful: it
// triggers a fresh scroll_begin/scroll_end pair, which recomputes an
// index and calls back in here, cascading into a rapid feedback loop
// that was observed oscillating (452 -> 0 -> -452 -> 480 -> 0) and
// landing wherever it happened to stop — this is why swiping looked
// like it "didn't work" despite the drag itself being fine.
static void show_index_impl(int index, bool scroll_to)
{
    // Must come first: build_pages() below calls lv_obj_clean(s_container),
    // and s_container doesn't exist until ui_favourites_create() has run.
    // go_favourites() now calls this pre-emptively (to reset scroll
    // position before the screen becomes visible) — on the very first
    // ever visit this boot, that happens before the screen exists at all,
    // and this used to crash (LoadProhibited in lv_obj_clean(NULL)).
    if (!s_scr || !s_container) return;

    int count = sonos_device_fav_count();
    if (count != s_last_count) {
        // Skip on the very first build (s_last_count starts at -1) — that
        // would just throw away ui_favourites_prefetch_all()'s boot-time
        // work for nothing. On any REAL change (add/remove while already
        // running), invalidate everything: a removal shifts every later
        // favourite's index down, so a permanent slot keyed by index could
        // otherwise go on holding a different favourite's stale content.
        if (s_last_count != -1) {
            ui_art_blob_invalidate_all();
            ui_favourites_prefetch_all();
        }
        s_last_count = count;
        build_pages(count);
    }

    if (index >= s_page_count) index = s_page_count - 1;
    if (index < 0) index = 0;
    int prev_index = s_index;
    bool index_changed = (prev_index != index);
    if (index_changed) s_enter_dir = (index > prev_index) ? 1 : -1;
    s_index = index;

    // Hide the page we're leaving's art — each page keeps its own art
    // image object alive permanently (never destroyed until the count
    // changes). It'll re-reveal instantly (no re-decode) if you come
    // straight back, since its decoded content is cached permanently in
    // ui_art.c (see ui_art_request_blob), not evicted.
    if (index_changed && prev_index >= 0 && prev_index < MAX_PAGES && s_page_art[prev_index]) {
        lv_obj_add_flag(s_page_art[prev_index], LV_OBJ_FLAG_HIDDEN);
    }

    rebuild_dots(count + 1);
    update_dots(index);

    if (scroll_to && s_page[index]) {
        lv_obj_scroll_to_view(s_page[index], LV_ANIM_OFF);
    }

    bool is_add = (index >= count);
    if (is_add) {
        if (index_changed) ui_art_request(NULL);
    } else if (index_changed) {
        // Proactively request both neighbours too — ui_art_request_blob
        // no-ops if a tag is already cached/queued (favourites' art slots
        // are permanent, see ui_art.h), so this is cheap, and it's what
        // gives the incoming page's art a real chance to be ready before
        // you even finish the next swipe (see reveal_art_if_ready).
        request_art_for(index);
        request_art_for(index - 1);
        request_art_for(index + 1);
    }
}

void ui_favourites_show_index(int index)
{
    show_index_impl(index, true);
}
