#pragma once

// ============================================================
// Waveshare ESP32-S3-Touch-LCD-2.1  —  hardware pin map
// Derived from official Waveshare gist (fallenartist/22d1d01e...)
// ============================================================

// ---- SPI bus for ST7701S initialisation commands ----
// (write-only; no MISO needed)
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_SPI_CLK_GPIO    GPIO_NUM_2
#define LCD_SPI_MOSI_GPIO   GPIO_NUM_1
#define LCD_SPI_CLK_HZ      40000000    // 40 MHz

// ---- TCA9554PWR I/O expander (I2C) ----
#define IO_I2C_PORT         I2C_NUM_0
#define IO_I2C_SCL_GPIO     GPIO_NUM_7
#define IO_I2C_SDA_GPIO     GPIO_NUM_15
#define IO_I2C_FREQ_HZ      400000
#define TCA9554_I2C_ADDR    0x20

// TCA9554 pin numbers (1-based, as used by the Waveshare driver)
#define TCA_PIN_LCD_RST     1   // EXIO1
#define TCA_PIN_TP_RST      2   // EXIO2
#define TCA_PIN_LCD_CS      3   // EXIO3

// ---- Backlight (LEDC PWM) ----
#define LCD_BL_GPIO         GPIO_NUM_6
#define LCD_BL_LEDC_TIMER   LEDC_TIMER_0
#define LCD_BL_LEDC_CH      LEDC_CHANNEL_0
#define LCD_BL_LEDC_FREQ_HZ 20000
#define LCD_BL_LEDC_RES     LEDC_TIMER_10_BIT   // 0-1023

// ---- RGB parallel panel ----
#define LCD_H_RES           480
#define LCD_V_RES           480
#define LCD_PCLK_HZ         (16 * 1000 * 1000)

// Timing (from Waveshare demo)
#define LCD_HPW             8
#define LCD_HBP             10
#define LCD_HFP             50
#define LCD_VPW             3
#define LCD_VBP             8
#define LCD_VFP             8

#define LCD_HSYNC_GPIO      GPIO_NUM_38
#define LCD_VSYNC_GPIO      GPIO_NUM_39
#define LCD_DE_GPIO         GPIO_NUM_40
#define LCD_PCLK_GPIO       GPIO_NUM_41
#define LCD_DISP_GPIO       (-1)        // not wired

// 16-bit RGB data bus: DATA[0..15]
#define LCD_DATA0_GPIO      GPIO_NUM_5
#define LCD_DATA1_GPIO      GPIO_NUM_45
#define LCD_DATA2_GPIO      GPIO_NUM_48
#define LCD_DATA3_GPIO      GPIO_NUM_47
#define LCD_DATA4_GPIO      GPIO_NUM_21
#define LCD_DATA5_GPIO      GPIO_NUM_14
#define LCD_DATA6_GPIO      GPIO_NUM_13
#define LCD_DATA7_GPIO      GPIO_NUM_12
#define LCD_DATA8_GPIO      GPIO_NUM_11
#define LCD_DATA9_GPIO      GPIO_NUM_10
#define LCD_DATA10_GPIO     GPIO_NUM_9
#define LCD_DATA11_GPIO     GPIO_NUM_46
#define LCD_DATA12_GPIO     GPIO_NUM_3
#define LCD_DATA13_GPIO     GPIO_NUM_8
#define LCD_DATA14_GPIO     GPIO_NUM_18
#define LCD_DATA15_GPIO     GPIO_NUM_17

// ---- Touch controller (CST820, I2C shared with TCA9554) ----
#define TP_I2C_ADDR         0x15
#define TP_INT_GPIO         GPIO_NUM_16

// ---- CAN bus (TWAI) — SN65HVD230 wired to the board's UART JST connector ----
// The UART connector (right side of board) exposes GPIO43/GPIO44.
// Console is routed through USB-Serial/JTAG so these pins are free for TWAI.
//
//   UART connector "TXD" (GPIO43) ──► SN65HVD230 TXD
//   UART connector "RXD" (GPIO44) ◄── SN65HVD230 RXD
//   UART connector "3V3"          ──► SN65HVD230 VCC
//   UART connector "GND"          ──► SN65HVD230 GND
#define CAN_TX_GPIO         GPIO_NUM_43
#define CAN_RX_GPIO         GPIO_NUM_44
