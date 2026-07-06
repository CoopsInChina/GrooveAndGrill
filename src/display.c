#include "display.h"
#include "board_config.h"
#include "tca9554.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;

// ---- Backlight -------------------------------------------------------

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_RES,
        .freq_hz         = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_BL_LEDC_CH,
        .timer_sel  = LCD_BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LCD_BL_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void display_set_backlight(bool on)
{
    uint32_t duty = on ? ((1u << LCD_BL_LEDC_RES) - 1) : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH);
}

void display_set_brightness(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint32_t max_duty = (1u << LCD_BL_LEDC_RES) - 1;
    uint32_t duty = (uint32_t)((pct * (int)max_duty) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH);
}

// ---- ST7701S 3-wire SPI init commands --------------------------------
// The SPI transaction format used by the Waveshare board:
//   command_bits=1  (0 = command byte, 1 = data byte)
//   address_bits=8  (the actual byte value)
// CS is managed via TCA9554 EXIO_PIN3.

static spi_device_handle_t s_spi = NULL;

static void lcd_spi_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_SPI_MOSI_GPIO,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_SPI_CLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .command_bits    = 1,
        .address_bits    = 8,
        .mode            = 0,                   // CPOL=0, CPHA=0
        .clock_speed_hz  = LCD_SPI_CLK_HZ,
        .spics_io_num    = -1,                  // CS toggled manually in task context
        .queue_size      = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI_HOST, &dev, &s_spi));
}

static void write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .cmd    = 0,        // D/C = 0 (command)
        .addr   = cmd,
        .length = 0,
    };
    spi_device_transmit(s_spi, &t);
}

static void write_data(uint8_t data)
{
    spi_transaction_t t = {
        .cmd    = 1,        // D/C = 1 (data)
        .addr   = data,
        .length = 0,
    };
    spi_device_transmit(s_spi, &t);
}

// Full ST7701S init sequence — extracted verbatim from the Waveshare
// reference gist (fallenartist/22d1d01e125afb02ae4ebdcdf02d1f80).
static void st7701_send_init_cmds(void)
{
    // CMD2 BK0
    write_cmd(0xFF); write_data(0x77); write_data(0x01);
    write_data(0x00); write_data(0x00); write_data(0x10);

    write_cmd(0xC0); write_data(0x3B); write_data(0x00);
    write_cmd(0xC1); write_data(0x0B); write_data(0x02);
    write_cmd(0xC2); write_data(0x07); write_data(0x02);
    write_cmd(0xCC); write_data(0x10);
    write_cmd(0xCD); write_data(0x08);  // RGB format

    // Positive gamma
    write_cmd(0xB0);
    write_data(0x00); write_data(0x11); write_data(0x16); write_data(0x0E);
    write_data(0x11); write_data(0x06); write_data(0x05); write_data(0x09);
    write_data(0x08); write_data(0x21); write_data(0x06); write_data(0x13);
    write_data(0x10); write_data(0x29); write_data(0x31); write_data(0x18);

    // Negative gamma
    write_cmd(0xB1);
    write_data(0x00); write_data(0x11); write_data(0x16); write_data(0x0E);
    write_data(0x11); write_data(0x07); write_data(0x05); write_data(0x09);
    write_data(0x09); write_data(0x21); write_data(0x05); write_data(0x13);
    write_data(0x11); write_data(0x2A); write_data(0x31); write_data(0x18);

    // CMD2 BK1
    write_cmd(0xFF); write_data(0x77); write_data(0x01);
    write_data(0x00); write_data(0x00); write_data(0x11);

    write_cmd(0xB0); write_data(0x6D);  // VOP
    write_cmd(0xB1); write_data(0x37);  // VCOM
    write_cmd(0xB2); write_data(0x81);  // VGH 12 V
    write_cmd(0xB3); write_data(0x80);
    write_cmd(0xB5); write_data(0x43);  // VGL −8.3 V
    write_cmd(0xB7); write_data(0x85);
    write_cmd(0xB8); write_data(0x20);
    write_cmd(0xC1); write_data(0x78);
    write_cmd(0xC2); write_data(0x78);
    write_cmd(0xD0); write_data(0x88);

    write_cmd(0xE0); write_data(0x00); write_data(0x00); write_data(0x02);

    write_cmd(0xE1);
    write_data(0x03); write_data(0xA0); write_data(0x00); write_data(0x00);
    write_data(0x04); write_data(0xA0); write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x20); write_data(0x20);

    write_cmd(0xE2);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);
    write_data(0x00);

    write_cmd(0xE3); write_data(0x00); write_data(0x00); write_data(0x11); write_data(0x00);
    write_cmd(0xE4); write_data(0x22); write_data(0x00);

    write_cmd(0xE5);
    write_data(0x05); write_data(0xEC); write_data(0xA0); write_data(0xA0);
    write_data(0x07); write_data(0xEE); write_data(0xA0); write_data(0xA0);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);

    write_cmd(0xE6); write_data(0x00); write_data(0x00); write_data(0x11); write_data(0x00);
    write_cmd(0xE7); write_data(0x22); write_data(0x00);

    write_cmd(0xE8);
    write_data(0x06); write_data(0xED); write_data(0xA0); write_data(0xA0);
    write_data(0x08); write_data(0xEF); write_data(0xA0); write_data(0xA0);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);

    write_cmd(0xEB);
    write_data(0x00); write_data(0x00); write_data(0x40); write_data(0x40);
    write_data(0x00); write_data(0x00); write_data(0x00);

    write_cmd(0xED);
    write_data(0xFF); write_data(0xFF); write_data(0xFF); write_data(0xBA);
    write_data(0x0A); write_data(0xBF); write_data(0x45); write_data(0xFF);
    write_data(0xFF); write_data(0x54); write_data(0xFB); write_data(0xA0);
    write_data(0xAB); write_data(0xFF); write_data(0xFF); write_data(0xFF);

    write_cmd(0xEF);
    write_data(0x10); write_data(0x0D); write_data(0x04);
    write_data(0x08); write_data(0x3F); write_data(0x1F);

    // CMD2 BK3
    write_cmd(0xFF); write_data(0x77); write_data(0x01);
    write_data(0x00); write_data(0x00); write_data(0x13);
    write_cmd(0xEF); write_data(0x08);

    // Return to user command set
    write_cmd(0xFF); write_data(0x77); write_data(0x01);
    write_data(0x00); write_data(0x00); write_data(0x00);

    write_cmd(0x36); write_data(0x00);  // MADCTL — normal orientation
    write_cmd(0x3A); write_data(0x66);  // COLMOD — 18-bit colour

    write_cmd(0x11);                    // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(480));
    write_cmd(0x20);                    // Display Inversion Off
    vTaskDelay(pdMS_TO_TICKS(120));
    write_cmd(0x29);                    // Display On
}

// ---- Public init ---------------------------------------------------

esp_err_t display_init(void)
{
    // 1. Hardware reset via TCA9554
    ESP_ERROR_CHECK(tca9554_set_pin(TCA_PIN_LCD_RST, false));
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_ERROR_CHECK(tca9554_set_pin(TCA_PIN_LCD_RST, true));
    vTaskDelay(pdMS_TO_TICKS(60));

    // 2. Send ST7701S init sequence over SPI (CS managed here in task context)
    lcd_spi_init();
    tca9554_set_pin(TCA_PIN_LCD_CS, false);   // assert CS
    st7701_send_init_cmds();
    tca9554_set_pin(TCA_PIN_LCD_CS, true);    // deassert CS
    ESP_LOGI(TAG, "ST7701S init done");

    // 3. Backlight (off until ComfortEnable turns it on)
    backlight_init();
    display_set_backlight(false);

    // 4. RGB panel
    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src        = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz          = LCD_PCLK_HZ,
            .h_res            = LCD_H_RES,
            .v_res            = LCD_V_RES,
            .hsync_pulse_width = LCD_HPW,
            .hsync_back_porch  = LCD_HBP,
            .hsync_front_porch = LCD_HFP,
            .vsync_pulse_width = LCD_VPW,
            .vsync_back_porch  = LCD_VBP,
            .vsync_front_porch = LCD_VFP,
            .flags = {
                .pclk_active_neg = false,
            },
        },
        .data_width          = 16,
        .bits_per_pixel      = 16,
        .num_fbs             = 2,
        .bounce_buffer_size_px = LCD_H_RES * 10,
        .psram_trans_align   = 64,
        .hsync_gpio_num      = LCD_HSYNC_GPIO,
        .vsync_gpio_num      = LCD_VSYNC_GPIO,
        .de_gpio_num         = LCD_DE_GPIO,
        .pclk_gpio_num       = LCD_PCLK_GPIO,
        .disp_gpio_num       = LCD_DISP_GPIO,
        .data_gpio_nums = {
            LCD_DATA0_GPIO,  LCD_DATA1_GPIO,  LCD_DATA2_GPIO,  LCD_DATA3_GPIO,
            LCD_DATA4_GPIO,  LCD_DATA5_GPIO,  LCD_DATA6_GPIO,  LCD_DATA7_GPIO,
            LCD_DATA8_GPIO,  LCD_DATA9_GPIO,  LCD_DATA10_GPIO, LCD_DATA11_GPIO,
            LCD_DATA12_GPIO, LCD_DATA13_GPIO, LCD_DATA14_GPIO, LCD_DATA15_GPIO,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // 5. LVGL port
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // For RGB panels, use lvgl_port_add_disp_rgb() — lvgl_port_add_disp() is
    // for SPI/I2C panels and asserts io_handle != NULL.
    // avoid_tearing=true: port fetches the two PSRAM frame buffers directly
    // from the RGB panel via esp_lcd_rgb_panel_get_frame_buffer() and uses
    // them as LVGL's draw buffers with vsync-synchronised swapping.
    // bb_mode=true: uses the bounce buffer registered in the RGB panel config.
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle  = s_panel,
        .buffer_size   = LCD_H_RES * LCD_V_RES,
        .double_buffer = false,         // avoid_tearing owns double-buffering
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = false,
            .buff_spiram = false,       // buffers come from the RGB panel
            .sw_rotate   = false,
            .full_refresh = false,
            .direct_mode  = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t lvgl_rgb_cfg = {
        .flags = {
            .bb_mode       = false,  // use on_vsync callback (reliable across all IDF 5.x)
            .avoid_tearing = true,
        },
    };
    lvgl_port_add_disp_rgb(&disp_cfg, &lvgl_rgb_cfg);

    ESP_LOGI(TAG, "LVGL display ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

bool display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
