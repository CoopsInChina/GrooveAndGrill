#ifdef SPEEDO_SIM

#include "sim.h"
#include "calibration.h"
#include "display.h"
#include "speedo_ui.h"
#include "splash.h"
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "sim";

#define UPDATE_MS      50       // UI update interval — 20 Hz
#define COMFORT_DELAY  5000     // ms screen-off before ComfortEnable fires
#define START_DELAY    5000     // ms screen-on at 0 mph before sweep starts
#define ACCEL_MS       15000    // 0 → 200 mph ramp duration
#define HOLD_MS        5000     // dwell at peak speed
#define DECEL_MS       15000    // 200 → 0 mph ramp duration
#define PEAK_MPH       200.0f

static void on_splash_done(void)
{
    speedo_ui_set_visible(true);
}

static void sim_task(void *arg)
{
    int steps;

    while (1) {
        // ---- Simulate ComfortEnable OFF — screen dark ----
        speedo_ui_set_speed(0.0f);
        display_set_backlight(false);
        ESP_LOGI(TAG, "ComfortEnable OFF");
        vTaskDelay(pdMS_TO_TICKS(COMFORT_DELAY));

        // ---- Simulate ComfortEnable ON — screen lights up, show splash ----
        display_set_backlight(true);
        speedo_ui_set_speed(0.0f);
        ESP_LOGI(TAG, "ComfortEnable ON — showing splash");
        if (display_lock(100)) {
            speedo_ui_set_visible(false);
            splash_show(on_splash_done);
            display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(START_DELAY));

        // ---- Accelerate 0 → 200 mph ----
        ESP_LOGI(TAG, "Accelerating 0 -> %.0f mph over %d s", PEAK_MPH, ACCEL_MS / 1000);
        steps = ACCEL_MS / UPDATE_MS;
        for (int i = 0; i <= steps; i++) {
            speedo_ui_set_speed(cal_apply(PEAK_MPH * ((float)i / steps)));
            vTaskDelay(pdMS_TO_TICKS(UPDATE_MS));
        }

        // ---- Hold at peak ----
        ESP_LOGI(TAG, "Holding at %.0f mph", PEAK_MPH);
        speedo_ui_set_speed(cal_apply(PEAK_MPH));
        vTaskDelay(pdMS_TO_TICKS(HOLD_MS));

        // ---- Decelerate 200 → 0 mph ----
        ESP_LOGI(TAG, "Decelerating");
        steps = DECEL_MS / UPDATE_MS;
        for (int i = 0; i <= steps; i++) {
            speedo_ui_set_speed(cal_apply(PEAK_MPH * (1.0f - (float)i / steps)));
            vTaskDelay(pdMS_TO_TICKS(UPDATE_MS));
        }
        speedo_ui_set_speed(0.0f);
        // ComfortEnable OFF phase handled at top of next loop iteration
    }
}

void sim_start(void)
{
    xTaskCreate(sim_task, "sim", 3072, NULL, 5, NULL);
}

#endif // SPEEDO_SIM
