#include "bbq_controller.h"
#include "bbq_ble.h"
#include "ble_probe.h"
#include "buzzer.h"
#include "app_config.h"
#include "esp_timer.h"
#include "app_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char *TAG = "bbq";

// ---- Persisted allocation record (source of truth for assignment) ----------
typedef struct {
    bool          used;
    sensor_src_t  src;
    uint8_t       hw_id;
    uint8_t       grill_num;
    sensor_role_t role;
    meat_kind_t   meat_kind;
    int16_t       target_c;
    // ---- session-only alarm tracking (NOT persisted — resets on reboot,
    // matching "lost connection since boot", not "never connected") ----
    bool          ever_present;
    float         last_temp_c;
} alloc_t;

static alloc_t          s_alloc[MAX_BBQ_SENSORS];

// Live pool, rebuilt every poll from bbq_ble + s_alloc.
static bbq_sensor_t     s_sensors[MAX_BBQ_SENSORS];
static int              s_sensor_count;

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_poll_timer;

// Legacy shim: the old UI always shows >=1 grill and can "add" up to MAX_GRILLS.
static int              s_legacy_grills = 1;

// BLE source mode, loaded once at init (see bbq_source_get/set).
static bbq_source_t     s_source = BBQ_SRC_BOX;

// ---- NVS persistence -------------------------------------------------------
// Blob layout: [ver=1][count] then count × {src,hw_id,grill,role,kind,tgt_lo,tgt_hi}
#define ALLOC_REC_BYTES   7
#define ALLOC_BLOB_VER    1

static void alloc_save(void)
{
    uint8_t blob[2 + MAX_BBQ_SENSORS * ALLOC_REC_BYTES];
    int n = 0, count = 0;
    blob[0] = ALLOC_BLOB_VER;
    n = 2;
    for (int i = 0; i < MAX_BBQ_SENSORS; i++) {
        if (!s_alloc[i].used || s_alloc[i].role == ROLE_UNASSIGNED) continue;
        blob[n++] = (uint8_t)s_alloc[i].src;
        blob[n++] = s_alloc[i].hw_id;
        blob[n++] = s_alloc[i].grill_num;
        blob[n++] = (uint8_t)s_alloc[i].role;
        blob[n++] = (uint8_t)s_alloc[i].meat_kind;
        blob[n++] = (uint8_t)(s_alloc[i].target_c & 0xFF);
        blob[n++] = (uint8_t)((s_alloc[i].target_c >> 8) & 0xFF);
        count++;
    }
    blob[1] = (uint8_t)count;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, NVS_KEY_BBQ_ALLOC, blob, n);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void alloc_load(void)
{
    memset(s_alloc, 0, sizeof(s_alloc));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;

    uint8_t blob[2 + MAX_BBQ_SENSORS * ALLOC_REC_BYTES];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY_BBQ_ALLOC, blob, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len < 2 || blob[0] != ALLOC_BLOB_VER) return;

    int count = blob[1];
    int n = 2, slot = 0;
    for (int i = 0; i < count && slot < MAX_BBQ_SENSORS; i++) {
        if (n + ALLOC_REC_BYTES > (int)len) break;
        alloc_t *a = &s_alloc[slot++];
        a->used      = true;
        a->src       = (sensor_src_t)blob[n++];
        a->hw_id     = blob[n++];
        a->grill_num = blob[n++];
        a->role      = (sensor_role_t)blob[n++];
        a->meat_kind = (meat_kind_t)blob[n++];
        a->target_c  = (int16_t)(blob[n] | (blob[n + 1] << 8));
        n += 2;
    }
    LOGI(TAG, "loaded %d sensor allocation(s)", slot);
}

// ---- Direct-probe slot identity (see header) --------------------------------
typedef struct { bool used; uint8_t hw_id; } probe_slot_t;
static probe_slot_t s_probe_slots[MAX_DIRECT_PROBES];

#define PSLOT_BLOB_VER 1

static void probe_slots_save(void)
{
    uint8_t blob[1 + MAX_DIRECT_PROBES * 2];
    blob[0] = PSLOT_BLOB_VER;
    for (int i = 0; i < MAX_DIRECT_PROBES; i++) {
        blob[1 + i * 2]     = s_probe_slots[i].used ? 1 : 0;
        blob[1 + i * 2 + 1] = s_probe_slots[i].hw_id;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, NVS_KEY_BBQ_PSLOTS, blob, sizeof(blob));
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void probe_slots_load(void)
{
    memset(s_probe_slots, 0, sizeof(s_probe_slots));
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t blob[1 + MAX_DIRECT_PROBES * 2];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY_BBQ_PSLOTS, blob, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len < 1 || blob[0] != PSLOT_BLOB_VER) return;
    for (int i = 0; i < MAX_DIRECT_PROBES && (size_t)(1 + i * 2 + 1) < len; i++) {
        s_probe_slots[i].used  = blob[1 + i * 2] != 0;
        s_probe_slots[i].hw_id = blob[1 + i * 2 + 1];
    }
}

// Lock-free — only called from within poll_cb, which already holds s_lock.
static void probe_slot_touch(uint8_t hw_id)
{
    for (int i = 0; i < MAX_DIRECT_PROBES; i++)
        if (s_probe_slots[i].used && s_probe_slots[i].hw_id == hw_id) return;
    for (int i = 0; i < MAX_DIRECT_PROBES; i++) {
        if (!s_probe_slots[i].used) {
            s_probe_slots[i].used  = true;
            s_probe_slots[i].hw_id = hw_id;
            probe_slots_save();
            LOGI(TAG, "probe id=0x%02x bonded to slot %d", hw_id, i + 1);
            return;
        }
    }
    // All slots taken by other probes — nothing to do (capped at MAX_DIRECT_PROBES).
}

static void probe_slot_release(uint8_t hw_id)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    for (int i = 0; i < MAX_DIRECT_PROBES; i++) {
        if (s_probe_slots[i].used && s_probe_slots[i].hw_id == hw_id) {
            s_probe_slots[i].used  = false;
            s_probe_slots[i].hw_id = 0;
            xSemaphoreGive(s_lock);
            probe_slots_save();
            LOGI(TAG, "probe id=0x%02x released from slot %d", hw_id, i + 1);
            return;
        }
    }
    xSemaphoreGive(s_lock);
}

int bbq_probe_slot_of(uint8_t hw_id)
{
    int slot = -1;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < MAX_DIRECT_PROBES; i++)
            if (s_probe_slots[i].used && s_probe_slots[i].hw_id == hw_id) { slot = i; break; }
        xSemaphoreGive(s_lock);
    }
    return slot;
}

bool bbq_probe_slot_get(int slot, uint8_t *hw_id_out)
{
    if (slot < 0 || slot >= MAX_DIRECT_PROBES) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_probe_slots[slot].used) {
            if (hw_id_out) *hw_id_out = s_probe_slots[slot].hw_id;
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

// ---- BLE source mode -------------------------------------------------------
static void source_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t v = BBQ_SRC_BOX;
    if (nvs_get_u8(nvs, NVS_KEY_BBQ_SOURCE, &v) == ESP_OK)
        s_source = (v == BBQ_SRC_PROBE) ? BBQ_SRC_PROBE : BBQ_SRC_BOX;
    nvs_close(nvs);
}

bbq_source_t bbq_source_get(void) { return s_source; }

static void poll_cb(void *arg);   // rebuild the live pool (defined below)

bool bbq_link_up(void)
{
    return (s_source == BBQ_SRC_PROBE) ? ble_probe_any() : bbq_ble_present();
}

void bbq_radio_pause(bool pause)
{
    if (s_source == BBQ_SRC_PROBE) {
        if (pause) ble_probe_scan_pause();
        else       ble_probe_scan_resume();
    } else {
        if (pause) bbq_ble_scan_pause();
        else       bbq_ble_scan_resume();
    }
}

void bbq_clear_all(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    memset(s_alloc, 0, sizeof(s_alloc));
    memset(s_probe_slots, 0, sizeof(s_probe_slots));
    xSemaphoreGive(s_lock);
    alloc_save();
    probe_slots_save();
    poll_cb(NULL);     // rebuild the live pool immediately (→ no views)
    LOGI(TAG, "cleared all sensor allocations");
}

void bbq_source_set(bbq_source_t src)
{
    s_source = src;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, NVS_KEY_BBQ_SOURCE, (uint8_t)src);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static alloc_t *alloc_find(sensor_src_t src, uint8_t hw_id)
{
    for (int i = 0; i < MAX_BBQ_SENSORS; i++)
        if (s_alloc[i].used && s_alloc[i].src == src && s_alloc[i].hw_id == hw_id)
            return &s_alloc[i];
    return NULL;
}

static alloc_t *alloc_get_or_add(sensor_src_t src, uint8_t hw_id)
{
    alloc_t *a = alloc_find(src, hw_id);
    if (a) return a;
    for (int i = 0; i < MAX_BBQ_SENSORS; i++) {
        if (!s_alloc[i].used) {
            s_alloc[i].used  = true;
            s_alloc[i].src   = src;
            s_alloc[i].hw_id = hw_id;
            return &s_alloc[i];
        }
    }
    return NULL;
}

// ---- Live pool rebuild (1 Hz) ----------------------------------------------
static void add_sensor(sensor_src_t src, uint8_t hw_id, bool present, float temp)
{
    // Merge with any existing entry for this identity.
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].src == src && s_sensors[i].hw_id == hw_id) {
            if (present) { s_sensors[i].present = true; s_sensors[i].temp_c = temp; }
            return;
        }
    }
    if (s_sensor_count >= MAX_BBQ_SENSORS) return;

    bbq_sensor_t *s = &s_sensors[s_sensor_count++];
    memset(s, 0, sizeof(*s));
    s->src     = src;
    s->hw_id   = hw_id;
    s->present = present;
    s->temp_c  = temp;

    alloc_t *a = alloc_find(src, hw_id);
    if (a) {
        s->grill_num = a->grill_num;
        s->role      = a->role;
        s->meat_kind = a->meat_kind;
        s->target_c  = a->target_c;

        if (present) {
            a->ever_present = true;
            a->last_temp_c  = temp;
        } else if (a->ever_present) {
            // Was live at some point this session, now silent: alarm, and
            // keep showing the last reading rather than reverting to "- C".
            s->alarm  = true;
            s->temp_c = a->last_temp_c;
        }
    }
}

static void poll_cb(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;

    s_sensor_count = 0;

    if (s_source == BBQ_SRC_PROBE) {
        // Direct wireless-probe mode (no box): up to MAX_DIRECT_PROBES probes
        // we connect to over GATT, each keyed by its address byte. No
        // thermocouples exist in this mode.
        int n = ble_probe_count();
        for (int i = 0; i < n; i++) {
            uint8_t id = 0; float t = 0;
            if (ble_probe_at(i, &id, &t)) {
                probe_slot_touch(id);   // first time seen -> bonds a stable UI slot
                add_sensor(SRC_PROBE, id, true, t);
            }
        }
    } else {
        // Box mode: surface all 4 fixed thermocouple channels (present per the
        // box advert, else "not connected") so they can be allocated up front,
        // plus any wireless probes the box hub is currently forwarding.
        for (int ch = 0; ch < BBQ_TC_COUNT; ch++) {
            float t = 0;
            bool ok = bbq_ble_channel(ch, &t);
            add_sensor(SRC_TC, (uint8_t)ch, ok, t);
        }
        int pc = bbq_ble_probe_count();
        for (int i = 0; i < pc; i++) {
            uint8_t id = 0; float t = 0;
            if (!bbq_ble_probe(i, &id, &t)) continue;
            add_sensor(SRC_PROBE, id, !isnan(t), isnan(t) ? 0.0f : t);
        }
    }

    // 3) Allocated-but-absent sensors (e.g. a configured probe that dropped)
    //    so they stay visible/configurable — but only those valid for the
    //    active source. In direct-probe mode there are no thermocouples, so a
    //    leftover TC allocation (e.g. from box-mode setup) must not conjure a
    //    grill view; in box mode a stray direct-probe id likewise doesn't apply.
    for (int i = 0; i < MAX_BBQ_SENSORS; i++) {
        if (!s_alloc[i].used || s_alloc[i].role == ROLE_UNASSIGNED) continue;
        if (s_source == BBQ_SRC_PROBE && s_alloc[i].src == SRC_TC) continue;
        // Backfill: an allocation from before this boot (or before the slot
        // table existed) still needs a bonded slot.
        if (s_source == BBQ_SRC_PROBE && s_alloc[i].src == SRC_PROBE)
            probe_slot_touch(s_alloc[i].hw_id);
        add_sensor(s_alloc[i].src, s_alloc[i].hw_id, false, 0.0f);
    }

    bool any_alarm = false;
    for (int i = 0; i < s_sensor_count; i++)
        if (s_sensors[i].alarm) { any_alarm = true; break; }

    xSemaphoreGive(s_lock);

    buzzer_set_alarm(any_alarm);   // outside the lock — keeps I2C off the critical section
}

void bbq_controller_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    alloc_load();
    source_load();
    probe_slots_load();
    poll_cb(NULL);   // seed the pool immediately

    const esp_timer_create_args_t targs = { .callback = poll_cb, .name = "bbq_poll" };
    if (esp_timer_create(&targs, &s_poll_timer) == ESP_OK)
        esp_timer_start_periodic(s_poll_timer, 1000000);   // 1 Hz
}

// ---- Sensor pool API -------------------------------------------------------
int bbq_sensor_count(void)
{
    int n = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        n = s_sensor_count;
        xSemaphoreGive(s_lock);
    }
    return n;
}

bool bbq_sensor_at(int i, bbq_sensor_t *out)
{
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (i >= 0 && i < s_sensor_count) { if (out) *out = s_sensors[i]; ok = true; }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

bool bbq_sensor_get(sensor_src_t src, uint8_t hw_id, bbq_sensor_t *out)
{
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_sensor_count; i++) {
            if (s_sensors[i].src == src && s_sensors[i].hw_id == hw_id) {
                if (out) *out = s_sensors[i];
                ok = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

void bbq_sensor_assign(sensor_src_t src, uint8_t hw_id, uint8_t grill_num,
                       sensor_role_t role, meat_kind_t kind, int target_c)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        alloc_t *a = alloc_get_or_add(src, hw_id);
        if (a) {
            a->grill_num = grill_num;
            a->role      = role;
            a->meat_kind = (role == ROLE_MEAT) ? kind : MEAT_KIND_NONE;
            a->target_c  = (int16_t)target_c;
            if (role == ROLE_UNASSIGNED) a->used = false;   // freed slot
        }
        alloc_save();
        xSemaphoreGive(s_lock);
    }
    poll_cb(NULL);   // reflect immediately in the live pool
}

void bbq_sensor_unassign(sensor_src_t src, uint8_t hw_id)
{
    bbq_sensor_assign(src, hw_id, 0, ROLE_UNASSIGNED, MEAT_KIND_NONE, 0);
    // Free its bonded UI slot too, so a different probe can claim that
    // position — "bonded until removed" per the explicit remove action.
    if (src == SRC_PROBE) probe_slot_release(hw_id);
}

// ---- Derived cook views ----------------------------------------------------
int bbq_view_count(void)
{
    int n = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    // one view per assigned meat sensor …
    for (int i = 0; i < s_sensor_count; i++)
        if (s_sensors[i].role == ROLE_MEAT && s_sensors[i].grill_num) n++;
    // … plus one ambient-only view per grill that has a grill sensor but no meat.
    for (int g = 1; g <= MAX_GRILLS; g++) {
        bool has_grill = false, has_meat = false;
        for (int i = 0; i < s_sensor_count; i++) {
            if (s_sensors[i].grill_num != g) continue;
            if (s_sensors[i].role == ROLE_GRILL) has_grill = true;
            if (s_sensors[i].role == ROLE_MEAT)  has_meat  = true;
        }
        if (has_grill && !has_meat) n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

// Fill the grill-ambient fields of a view from grill number g (lock held).
static void fill_grill_ambient(bbq_view_t *v, uint8_t g)
{
    v->grill_num = g;
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].grill_num == g && s_sensors[i].role == ROLE_GRILL) {
            v->grill_assigned = true;
            v->grill_present  = s_sensors[i].present;
            v->grill_alarm    = s_sensors[i].alarm;
            v->grill_temp_c   = s_sensors[i].temp_c;
            v->grill_target_c = s_sensors[i].target_c;
            v->grill_src      = s_sensors[i].src;
            v->grill_hw_id    = s_sensors[i].hw_id;
            return;
        }
    }
}

bool bbq_grill_has_ambient(uint8_t grill_num, sensor_src_t *src, uint8_t *hw_id)
{
    bool found = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_sensor_count; i++) {
            if (s_sensors[i].grill_num == grill_num && s_sensors[i].role == ROLE_GRILL) {
                if (src)   *src   = s_sensors[i].src;
                if (hw_id) *hw_id = s_sensors[i].hw_id;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return found;
}

// ---- On-screen wizard context ---------------------------------------------
static bbq_setup_t s_setup;
static bool        s_setup_valid;

void bbq_setup_set(const bbq_setup_t *s)
{
    if (!s) { s_setup_valid = false; return; }
    s_setup = *s;
    s_setup_valid = true;
}

bool bbq_setup_get(bbq_setup_t *out)
{
    if (!s_setup_valid || !out) return false;
    *out = s_setup;
    return true;
}

int bbq_view_index_for(sensor_src_t src, uint8_t hw_id)
{
    int idx = -1;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return -1;

    int seen = 0;
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].role != ROLE_MEAT || !s_sensors[i].grill_num) continue;
        if (s_sensors[i].src == src && s_sensors[i].hw_id == hw_id) { idx = seen; break; }
        seen++;
    }
    if (idx < 0) {
        for (int g = 1; g <= MAX_GRILLS && idx < 0; g++) {
            bool has_grill = false, has_meat = false;
            sensor_src_t gsrc = SRC_TC; uint8_t ghw = 0;
            for (int i = 0; i < s_sensor_count; i++) {
                if (s_sensors[i].grill_num != g) continue;
                if (s_sensors[i].role == ROLE_GRILL) { has_grill = true; gsrc = s_sensors[i].src; ghw = s_sensors[i].hw_id; }
                if (s_sensors[i].role == ROLE_MEAT)  has_meat = true;
            }
            if (!has_grill || has_meat) continue;
            if (gsrc == src && ghw == hw_id) idx = seen;
            seen++;
        }
    }

    xSemaphoreGive(s_lock);
    return idx;
}

bool bbq_can_add_more(void)
{
    if (s_source != BBQ_SRC_PROBE) return true;   // box mode: unrestricted here

    bool can = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < MAX_DIRECT_PROBES; i++) {
            if (!s_probe_slots[i].used) { can = true; break; }   // room for a new identity
            alloc_t *a = alloc_find(SRC_PROBE, s_probe_slots[i].hw_id);
            if (!a || !a->used || a->role == ROLE_UNASSIGNED) { can = true; break; } // bonded but not yet a meat
        }
        xSemaphoreGive(s_lock);
    }
    return can;
}

bool bbq_view_at(int idx, bbq_view_t *out)
{
    if (!out || idx < 0) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;

    int seen = 0;

    // Pass 1: one view per assigned meat sensor.
    for (int i = 0; i < s_sensor_count && !ok; i++) {
        if (s_sensors[i].role != ROLE_MEAT || !s_sensors[i].grill_num) continue;
        if (seen++ != idx) continue;
        memset(out, 0, sizeof(*out));
        fill_grill_ambient(out, s_sensors[i].grill_num);
        out->has_meat      = true;
        out->meat_src      = s_sensors[i].src;
        out->meat_hw_id    = s_sensors[i].hw_id;
        out->meat_kind     = s_sensors[i].meat_kind;
        out->meat_present  = s_sensors[i].present;
        out->meat_alarm    = s_sensors[i].alarm;
        out->meat_temp_c   = s_sensors[i].temp_c;
        out->meat_target_c = s_sensors[i].target_c;
        ok = true;
    }

    // Pass 2: ambient-only views for grills with a grill sensor but no meat.
    for (int g = 1; g <= MAX_GRILLS && !ok; g++) {
        bool has_grill = false, has_meat = false;
        for (int i = 0; i < s_sensor_count; i++) {
            if (s_sensors[i].grill_num != g) continue;
            if (s_sensors[i].role == ROLE_GRILL) has_grill = true;
            if (s_sensors[i].role == ROLE_MEAT)  has_meat  = true;
        }
        if (!has_grill || has_meat) continue;
        if (seen++ != idx) continue;
        memset(out, 0, sizeof(*out));
        fill_grill_ambient(out, (uint8_t)g);
        out->has_meat = false;
        ok = true;
    }

    xSemaphoreGive(s_lock);
    return ok;
}

int bbq_meat_type_idx(meat_kind_t k)
{
    switch (k) {
        case MEAT_KIND_BEEF: return 0;
        case MEAT_KIND_LAMB: return 1;
        case MEAT_KIND_PORK: return 2;
        default:             return -1;   // none / chicken (single target)
    }
}

// ============================================================================
// Legacy grill API (shim over the sensor model)
// ----------------------------------------------------------------------------
// Legacy grill idx (0-based) maps to grill number idx+1. When that grill has
// no explicit allocation yet, we fall back to the historical fixed pairing —
// grill n = TC[2n] (ambient) + TC[2n+1] (meat) — so bench testing (plug a TC
// in, see grill 1) still works. Config-screen writes create real allocations.
// ============================================================================

int bbq_grill_count(void) { return s_legacy_grills; }

bool bbq_add_grill(void)
{
    if (s_legacy_grills >= MAX_GRILLS) return false;
    s_legacy_grills++;
    return true;
}

const bbq_grill_t *bbq_get_grill(int idx)
{
    static bbq_grill_t g;   // returned by pointer; single-threaded UI use
    if (idx < 0 || idx >= s_legacy_grills) return NULL;

    memset(&g, 0, sizeof(g));
    uint8_t grill_num = (uint8_t)(idx + 1);
    bool have_grill = false, have_meat = false;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_sensor_count; i++) {
            bbq_sensor_t *s = &s_sensors[i];
            if (s->grill_num != grill_num) continue;
            if (s->role == ROLE_GRILL) {
                if (s->present) g.grill_temp_c = s->temp_c;
                g.grill_target_c = s->target_c;
                have_grill = true;
                if (s->present) g.probe_state = PROBE_CONNECTED;
            } else if (s->role == ROLE_MEAT && !have_meat) {
                if (s->present) g.meat_temp_c = s->temp_c;
                g.meat_target_c = s->target_c;
                g.meat_kind     = s->meat_kind;
                have_meat = true;
                if (s->present) g.probe_state = PROBE_CONNECTED;
            }
        }
        // Fallback to the historical TC pairing for a still-unallocated grill.
        if (!have_grill && !have_meat) {
            for (int i = 0; i < s_sensor_count; i++) {
                bbq_sensor_t *s = &s_sensors[i];
                if (s->src != SRC_TC) continue;
                if (s->hw_id == idx * 2     && s->present) { g.grill_temp_c = s->temp_c; g.probe_state = PROBE_CONNECTED; }
                if (s->hw_id == idx * 2 + 1 && s->present) { g.meat_temp_c  = s->temp_c; g.probe_state = PROBE_CONNECTED; }
            }
        }
        xSemaphoreGive(s_lock);
    }

    g.configured = have_grill || have_meat;
    return &g;
}

void bbq_set_targets(int idx, int grill_target_c, int meat_target_c, meat_kind_t kind)
{
    if (idx < 0 || idx >= MAX_GRILLS) return;
    uint8_t grill_num = (uint8_t)(idx + 1);
    // Materialise the legacy grill onto its default TC pair.
    bbq_sensor_assign(SRC_TC, (uint8_t)(idx * 2),     grill_num, ROLE_GRILL, MEAT_KIND_NONE, grill_target_c);
    bbq_sensor_assign(SRC_TC, (uint8_t)(idx * 2 + 1), grill_num, ROLE_MEAT,  kind,           meat_target_c);
}

void bbq_set_grill_target(int idx, int grill_target_c)
{
    if (idx < 0 || idx >= MAX_GRILLS) return;
    bbq_sensor_assign(SRC_TC, (uint8_t)(idx * 2), (uint8_t)(idx + 1),
                      ROLE_GRILL, MEAT_KIND_NONE, grill_target_c);
}

// Real presence now comes from BLE; the old demo hooks are inert.
void bbq_mock_connect_probe(int idx) { (void)idx; }
void bbq_mock_toggle_probe(int idx)  { (void)idx; }
