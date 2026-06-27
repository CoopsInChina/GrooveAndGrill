#include "tca9554.h"
#include "board_config.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tca9554";

#define TCA9554_REG_INPUT   0x00
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_POLAR   0x02
#define TCA9554_REG_CONFIG  0x03

#define I2C_TIMEOUT_MS      50

// Pins 1-3 (LCD_RST, TP_RST, LCD_CS) deasserted high; pin 8 (buzzer) low.
static uint8_t s_output_shadow = 0x07;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(IO_I2C_PORT, TCA9554_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(IO_I2C_PORT, TCA9554_I2C_ADDR,
                                        &reg, 1, val, 1,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t tca9554_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = IO_I2C_SDA_GPIO,
        .scl_io_num       = IO_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = IO_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(IO_I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(IO_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    ESP_ERROR_CHECK(write_reg(TCA9554_REG_OUTPUT, s_output_shadow));
    ESP_ERROR_CHECK(write_reg(TCA9554_REG_CONFIG, 0x00));  // all outputs

    ESP_LOGI(TAG, "TCA9554PWR ready");
    return ESP_OK;
}

esp_err_t tca9554_set_pin(uint8_t pin, bool level)
{
    if (pin < 1 || pin > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mask = 1u << (pin - 1);
    if (level) {
        s_output_shadow |= mask;
    } else {
        s_output_shadow &= ~mask;
    }
    return write_reg(TCA9554_REG_OUTPUT, s_output_shadow);
}
