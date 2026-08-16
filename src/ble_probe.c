#include "ble_probe.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ble_probe";

// Match the thermometer by advertised name (see protocol doc).
#define PROBE_NAME        "BBQ"
// Notifies ~1.56 s, but WiFi/BLE share one radio so notifications drop out for
// several seconds under WiFi load. BBQ temps move slowly, so hold the last
// reading through those gaps rather than flapping to "waiting".
#define FRESH_WINDOW_MS   30000

// Temperature characteristic 772ae377-b3d2-ff8e-1042-5481d1e03456.
// NimBLE stores 128-bit UUIDs little-endian, so the bytes are the display
// order reversed.
static const ble_uuid128_t TEMP_CHR_UUID =
    BLE_UUID128_INIT(0x56, 0x34, 0xe0, 0xd1, 0x81, 0x54, 0x42, 0x10,
                     0x8e, 0xff, 0xd2, 0xb3, 0x77, 0xe3, 0x2a, 0x77);

// CCCD is the standard 0x2902 descriptor.
static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(0x2902);

// ---- State ---------------------------------------------------------------

typedef struct {
    bool     in_use;          // slot holds a connection (connecting or up)
    bool     connected;       // subscribed and streaming
    uint16_t conn_handle;
    uint16_t val_handle;      // temperature characteristic value handle
    bool     cccd_done;       // CCCD write already issued this connection
    uint8_t  addr[6];         // peer address (dedupe scan + stable identity)
    uint8_t  id;              // low address byte → sensor hw_id
    float    temp_c;
    uint8_t  prefix;
    uint32_t last_ms;
    bool     ever;
} probe_t;

static probe_t s_probes[MAX_DIRECT_PROBES];
static bool    s_connecting;   // one connect attempt in flight at a time
static SemaphoreHandle_t s_mutex;

static inline uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static int gap_event(struct ble_gap_event *event, void *arg);

// ---- Slot helpers (called from the NimBLE host task) ---------------------

static int free_slot(void)
{
    for (int i = 0; i < MAX_DIRECT_PROBES; i++)
        if (!s_probes[i].in_use) return i;
    return -1;
}

static int slot_by_conn(uint16_t ch)
{
    for (int i = 0; i < MAX_DIRECT_PROBES; i++)
        if (s_probes[i].in_use && s_probes[i].conn_handle == ch) return i;
    return -1;
}

static bool addr_in_use(const uint8_t *a)
{
    for (int i = 0; i < MAX_DIRECT_PROBES; i++)
        if (s_probes[i].in_use && memcmp(s_probes[i].addr, a, 6) == 0) return true;
    return false;
}

// ---- Scan / connect ------------------------------------------------------

static void start_scan(void)
{
    if (s_connecting) return;            // can't scan and connect at once
    if (free_slot() < 0) {               // every slot taken — nothing to find
        ESP_LOGI(TAG, "all %d probe slots in use", MAX_DIRECT_PROBES);
        return;
    }
    struct ble_gap_disc_params p = {0};
    p.passive = 1;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "disc rc=%d", rc);
    else ESP_LOGI(TAG, "scanning for \"%s\"", PROBE_NAME);
}

// True if this advertisement's complete/short name equals PROBE_NAME.
static bool adv_is_probe(const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, data, len) != 0) return false;
    if (!f.name || f.name_len == 0) return false;
    return f.name_len == strlen(PROBE_NAME) &&
           memcmp(f.name, PROBE_NAME, f.name_len) == 0;
}

// ---- CCCD write (enable notifications) -----------------------------------

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg)
{
    int slot = slot_by_conn(conn);
    if (slot < 0) return 0;
    if (err->status == 0) {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_probes[slot].connected = true;
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(TAG, "id=0x%02x subscribed — streaming temperature", s_probes[slot].id);
        start_scan();   // look for the next probe if a slot remains
    } else {
        ESP_LOGW(TAG, "CCCD write failed status=%d — disconnecting", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

// Discover the temp characteristic's CCCD, then write it to enable notify.
static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    int slot = slot_by_conn(conn);
    if (slot < 0) return 0;
    if (err->status != 0 && err->status != BLE_HS_EDONE) return 0;
    if (s_probes[slot].cccd_done) return 0;    // subscribe to the first CCCD only
    if (dsc && ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0) {
        s_probes[slot].cccd_done = true;
        uint8_t val[2] = { 0x01, 0x00 };       // notifications on
        ble_gattc_write_flat(conn, dsc->handle, val, sizeof(val), on_cccd_written, NULL);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    int slot = slot_by_conn(conn);
    if (slot < 0) return 0;
    if (chr && err->status == 0) {
        s_probes[slot].val_handle = chr->val_handle;
        ble_gattc_disc_all_dscs(conn, chr->val_handle, 0xffff, on_dsc, NULL);
    } else if (err->status == BLE_HS_EDONE && s_probes[slot].val_handle == 0) {
        ESP_LOGW(TAG, "temp characteristic not found — disconnecting");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

// ---- Notification parse --------------------------------------------------

static void on_notify(struct ble_gap_event *event)
{
    int slot = slot_by_conn(event->notify_rx.conn_handle);
    if (slot < 0) return;
    if (event->notify_rx.attr_handle != s_probes[slot].val_handle) return;

    char buf[16];
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len < 2 || len >= sizeof(buf)) return;
    os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
    buf[len] = '\0';

    // Payload: [prefix byte][ASCII decimal temperature]. Trust only the temp;
    // keep the prefix raw (its meaning is unresolved — see the protocol doc).
    char *end = NULL;
    float t = strtof(buf + 1, &end);
    if (end == buf + 1) return;                 // not numeric — ignore

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_probes[slot].prefix  = (uint8_t)buf[0];
        s_probes[slot].temp_c  = t;
        s_probes[slot].last_ms = ms_now();
        s_probes[slot].ever    = true;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "id=0x%02x temp=%.1f C (prefix=0x%02x)",
             s_probes[slot].id, t, (uint8_t)buf[0]);
}

// ---- GAP event handler ---------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_connecting) break;                            // one at a time
        if (!adv_is_probe(event->disc.data, event->disc.length_data)) break;
        if (addr_in_use(event->disc.addr.val)) break;       // already have it
        if (free_slot() < 0) break;
        s_connecting = true;
        ble_gap_disc_cancel();                              // can't connect while scanning
        // NULL conn params -> NimBLE defaults (a zeroed struct passes invalid
        // 0 intervals to the controller).
        if (ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                            10000, NULL, gap_event, NULL) != 0) {
            s_connecting = false;
            start_scan();
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc d;
            int slot = free_slot();
            if (slot < 0 || ble_gap_conn_find(event->connect.conn_handle, &d) != 0) {
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            probe_t *pr = &s_probes[slot];
            if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
                memset(pr, 0, sizeof(*pr));
                pr->in_use      = true;
                pr->conn_handle = event->connect.conn_handle;
                memcpy(pr->addr, d.peer_id_addr.val, 6);
                pr->id          = d.peer_id_addr.val[0];
                xSemaphoreGive(s_mutex);
            }
            ESP_LOGI(TAG, "id=0x%02x connected — discovering temperature characteristic",
                     pr->id);
            ble_gattc_disc_chrs_by_uuid(pr->conn_handle, 1, 0xffff,
                                        &TEMP_CHR_UUID.u, on_chr, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
        }
        start_scan();   // resume scanning (for another probe, or retry)
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        int slot = slot_by_conn(event->disconnect.conn.conn_handle);
        if (slot >= 0) {
            ESP_LOGW(TAG, "id=0x%02x disconnected (reason=%d) — rescanning",
                     s_probes[slot].id, event->disconnect.reason);
            if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
                s_probes[slot].in_use    = false;
                s_probes[slot].connected = false;
                xSemaphoreGive(s_mutex);
            }
        }
        start_scan();
        break;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        on_notify(event);
        break;

    default:
        break;
    }
    return 0;
}

// ---- Lifecycle -----------------------------------------------------------

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    start_scan();
}

static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_probe_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "started (central, up to %d probes)", MAX_DIRECT_PROBES);
}

// ---- Accessors -----------------------------------------------------------

static bool slot_fresh(const probe_t *pr)
{
    return pr->in_use && pr->connected && pr->ever &&
           (ms_now() - pr->last_ms) <= FRESH_WINDOW_MS;
}

int ble_probe_count(void)
{
    // bbq_controller_init() seeds its pool before ble_probe_init() runs (it's
    // only started later, in main.c, for BBQ_SRC_PROBE mode) — s_mutex may
    // still be NULL at that first call.
    if (!s_mutex) return 0;
    int n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < MAX_DIRECT_PROBES; i++)
            if (slot_fresh(&s_probes[i])) n++;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

bool ble_probe_at(int idx, uint8_t *id, float *temp_c)
{
    if (!s_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = 0;
        for (int i = 0; i < MAX_DIRECT_PROBES; i++) {
            if (!slot_fresh(&s_probes[i])) continue;
            if (n == idx) {
                if (id)     *id     = s_probes[i].id;
                if (temp_c) *temp_c = s_probes[i].temp_c;
                ok = true;
                break;
            }
            n++;
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

bool ble_probe_any(void)
{
    if (!s_mutex) return false;
    bool any = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < MAX_DIRECT_PROBES; i++)
            if (s_probes[i].in_use && s_probes[i].connected) { any = true; break; }
        xSemaphoreGive(s_mutex);
    }
    return any;
}
