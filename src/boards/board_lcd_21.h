#pragma once
// ---------------------------------------------------------------------------
//  Waveshare ESP32-S3-Touch-LCD-2.1 : ST7701 480x480 IPS, 16-bit RGB parallel.
#define BOARD_NAME          "ESP32-S3-Touch-LCD-2.1"
#define BOARD_PIO_ENV       "esp32-s3-lcd-21"
//  Pins/timings are from Waveshare's own Arduino demo (LVGL_Arduino):
//    files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1-Code.zip
//  The ST7701 init sequence lives in display_st7701.cpp
//  (lifted verbatim from the demo). Only the higher-level knobs are here.
// ---------------------------------------------------------------------------

// ---------- Screen (ST7701 IPS, 480x480 round) ----------
#define SCREEN_W            480
#define SCREEN_H            480
#define SCREEN_CX           240
#define SCREEN_CY           240
#define RADAR_R_OUTER_PX    236            // outer ring radius in px (fills the 480 round panel)

// ---------- Backlight (PWM on a real GPIO — unlike the AMOLED's panel command) ----------
#define PIN_LCD_BL          6              // ledc PWM backlight enable/level
#define LCD_BL_PWM_FREQ     20000          // 20 kHz (from the demo)
#define LCD_BL_PWM_BITS     10             // 10-bit resolution -> duty 0..1023

// ---------- 3-wire SPI used only to push the ST7701 init sequence ----------
#define PIN_LCD_SCLK        2              // also referenced by main.cpp's pin sanity print
#define PIN_LCD_MOSI        1

// ---------- Shared I2C bus (touch + TCA9554 expander + QMI8658 IMU + PCF85063 RTC) ----------
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         7

// ---------- TCA9554PWR I2C GPIO expander (drives lines the ESP32 GPIOs don't) ----------
#define TCA9554_ADDR        0x20
#define EXIO_LCD_RST        1              // ST7701 reset   (EXIO1)
#define EXIO_TP_RST         2              // CST820 reset   (EXIO2)
#define EXIO_AUX            8              // vendor init holds buzzer/aux line low
#define EXIO_LCD_CS         3              // ST7701 3-wire SPI chip-select (EXIO3)

// ---------- Touch (CST820 capacitive, I2C) ----------
#define PIN_TP_INT          16             // CST820 interrupt (real GPIO); reset is EXIO_TP_RST
#define I2C_ADDR_TOUCH      0x15
#define TP_MIRROR_X         false          // adjust on-hardware if X is flipped
#define TP_MIRROR_Y         false          // adjust on-hardware if Y is flipped

// ---------- Peripherals ----------
#define BOARD_HAS_PMIC      0
#define BOARD_HAS_AUDIO     0
#define TOUCH_DRIVER_CST820 1
#define ADSB_TASK_STACK_BYTES 32768 // IDF-mode TLS needs more stack than precompiled Arduino

// ---------- I2C addresses (shared bus) ----------
#define I2C_ADDR_IMU        0x6B           // QMI8658 (imu driver also probes 0x6A)
#define I2C_ADDR_RTC        0x51           // PCF85063


#define PIN_BOOT_BUTTON     0              // BOOT button


// Vendor RGB bus and timings (12 MHz pixel clock for PSRAM DMA headroom).
#define RGB_PCLK_HZ   (12 * 1000 * 1000)   // 12 MHz (~44 Hz). Lower than the demo's 16 MHz so the
                                            // direct-from-PSRAM DMA keeps up without a bounce buffer
                                            // (no drift). Drop to 10 MHz if any drift remains.
#define RGB_HSYNC     38
#define RGB_VSYNC     39
#define RGB_DE        40
#define RGB_PCLK      41
static const int RGB_DATA_PINS[16] = {
    5, 45, 48, 47, 21,          // B0..B4 (DATA0..4)
    14, 13, 12, 11, 10, 9,      // G0..G5 (DATA5..10)
    46, 3, 8, 18, 17            // R0..R4 (DATA11..15)
};
#define RGB_HPW 8
#define RGB_HBP 10
#define RGB_HFP 50
#define RGB_VPW 3
#define RGB_VBP 8
#define RGB_VFP 8

