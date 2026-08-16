#include "ota_update.h"
#include "app_config.h"
#include "globals.h"
#include "ui_network_guard.h"
#include "bbq_controller.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota";

static inline uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

#define CHECK_TIMEOUT_MS   12000
#define OTA_TIMEOUT_MS     30000    // link to GitHub Pages can be slow/laggy from some networks
#define OTA_BUF_SIZE       4096     // larger reads = fewer round trips over a high-latency link
#define OTA_BEGIN_RETRIES  3
#define OTA_RETRY_DELAY_MS 2000
#define OTA_LOG_INTERVAL_MS 3000    // heartbeat so a slow-but-progressing download is visible in logs
#define OTA_STALL_TIMEOUT_MS 20000  // give up if no forward progress for this long
#define VERSION_BUF_SZ     256

#define CHECK_STACK_WORDS  6144
#define UPDATE_STACK_WORDS 8192

// check_task's stack lives in PSRAM (not internal DRAM): by the time the
// user opens the OTA page, WiFi + Sonos + BLE are already running and
// internal DRAM is tight, and a plain xTaskCreate() can fail with no error
// surfaced. Static allocation also sidesteps runtime heap fragmentation —
// the space is reserved at link time, not carved out of a live, fragmented
// heap. Safe here: it only does an HTTPS GET, never touches flash.
static EXT_RAM_BSS_ATTR StackType_t s_check_stack[CHECK_STACK_WORDS];
static StaticTask_t                 s_check_tcb;

// update_task's stack MUST stay in internal DRAM, unlike check_task's: the
// OTA write path (esp_ota_begin -> esp_partition_mmap -> spi_flash_mmap)
// briefly disables the flash cache, and ESP-IDF asserts that the currently
// running task's own stack isn't in PSRAM at that point (PSRAM access goes
// through the same bus/cache being disabled) — crashes with
// "esp_task_stack_is_sane_cache_disabled()" otherwise.
//
// Static (not dynamic xTaskCreate): tried dynamic first to avoid a permanent
// reservation, but it failed outright once the larger LWIP_TCP_WND_DEFAULT
// (see sdkconfig.defaults) left too little contiguous internal heap at the
// exact moment "Update Now" is tapped — confirmed by "xTaskCreate(update)
// failed" in the field. A static buffer is reserved once at link time, so it
// no longer competes for a live, possibly-fragmented heap right when it's
// needed most. The 8KB permanent cost is paid for by NOT also cutting the
// LVGL pool to compensate — our usage-% logs undercount the true worst case
// (screens never visited in a given session don't show up), so that's not
// a safe lever to guess at. Watch "DRAM free before tasks" after this
// change; if Sonos's poll_task/cmd_task ever fail to spawn because of it,
// that's the real, measured signal to revisit — not a guess.
static StackType_t  s_update_stack[UPDATE_STACK_WORDS];
static StaticTask_t s_update_tcb;

static SemaphoreHandle_t s_mutex;
static ota_status_t      s_status;

// Every public entry point calls this before touching s_mutex — cheap once
// created, and avoids relying on init-order across callers (the same NULL-
// mutex crash class hit earlier in ble_probe.c).
static void ensure_mutex(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

static void set_status(const ota_status_t *s)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_status = *s;
        xSemaphoreGive(s_mutex);
    }
}

void ota_get_status(ota_status_t *out)
{
    if (!out) return;
    ensure_mutex();
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_mutex);
    }
}

// ---- version.json GET — same event-handler-into-buffer pattern as weather.c ----

typedef struct { char *buf; int max_len; int total; } fetch_ctx_t;

static esp_err_t fetch_evt(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx->total < ctx->max_len) {
        int space = ctx->max_len - ctx->total;
        int copy  = evt->data_len < space ? evt->data_len : space;
        memcpy(ctx->buf + ctx->total, evt->data, copy);
        ctx->total += copy;
    }
    return ESP_OK;
}

static int fetch_url(const char *url, char *buf, int max_len, int timeout_ms)
{
    fetch_ctx_t ctx = { .buf = buf, .max_len = max_len - 1, .total = 0 };
    esp_http_client_config_t cfg = {
        .url                = url,
        .timeout_ms         = timeout_ms,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .event_handler      = fetch_evt,
        .user_data          = &ctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_err_t err = esp_http_client_perform(c);
    buf[ctx.total] = '\0';
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "check fetch err=%d status=%d", err, status);
        return -1;
    }
    return status;
}

// ---- Check ------------------------------------------------------------

static void check_task(void *arg)
{
    (void)arg;
    ota_status_t s = { .state = OTA_CHECKING };
    set_status(&s);

    // Coordinate with the rest of the app's HTTP traffic (Sonos/weather) —
    // without this, an OTA check landing mid-BLE-scan or racing another
    // request is exactly the kind of WiFi/BLE radio contention that's bitten
    // this project before (see net_pre_wait / g_network_mutex elsewhere).
    net_pre_wait("OTA", NET_WAIT_GENERAL);
    static char buf[VERSION_BUF_SZ];
    int status = -1;
    if (xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        status = fetch_url(OTA_VERSION_URL, buf, sizeof(buf), CHECK_TIMEOUT_MS);
        g_last_network_end_ms = ms_now();
        xSemaphoreGive(g_network_mutex);
    }
    if (status != 200) {
        s.state = OTA_CHECK_FAILED;
        snprintf(s.error, sizeof(s.error), "Could not reach update server");
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    cJSON *ver  = root ? cJSON_GetObjectItem(root, "version") : NULL;
    if (!cJSON_IsString(ver)) {
        s.state = OTA_CHECK_FAILED;
        snprintf(s.error, sizeof(s.error), "Bad response from update server");
        if (root) cJSON_Delete(root);
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    strlcpy(s.latest_version, ver->valuestring, sizeof(s.latest_version));
    s.state = (strcmp(s.latest_version, FIRMWARE_VERSION) == 0) ? OTA_UP_TO_DATE
                                                                 : OTA_UPDATE_AVAILABLE;
    cJSON_Delete(root);
    set_status(&s);
    ESP_LOGI(TAG, "check: running=%s latest=%s", FIRMWARE_VERSION, s.latest_version);
    vTaskDelete(NULL);
}

void ota_check_async(void)
{
    ensure_mutex();
    TaskHandle_t h = xTaskCreateStatic(check_task, "ota_check", CHECK_STACK_WORDS,
                                       NULL, 3, s_check_stack, &s_check_tcb);
    if (!h) {
        ota_status_t s = { .state = OTA_CHECK_FAILED };
        snprintf(s.error, sizeof(s.error), "Out of memory — try again");
        set_status(&s);
        ESP_LOGE(TAG, "xTaskCreateStatic(check) failed");
    }
}

// ---- Update (resumable via HTTP Range) -----------------------------------
// esp_https_ota()'s high-level wrapper always erases the partition and
// restarts from byte 0. On this network (observed ~2.5KB/s with occasional
// drops) that turns a late failure into a ~15-minute redo. So we drive
// esp_ota_begin/write/end directly: on a retriable failure the OTA handle is
// left open and the bytes already written are kept, so the next tap of
// Update Now sends "Range: bytes=<written>-" and continues instead of
// restarting — as long as the device hasn't rebooted in between.
static esp_ota_handle_t       s_ota_handle;
static const esp_partition_t *s_ota_partition;
static bool                   s_ota_open;      // esp_ota_begin() succeeded, not yet ended
static int                    s_ota_written;    // bytes written into s_ota_handle so far
static int                    s_ota_total;      // expected image size once known (0 = unknown)

#define OTA_READ_BUF_SZ 4096
static EXT_RAM_BSS_ATTR char s_ota_read_buf[OTA_READ_BUF_SZ];   // network staging buffer, PSRAM is fine (never touched with cache disabled)

static void update_task(void *arg)
{
    (void)arg;
    ota_status_t s = { .state = OTA_UPDATING, .bytes_read = s_ota_written, .image_size = s_ota_total };
    set_status(&s);

    // Hold the shared network mutex for the whole download: letting
    // Sonos/weather contend for the radio mid-download risks exactly the
    // "Failed to open new connection" seen during the version check.
    net_pre_wait("OTA", NET_WAIT_GENERAL);
    if (xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        s.state = OTA_DONE_FAIL;
        snprintf(s.error, sizeof(s.error), "Network busy — try again");
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    // BLE (scanning + any live connections) shares the same radio as WiFi —
    // left running, it throttles the download badly. Paused for the
    // download, resumed on every exit path below.
    bbq_radio_pause_for_ota(true);

    if (!s_ota_open) {
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition || esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle) != ESP_OK) {
            g_last_network_end_ms = ms_now();
            xSemaphoreGive(g_network_mutex);
            bbq_radio_pause_for_ota(false);
            s.state = OTA_DONE_FAIL;
            snprintf(s.error, sizeof(s.error), "Could not start download");
            set_status(&s);
            vTaskDelete(NULL);
            return;
        }
        s_ota_open = true;
        s_ota_written = 0;
        s_ota_total   = 0;
    }

    char range_hdr[32] = {0};
    if (s_ota_written > 0)
        snprintf(range_hdr, sizeof(range_hdr), "bytes=%d-", s_ota_written);

    esp_http_client_config_t http_cfg = {
        .url                = OTA_FIRMWARE_URL,
        .timeout_ms         = OTA_TIMEOUT_MS,
        .buffer_size        = OTA_BUF_SIZE,
        .buffer_size_tx     = OTA_BUF_SIZE,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .keep_alive_enable  = true,
    };

    // The initial connect (DNS + TCP + TLS handshake) is where we've seen
    // transient "select() timeout" failures — retry a few times before
    // giving up rather than making the user re-tap Update Now each time.
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 1; attempt <= OTA_BEGIN_RETRIES; attempt++) {
        client = esp_http_client_init(&http_cfg);
        if (!client) { err = ESP_FAIL; break; }
        if (range_hdr[0]) esp_http_client_set_header(client, "Range", range_hdr);
        err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (status == 200 || status == 206) break;
            ESP_LOGW(TAG, "unexpected HTTP status %d", status);
            err = ESP_FAIL;
        }
        ESP_LOGW(TAG, "connect attempt %d/%d failed: %s",
                 attempt, OTA_BEGIN_RETRIES, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        client = NULL;
        if (attempt < OTA_BEGIN_RETRIES) vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));
    }
    if (!client || err != ESP_OK) {
        g_last_network_end_ms = ms_now();
        xSemaphoreGive(g_network_mutex);
        bbq_radio_pause_for_ota(false);
        s.state = OTA_DONE_FAIL;
        snprintf(s.error, sizeof(s.error), "Could not start download — progress kept, try again");
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    // Server ignored our Range header (sent 200 instead of 206) — it's
    // sending the whole file from byte 0 again. Restart clean rather than
    // risk corrupting the partition by writing over our partial progress.
    if (s_ota_written > 0 && status == 200) {
        ESP_LOGW(TAG, "server ignored Range — restarting from scratch");
        esp_ota_abort(s_ota_handle);
        esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
        s_ota_written = 0;
    }

    int content_len = esp_http_client_get_content_length(client);
    if (content_len > 0) s_ota_total = s_ota_written + content_len;
    s.image_size = s_ota_total;

    uint32_t last_log_ms      = ms_now();
    uint32_t last_progress_ms = ms_now();
    int      last_bytes       = s_ota_written;
    bool     stalled = false, failed = false;
    while (1) {
        int n = esp_http_client_read(client, s_ota_read_buf, sizeof(s_ota_read_buf));
        if (n < 0) { failed = true; break; }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
        } else if (esp_ota_write(s_ota_handle, s_ota_read_buf, n) != ESP_OK) {
            failed = true;
            break;
        } else {
            s_ota_written += n;
        }
        s.bytes_read = s_ota_written;
        set_status(&s);

        uint32_t now = ms_now();
        if (s_ota_written > last_bytes) {
            last_bytes = s_ota_written;
            last_progress_ms = now;
        } else if (now - last_progress_ms >= OTA_STALL_TIMEOUT_MS) {
            ESP_LOGW(TAG, "update: stalled at %d bytes, giving up", s_ota_written);
            stalled = true;
            break;
        }
        if (now - last_log_ms >= OTA_LOG_INTERVAL_MS) {
            last_log_ms = now;
            ESP_LOGI(TAG, "update: %d / %d bytes", s.bytes_read, s.image_size);
        }
    }
    esp_http_client_cleanup(client);
    g_last_network_end_ms = ms_now();
    xSemaphoreGive(g_network_mutex);
    bbq_radio_pause_for_ota(false);

    if (stalled || failed) {
        // s_ota_open / s_ota_written deliberately left as-is: next Update Now
        // resumes from here via Range instead of starting over.
        s.state = OTA_DONE_FAIL;
        if (s_ota_total > 0)
            snprintf(s.error, sizeof(s.error), "%s at %d%% — progress kept, tap to resume",
                     stalled ? "Stalled" : "Failed", (int)((int64_t)s_ota_written * 100 / s_ota_total));
        else
            snprintf(s.error, sizeof(s.error), "%s — progress kept, tap to resume",
                     stalled ? "Stalled" : "Failed");
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t fin = esp_ota_end(s_ota_handle);
    s_ota_open = false;   // whole handle lifecycle is done either way past this point
    if (fin == ESP_OK) fin = esp_ota_set_boot_partition(s_ota_partition);
    if (fin != ESP_OK) {
        s.state = OTA_DONE_FAIL;
        snprintf(s.error, sizeof(s.error), "Write failed (%s)", esp_err_to_name(fin));
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    // Written to the inactive OTA slot and marked bootable. The caller
    // (Settings > OTA page) notices OTA_DONE_OK and drives the reboot
    // countdown screen — we don't esp_restart() here ourselves.
    s.state = OTA_DONE_OK;
    set_status(&s);
    ESP_LOGI(TAG, "update: wrote %d bytes, ready to reboot", s.bytes_read);
    vTaskDelete(NULL);
}

void ota_start_async(void)
{
    ensure_mutex();
    TaskHandle_t h = xTaskCreateStatic(update_task, "ota_update", UPDATE_STACK_WORDS,
                                       NULL, 3, s_update_stack, &s_update_tcb);
    if (!h) {
        ota_status_t s = { .state = OTA_DONE_FAIL };
        snprintf(s.error, sizeof(s.error), "Out of memory — try again");
        set_status(&s);
        ESP_LOGE(TAG, "xTaskCreateStatic(update) failed");
    }
}

void ota_mark_app_valid(void)
{
    ensure_mutex();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image marked valid, rollback cancelled");
    }
}
