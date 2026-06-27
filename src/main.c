#include "tca9554.h"
#include "display.h"
#include "can_bus.h"
#include "speedo_ui.h"
#include "calibration.h"
#include "wifi_cal.h"
#ifdef SPEEDO_SIM
#include "sim.h"
#endif

#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// ---- CAN callbacks (called from can_rx_task on CPU1) ----------------

#ifndef SPEEDO_SIM
static void on_speed_update(float raw_mph)
{
    speedo_ui_set_speed(cal_apply(raw_mph));
}

static void on_comfort_enable(bool enabled)
{
    ESP_LOGI(TAG, "ComfortEnable -> %s", enabled ? "ON" : "OFF");
    display_set_backlight(enabled);
}
#endif

// ---- App entry point ------------------------------------------------

void app_main(void)
{
    // NVS — required by calibration, WiFi, and OTA
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load persisted calibration offset before the first CAN frame arrives
    ESP_ERROR_CHECK(cal_init());

    // I2C + TCA9554PWR expander (must come before display_init)
    ESP_ERROR_CHECK(tca9554_init());

    // Display (ST7701S SPI init, RGB panel, LVGL — backlight off until ComfortEnable)
    ESP_ERROR_CHECK(display_init());

    // Build LVGL speedometer screen
    if (display_lock(5000)) {
        speedo_ui_create();
        display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for UI creation");
    }

    // WiFi AP + calibration/OTA HTTP server (always running in background)
    ESP_ERROR_CHECK(wifi_cal_server_start());

#ifdef SPEEDO_SIM
    ESP_LOGW(TAG, "*** SIMULATION MODE — CAN bus not initialised ***");
    sim_start();
#else
    // CAN bus — starts listening for speed and ComfortEnable frames
    ESP_ERROR_CHECK(can_bus_init(on_speed_update, on_comfort_enable));
    ESP_LOGI(TAG, "Speedometer running  |  Cal/OTA: http://192.168.4.1");
#endif
}
