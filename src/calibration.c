#include "calibration.h"
#include "app_config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG      = "cal";
static const char *NVS_NS   = "speedo";
static const char *NVS_KEY  = "cal_pct";

static float s_offset_pct = 0.0f;

esp_err_t cal_init(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved calibration — using 0%%");
        return ESP_OK;
    }
    ESP_ERROR_CHECK(ret);

    // Stored as int32: pct * 100  (0.01% resolution)
    int32_t raw = 0;
    ret = nvs_get_i32(h, NVS_KEY, &raw);
    nvs_close(h);

    if (ret == ESP_OK) {
        s_offset_pct = (float)raw / 100.0f;
        ESP_LOGI(TAG, "Loaded calibration: %.2f%%", s_offset_pct);
    }
    return ESP_OK;
}

float cal_get_offset_pct(void)
{
    return s_offset_pct;
}

esp_err_t cal_set_offset_pct(float pct)
{
    if (pct >  CAL_MAX_OFFSET_PCT) pct =  CAL_MAX_OFFSET_PCT;
    if (pct < -CAL_MAX_OFFSET_PCT) pct = -CAL_MAX_OFFSET_PCT;

    s_offset_pct = pct;

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    esp_err_t ret = nvs_set_i32(h, NVS_KEY, (int32_t)(pct * 100.0f));
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "Calibration set to %.2f%% (saved)", s_offset_pct);
    return ret;
}

float cal_apply(float raw_mph)
{
    return raw_mph * (1.0f + s_offset_pct / 100.0f);
}
