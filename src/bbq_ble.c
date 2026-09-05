#include "bbq_ble.h"
#include "app_log.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "bbq_ble";

// Must match the box firmware (app_config.h + docs/ble_protocol.md there).
#define BOX_COMPANY_ID     0xFFFF
#define BOX_PROTO_VERSION  0x02
// Fixed header before the probe list: company(2) ver(1) fault(1) TCx4(8) count(1)
#define MFG_FIXED_LEN      13
#define PROBE_ENTRY_LEN    3        // [id:1][temp:uint16 LE]
#define FRESH_WINDOW_MS    5000     // box advertises ~1 Hz; 5 s = stale

static struct {
    float    temp_c[BBQ_BLE_CHANNELS];
    bool     ch_ok[BBQ_BLE_CHANNELS];
    uint8_t  probe_id[BBQ_BLE_MAX_PROBES];
    float    probe_c[BBQ_BLE_MAX_PROBES];
    int      probe_count;
    uint32_t last_ms;
    bool     ever;
} s_state;

static SemaphoreHandle_t s_mutex;

static inline uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// Parse the box's manufacturer payload out of an advertisement.
static void handle_adv(const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, data, len) != 0) return;
    if (fields.mfg_data == NULL || fields.mfg_data_len < MFG_FIXED_LEN) return;

    const uint8_t *m = fields.mfg_data;
    uint16_t company = (uint16_t)m[0] | ((uint16_t)m[1] << 8);
    if (company != BOX_COMPANY_ID || m[2] != BOX_PROTO_VERSION) return;

    // Probe list: count at offset 12, entries follow. Trust the smaller of the
    // advertised count and what the payload actually contains (defensive against
    // a truncated advert), and cap at what we can store.
    int count = m[12];
    int avail = ((int)fields.mfg_data_len - MFG_FIXED_LEN) / PROBE_ENTRY_LEN;
    if (count > avail)              count = avail;
    if (count > BBQ_BLE_MAX_PROBES) count = BBQ_BLE_MAX_PROBES;
    if (count < 0)                  count = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    for (int i = 0; i < BBQ_BLE_CHANNELS; i++) {
        uint16_t raw = (uint16_t)m[4 + i * 2] | ((uint16_t)m[4 + i * 2 + 1] << 8);
        if (raw == 0xFFFF) {              // faulted / absent channel
            s_state.ch_ok[i]  = false;
        } else {
            s_state.temp_c[i] = raw / 10.0f;
            s_state.ch_ok[i]  = true;
        }
    }
    for (int i = 0; i < count; i++) {
        const uint8_t *e = m + MFG_FIXED_LEN + i * PROBE_ENTRY_LEN;
        uint16_t raw = (uint16_t)e[1] | ((uint16_t)e[2] << 8);
        s_state.probe_id[i] = e[0];
        s_state.probe_c[i]  = (raw == 0xFFFF) ? NAN : raw / 10.0f;
    }
    s_state.probe_count = count;
    bool     first = !s_state.ever;
    uint32_t now   = ms_now();
    s_state.last_ms = now;
    s_state.ever    = true;

    float t[BBQ_BLE_CHANNELS]; bool ok[BBQ_BLE_CHANNELS];
    memcpy(t,  s_state.temp_c, sizeof(t));
    memcpy(ok, s_state.ch_ok,  sizeof(ok));
    xSemaphoreGive(s_mutex);

    // First contact, then a throttled heartbeat so the serial console can
    // confirm reception without spamming (box advertises ~10 Hz).
    static uint32_t s_last_log;
    if (first || now - s_last_log > 5000) {
        s_last_log = now;
        LOGI(TAG, "%sTC0=%s%.1f TC1=%s%.1f TC2=%s%.1f TC3=%s%.1f  probes=%d",
                 first ? "box found — " : "",
                 ok[0] ? "" : "x", t[0], ok[1] ? "" : "x", t[1],
                 ok[2] ? "" : "x", t[2], ok[3] ? "" : "x", t[3], count);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC)
        handle_adv(event->disc.data, event->disc.length_data);
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {0};
    p.passive        = 1;              // don't send scan requests
    p.itvl           = 0;             // controller default
    p.window         = 0;
    p.filter_duplicates = 0;          // we want every refresh
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) LOGE(TAG, "ble_gap_disc rc=%d", rc);
    else         LOGI(TAG, "passive scan started");
}

static void on_sync(void) { start_scan(); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void bbq_ble_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < BBQ_BLE_CHANNELS; i++) s_state.ch_ok[i] = false;

    // We only use BLE — hand the Classic-BT controller memory back to the heap.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    LOGI(TAG, "receiver started (observer)");
}

uint32_t bbq_ble_age_ms(void)
{
    if (!s_state.ever) return UINT32_MAX;
    return ms_now() - s_state.last_ms;
}

bool bbq_ble_present(void)
{
    return bbq_ble_age_ms() <= FRESH_WINDOW_MS;
}

bool bbq_ble_channel(int ch, float *temp_c)
{
    if (ch < 0 || ch >= BBQ_BLE_CHANNELS) return false;
    if (!bbq_ble_present()) return false;

    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_state.ch_ok[ch]) {
            if (temp_c) *temp_c = s_state.temp_c[ch];
            ok = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

int bbq_ble_probe_count(void)
{
    if (!bbq_ble_present()) return 0;

    int n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        n = s_state.probe_count;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

bool bbq_ble_probe(int slot, uint8_t *id, float *temp_c)
{
    if (slot < 0 || slot >= BBQ_BLE_MAX_PROBES) return false;
    if (!bbq_ble_present()) return false;

    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (slot < s_state.probe_count) {
            if (id)     *id     = s_state.probe_id[slot];
            if (temp_c) *temp_c = s_state.probe_c[slot];
            ok = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

// Give WiFi the radio to itself for the duration of an OTA download — BLE
// scanning is continuous-duty-cycle radio activity that otherwise throttles
// WiFi throughput to a crawl (same physical radio on ESP32-S3).
void bbq_ble_scan_pause(void)  { ble_gap_disc_cancel(); }
void bbq_ble_scan_resume(void) { start_scan(); }
