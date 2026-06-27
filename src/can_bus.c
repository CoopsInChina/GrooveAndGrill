#include "can_bus.h"
#include "app_config.h"
#include "board_config.h"

#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "can_bus";

static speed_update_cb_t   s_speed_cb   = NULL;
static comfort_enable_cb_t s_comfort_cb = NULL;
static bool                s_comfort_prev = false;

// Build the timing config at compile time from CAN_BITRATE_BPS.
static twai_timing_config_t get_timing(void)
{
#if   CAN_BITRATE_BPS == 1000000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
#elif CAN_BITRATE_BPS == 500000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
#elif CAN_BITRATE_BPS == 250000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
#elif CAN_BITRATE_BPS == 125000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
#else
#error "CAN_BITRATE_BPS must be one of: 125000 / 250000 / 500000 / 1000000"
#endif
}

static float parse_speed(const twai_message_t *msg)
{
    uint32_t raw;
    if (SPEED_BYTE_COUNT >= 2) {
        raw = ((uint32_t)msg->data[SPEED_BYTE_OFFSET] << 8) |
               (uint32_t)msg->data[SPEED_BYTE_OFFSET + 1];
    } else {
        raw = msg->data[SPEED_BYTE_OFFSET];
    }
    float kmh = (float)raw * SPEED_SCALE + SPEED_OFFSET;
    return kmh * KMH_TO_MPH;
}

static bool parse_comfort(const twai_message_t *msg)
{
    return (msg->data[COMFORT_BYTE_OFFSET] & COMFORT_BIT_MASK) != 0;
}

static void can_rx_task(void *arg)
{
    twai_message_t msg;
    while (1) {
        if (twai_receive(&msg, pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }

        uint32_t id = msg.identifier;

        if (id == SPEED_CAN_ID && s_speed_cb) {
            s_speed_cb(parse_speed(&msg));
        }

        if (id == COMFORT_CAN_ID && s_comfort_cb) {
            bool now = parse_comfort(&msg);
            if (now != s_comfort_prev) {
                s_comfort_prev = now;
                s_comfort_cb(now);
            }
        }
    }
}

esp_err_t can_bus_init(speed_update_cb_t speed_cb, comfort_enable_cb_t comfort_cb)
{
    s_speed_cb   = speed_cb;
    s_comfort_cb = comfort_cb;

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO,
                                                           TWAI_MODE_LISTEN_ONLY);
    twai_timing_config_t  t = get_timing();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g, &t, &f);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "twai_start: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
        return ret;
    }

    xTaskCreatePinnedToCore(can_rx_task, "can_rx", 4096, NULL, 10, NULL, 1);
    ESP_LOGI(TAG, "TWAI listen-only @ %d bps", CAN_BITRATE_BPS);
    return ESP_OK;
}
