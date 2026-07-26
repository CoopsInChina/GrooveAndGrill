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

static struct {
    uint16_t conn_handle;
    uint16_t val_handle;      // temperature characteristic value handle
    bool     cccd_done;       // CCCD write already issued this connection
    bool     connected;
    float    temp_c;
    uint8_t  prefix;
    int8_t   rssi;
    uint32_t last_ms;
    bool     ever;
} s;

static SemaphoreHandle_t s_mutex;

static inline uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static int gap_event(struct ble_gap_event *event, void *arg);

// ---- Scan / connect ------------------------------------------------------

static void start_scan(void)
{
    s.connected   = false;
    s.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s.val_handle  = 0;

    struct ble_gap_disc_params p = {0};
    p.passive = 1;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "disc rc=%d", rc);
    else         ESP_LOGI(TAG, "scanning for \"%s\"", PROBE_NAME);
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
    if (err->status == 0) {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s.connected = true;
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(TAG, "subscribed — streaming temperature");
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
    if (err->status != 0 && err->status != BLE_HS_EDONE) return 0;
    if (s.cccd_done) return 0;             // subscribe to the first CCCD only
    if (dsc && ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0) {
        s.cccd_done = true;
        uint8_t val[2] = { 0x01, 0x00 };   // notifications on
        ble_gattc_write_flat(conn, dsc->handle, val, sizeof(val), on_cccd_written, NULL);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (chr && err->status == 0) {
        s.val_handle = chr->val_handle;
        // Find the CCCD in the handles just above the value handle.
        ble_gattc_disc_all_dscs(conn, chr->val_handle, 0xffff, on_dsc, NULL);
    } else if (err->status == BLE_HS_EDONE && s.val_handle == 0) {
        ESP_LOGW(TAG, "temp characteristic not found — disconnecting");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

// ---- Notification parse --------------------------------------------------

static void on_notify(struct ble_gap_event *event)
{
    if (event->notify_rx.attr_handle != s.val_handle) return;

    char buf[16];
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len < 2 || len >= sizeof(buf)) return;
    os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
    buf[len] = '\0';

    // Payload: [prefix byte][ASCII decimal temperature]. Trust only the temp;
    // keep the prefix raw (its meaning is unresolved — see the protocol doc).
    char *end = NULL;
    float t = strtof(buf + 1, &end);
    if (end == buf + 1) return;             // not numeric — ignore

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s.prefix  = (uint8_t)buf[0];
        s.temp_c  = t;
        s.last_ms = ms_now();
        s.ever    = true;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "temp=%.1f C (prefix=0x%02x)", t, (uint8_t)buf[0]);
}

// ---- GAP event handler ---------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (adv_is_probe(event->disc.data, event->disc.length_data)) {
            s.rssi = event->disc.rssi;
            ble_gap_disc_cancel();   // can't connect while scanning
            // NULL conn params -> NimBLE defaults (a zeroed struct would pass
            // invalid 0 intervals to the controller).
            int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                                     10000, NULL, gap_event, NULL);
            if (rc != 0) { ESP_LOGE(TAG, "connect rc=%d", rc); start_scan(); }
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s.conn_handle = event->connect.conn_handle;
            s.val_handle  = 0;
            s.cccd_done   = false;
            ESP_LOGI(TAG, "connected — discovering temperature characteristic");
            ble_gattc_disc_chrs_by_uuid(s.conn_handle, 1, 0xffff,
                                        &TEMP_CHR_UUID.u, on_chr, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_scan();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason=%d) — rescanning", event->disconnect.reason);
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s.connected = false;
            xSemaphoreGive(s_mutex);
        }
        start_scan();
        break;

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
    s.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "started (central)");
}

// ---- Accessors -----------------------------------------------------------

uint32_t ble_probe_age_ms(void)
{
    if (!s.ever) return UINT32_MAX;
    return ms_now() - s.last_ms;
}

bool ble_probe_connected(void) { return s.connected; }

bool ble_probe_get(float *temp_c)
{
    if (!s.connected || ble_probe_age_ms() > FRESH_WINDOW_MS) return false;
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s.ever) { if (temp_c) *temp_c = s.temp_c; ok = true; }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}
