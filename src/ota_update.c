#include "ota_update.h"
#include "app_config.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota";

#define CHECK_TIMEOUT_MS   8000
#define OTA_TIMEOUT_MS     15000
#define VERSION_BUF_SZ     256

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

    static char buf[VERSION_BUF_SZ];
    if (fetch_url(OTA_VERSION_URL, buf, sizeof(buf), CHECK_TIMEOUT_MS) != 200) {
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
    xTaskCreate(check_task, "ota_check", 6144, NULL, 3, NULL);
}

// ---- Update -------------------------------------------------------------

static void update_task(void *arg)
{
    (void)arg;
    ota_status_t s = { .state = OTA_UPDATING };
    set_status(&s);

    esp_http_client_config_t http_cfg = {
        .url                = OTA_FIRMWARE_URL,
        .timeout_ms         = OTA_TIMEOUT_MS,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .keep_alive_enable  = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        s.state = OTA_DONE_FAIL;
        snprintf(s.error, sizeof(s.error), "Could not start download");
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    s.image_size = esp_https_ota_get_image_size(handle);
    do {
        err = esp_https_ota_perform(handle);
        s.bytes_read = esp_https_ota_get_image_len_read(handle);
        set_status(&s);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        s.state = OTA_DONE_FAIL;
        snprintf(s.error, sizeof(s.error), "Download incomplete");
        set_status(&s);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t fin = esp_https_ota_finish(handle);
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
    xTaskCreate(update_task, "ota_update", 8192, NULL, 3, NULL);
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
