#include "weather.h"
#include "globals.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "weather";

#define BUF_SIZE        (48 * 1024)             // wttr.in j1 includes all language translations; Shanghai ~35 KB
#define REFRESH_MS      (30 * 60 * 1000)
#define LOC_REFRESH_MS  (24 * 60 * 60 * 1000)
#define LOC_RETRY_MS    (60 * 1000)             // retry location every 60 s while not yet valid
#define TZ_TIMEOUT_MS   10000                   // 10 s — gives slow networks time to respond

static weather_data_t    s_data;
static SemaphoreHandle_t s_mutex;

// ---- HTTP helper -------------------------------------------------------

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

static int fetch_url(const char *url, char *buf, int max_len, int timeout_ms, bool tls)
{
    fetch_ctx_t ctx = { .buf = buf, .max_len = max_len - 1, .total = 0 };
    esp_http_client_config_t cfg = {
        .url                         = url,
        .timeout_ms                  = timeout_ms,
        .buffer_size                 = 2048,
        .max_redirection_count       = 3,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach           = tls ? esp_crt_bundle_attach : NULL,
        .event_handler               = fetch_evt,
        .user_data                   = &ctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_err_t err = esp_http_client_perform(c);
    buf[ctx.total] = '\0';
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "fetch %s err=%d status=%d", url, err, status);
        return -1;
    }
    return ctx.total;
}

// ---- ip-api.com: lat/lon/city/TZ offset --------------------------------

static bool fetch_location(char *buf, int buflen,
                             float *out_lat, float *out_lon,
                             char *out_city, int city_sz,
                             int *out_offset_sec)
{
    int len = fetch_url("http://ip-api.com/json?fields=lat,lon,city,offset",
                        buf, buflen, TZ_TIMEOUT_MS, false);
    if (len <= 0) { ESP_LOGW(TAG, "ip-api failed"); return false; }

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) { ESP_LOGW(TAG, "ip-api JSON parse failed"); return false; }

    bool ok = false;
    cJSON *jlat = cJSON_GetObjectItem(root, "lat");
    cJSON *jlon = cJSON_GetObjectItem(root, "lon");
    cJSON *jcit = cJSON_GetObjectItem(root, "city");
    cJSON *joff = cJSON_GetObjectItem(root, "offset");

    if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
        *out_lat = (float)jlat->valuedouble;
        *out_lon = (float)jlon->valuedouble;
        ok = true;
    }
    if (cJSON_IsString(jcit) && jcit->valuestring[0])
        strlcpy(out_city, jcit->valuestring, city_sz);
    if (cJSON_IsNumber(joff) && out_offset_sec)
        *out_offset_sec = (int)joff->valuedouble;

    cJSON_Delete(root);
    return ok;
}

// ---- wttr.in: current conditions + 3-hour hourly slots ----------------
// URL: http://wttr.in/~{lat},{lon}?format=j1&lang=en  (HTTP — no TLS)
// wttr.in weather codes: 113 (sunny) … 395 (heavy snow/thunder)

static void fetch_wttr(char *buf, int buflen, float lat, float lon)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://wttr.in/~%.2f,%.2f?format=j1&days=2&lang=en", lat, lon);

    int len = fetch_url(url, buf, buflen, 15000, false);
    if (len <= 0) { ESP_LOGW(TAG, "wttr.in failed"); return; }

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) { ESP_LOGW(TAG, "wttr.in JSON parse failed (len=%d)", len); return; }

    // ---- Current conditions ----
    int  temp_c = 0, humidity = 0, wmo = 0, wind_kmh = 0;
    char condition[32] = {0};
    bool cur_ok = false;

    cJSON *cc = cJSON_GetObjectItem(root, "current_condition");
    if (cJSON_IsArray(cc)) {
        cJSON *c0 = cJSON_GetArrayItem(cc, 0);
        if (c0) {
            cJSON *jt  = cJSON_GetObjectItem(c0, "temp_C");
            cJSON *jh  = cJSON_GetObjectItem(c0, "humidity");
            cJSON *jwc = cJSON_GetObjectItem(c0, "weatherCode");
            cJSON *jws = cJSON_GetObjectItem(c0, "windspeedKmph");
            cJSON *jwd = cJSON_GetObjectItem(c0, "weatherDesc");

            // wttr.in returns all numeric fields as strings
            if (cJSON_IsString(jt))  { temp_c   = atoi(jt->valuestring);  cur_ok = true; }
            if (cJSON_IsString(jh))    humidity  = atoi(jh->valuestring);
            if (cJSON_IsString(jwc))   wmo       = atoi(jwc->valuestring);
            if (cJSON_IsString(jws))   wind_kmh  = atoi(jws->valuestring);

            if (cJSON_IsArray(jwd)) {
                cJSON *d0 = cJSON_GetArrayItem(jwd, 0);
                if (d0) {
                    cJSON *v = cJSON_GetObjectItem(d0, "value");
                    if (cJSON_IsString(v))
                        strlcpy(condition, v->valuestring, sizeof(condition));
                }
            }
        }
    }

    // ---- Hourly forecast (3-hour slots, pick next 3 future slots) ----
    weather_hour_t hourly[6] = {0};
    int filled = 0;

    time_t now_t = time(NULL);
    struct tm *ti = localtime(&now_t);
    int cur_hour = ti ? ti->tm_hour : 0;

    cJSON *weather_arr = cJSON_GetObjectItem(root, "weather");
    if (cJSON_IsArray(weather_arr)) {
        // Scan today (day 0) then tomorrow (day 1) to find 3 future slots
        for (int day = 0; day < 2 && filled < 3; day++) {
            cJSON *wd = cJSON_GetArrayItem(weather_arr, day);
            if (!wd) continue;
            cJSON *hourly_arr = cJSON_GetObjectItem(wd, "hourly");
            if (!cJSON_IsArray(hourly_arr)) continue;

            int sz = cJSON_GetArraySize(hourly_arr);
            for (int i = 0; i < sz && filled < 3; i++) {
                cJSON *slot = cJSON_GetArrayItem(hourly_arr, i);
                if (!slot) continue;

                cJSON *st  = cJSON_GetObjectItem(slot, "time");       // "0","300"…"2100"
                cJSON *jtc = cJSON_GetObjectItem(slot, "tempC");
                cJSON *jwc = cJSON_GetObjectItem(slot, "weatherCode");

                if (!cJSON_IsString(st)) continue;
                int slot_hour = atoi(st->valuestring) / 100;

                // Today: skip slots already passed; tomorrow: take all
                if (day == 0 && slot_hour <= cur_hour) continue;

                hourly[filled].hour = slot_hour;
                hourly[filled].temp = cJSON_IsString(jtc) ? atoi(jtc->valuestring) : 0;
                hourly[filled].wmo  = cJSON_IsString(jwc) ? atoi(jwc->valuestring) : 113;
                filled++;
            }
        }
    }

    cJSON_Delete(root);

    if (!cur_ok && filled == 0) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (cur_ok) {
            s_data.temp_c   = temp_c;
            s_data.humidity = humidity;
            s_data.wmo      = wmo;
            s_data.wind_kmh = wind_kmh;
            s_data.valid    = true;
            strlcpy(s_data.condition,
                    condition[0] ? condition : "---",
                    sizeof(s_data.condition));
        }
        if (filled > 0) {
            memcpy(s_data.hourly, hourly, filled * sizeof(weather_hour_t));
            s_data.hourly_valid = true;
        }
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Weather: %d°C code=%d hum=%d%% wind=%dkm/h hourly=%d [%s]",
             temp_c, wmo, humidity, wind_kmh, filled, condition);
}

// ---- Task --------------------------------------------------------------

static void weather_task(void *arg)
{
    char *buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "OOM"); vTaskDelete(NULL); return; }

    // Wait for NTP sync (up to 30 s)
    int wait = 0;
    while (!g_ntp_synced && wait < 30) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) g_ntp_synced = true;
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait++;
    }

    float lat = 0.0f, lon = 0.0f;
    bool  loc_valid = false;
    uint32_t loc_last_ms = 0;

    while (1) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        // Re-fetch location/TZ once per day (or on first run)
        if (!loc_valid || (now_ms - loc_last_ms) >= (uint32_t)LOC_REFRESH_MS) {
            if (xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                char city[64] = {0};
                int  offset_sec = 0;
                bool ok = fetch_location(buf, BUF_SIZE, &lat, &lon, city, sizeof(city), &offset_sec);
                xSemaphoreGive(g_network_mutex);

                if (ok) {
                    loc_valid   = true;
                    loc_last_ms = now_ms;

                    // Apply timezone
                    int h = offset_sec / 3600;
                    int m = abs((offset_sec % 3600) / 60);
                    char tz[32];
                    if (m == 0) snprintf(tz, sizeof(tz), "UTC%+d", -h);
                    else        snprintf(tz, sizeof(tz), "UTC%+d:%02d", -h, m);
                    setenv("TZ", tz, 1);
                    tzset();
                    ESP_LOGI(TAG, "Location: %s (%.2f,%.2f) TZ=%s", city, lat, lon, tz);

                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        strlcpy(s_data.location, city, sizeof(s_data.location));
                        strlcpy(s_data.tz_posix, tz,   sizeof(s_data.tz_posix));
                        s_data.tz_valid = true;
                        xSemaphoreGive(s_mutex);
                    }
                }
            }
        }

        // Fetch weather if we have coordinates
        if (loc_valid) {
            if (xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                fetch_wttr(buf, BUF_SIZE, lat, lon);
                xSemaphoreGive(g_network_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
        } else {
            // Location not yet known — retry soon instead of waiting 30 minutes.
            vTaskDelay(pdMS_TO_TICKS(LOC_RETRY_MS));
        }
    }

    free(buf);
}

// ---- Public API --------------------------------------------------------

void weather_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_data, 0, sizeof(s_data));
    xTaskCreatePinnedToCore(weather_task, "weather", 6144, NULL, 2, NULL, 0);
}

void weather_get(weather_data_t *out)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_data;
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}
