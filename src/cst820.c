#include "cst820.h"
#include "board_config.h"
#include "globals.h"
#include "tca9554.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "cst820";

static lv_coord_t s_x = 0, s_y = 0;
static bool       s_pressed     = false;
static uint32_t   s_log_counter = 0;
static int        s_nack_count  = 0;

static esp_err_t cst820_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(IO_I2C_PORT, TP_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(10));
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint8_t reg = 0x00;
    uint8_t buf[7] = {0};
    esp_err_t ret = i2c_master_write_read_device(
        IO_I2C_PORT, TP_I2C_ADDR,
        &reg, 1, buf, sizeof(buf),
        pdMS_TO_TICKS(10));

    s_log_counter++;
    if (s_log_counter % 200 == 1) {
        ESP_LOGI(TAG, "poll ret=%d buf=[%02x %02x %02x %02x %02x %02x %02x]",
                 ret, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
    }

    if (ret != ESP_OK) {
        s_nack_count++;
        // Every 100 consecutive NACKs (~3 s), re-assert no-auto-sleep. If the
        // chip glitch-reset and re-enabled sleep, this restores it once it's
        // responding again (a persistent NACK means it's still asleep and the
        // write itself NACKs — harmless). No task/vTaskDelay from this context.
        if (s_nack_count % 100 == 0) {
            esp_err_t wr = cst820_write_reg(0xFE, 0x01);
            ESP_LOGW(TAG, "CST820 NACK×%d — re-asserted no-sleep: %s",
                     s_nack_count, wr == ESP_OK ? "OK" : esp_err_to_name(wr));
        }
        s_pressed = false;
    } else {
        s_nack_count = 0;
        if ((buf[2] & 0x0F) > 0) {
            s_x = (lv_coord_t)(((buf[3] & 0x0F) << 8) | buf[4]);
            s_y = (lv_coord_t)(((buf[5] & 0x0F) << 8) | buf[6]);
            ESP_LOGI(TAG, "TOUCH x=%d y=%d gesture=0x%02x", s_x, s_y, buf[1]);

            if (g_screen_dimmed) {
                // First tap only wakes the screen — don't pass to LVGL so
                // the tap doesn't accidentally fire a button while dark.
                globals_touch_activity();
                s_pressed = false;
            } else {
                globals_touch_activity();
                s_pressed = true;
            }
        } else {
            s_pressed = false;
        }
    }

    data->point.x = s_x;
    data->point.y = s_y;
    data->state   = s_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

void cst820_init(void)
{
    // Hardware reset via TCA9554 EXIO2 (TP_RST)
    tca9554_set_pin(TCA_PIN_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    tca9554_set_pin(TCA_PIN_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(60));

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << TP_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);

    esp_err_t ret = cst820_write_reg(0xFA, 0x01);
    ESP_LOGI(TAG, "CST820 scan mode: %s", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    // Disable auto-sleep (CST816-family reg 0xFE, DisAutoSleep — any non-zero
    // value keeps it awake). Without this the controller powers down its scan
    // engine after a few seconds idle and NACKs every I2C read until touched,
    // which floods the log and adds wake latency. This device is mains-powered,
    // so keeping touch always-on costs nothing.
    ret = cst820_write_reg(0xFE, 0x01);
    ESP_LOGI(TAG, "CST820 auto-sleep disable: %s", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    uint8_t id_reg = 0x15, chip_id = 0;
    ret = i2c_master_write_read_device(IO_I2C_PORT, TP_I2C_ADDR,
                                       &id_reg, 1, &chip_id, 1,
                                       pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "CST820 chip_id=0x%02x (%s)", chip_id, esp_err_to_name(ret));

    static lv_indev_drv_t drv;
    lv_indev_drv_init(&drv);
    drv.type    = LV_INDEV_TYPE_POINTER;
    drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&drv);
    ESP_LOGI(TAG, "CST820 registered (INT=GPIO%d)", TP_INT_GPIO);
}
