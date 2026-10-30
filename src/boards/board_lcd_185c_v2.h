#pragma once
// Board: Waveshare ESP32-S3-Touch-LCD-1.85C V2 (PCB Rev2.0; V1 has different audio).
// ESP32-S3R8 (8 MB OPI PSRAM / 16 MB flash), ST77916 360x360 round LCD (QSPI) with an
// LEDC backlight, CST816 touch, TCA9554 expander, ES8311 DAC + NS4150B amp, ES7210 mics.
//
// Pins come from Waveshare's factory demo (ESP32-S3-Touch-LCD-1.85C @ 8ead4a96) and the
// V2 schematic, as used by Hermes Gadget SDK's working port of this board
// (docs/hardware.md, firmware/esp32/main/board.cpp). The panel and touch resets are on
// the TCA9554, so display.cpp pulses them before the panel and touch start.

#define BOARD_NAME          "ESP32-S3-Touch-LCD-1.85C-V2"
#define BOARD_PIO_ENV       "esp32-s3-lcd-185c-v2"

// ---------- Screen ----------
// The UI keeps the 466x466 layout of the AMOLED boards (config.h's SCREEN_*); panel_scale.h
// resamples each frame down to the 360x360 panel, and touch scales back up.
#define PANEL_W             360
#define PANEL_H             360

// ---------- Panel (ST77916, QSPI) ----------
#define PIN_LCD_CS          21
#define PIN_LCD_SCLK        40             // QSPI PCLK
#define PIN_LCD_D0          46
#define PIN_LCD_D1          45
#define PIN_LCD_D2          42
#define PIN_LCD_D3          41
#define LCD_QSPI_HZ         80000000       // as in the Hermes port
#define PIN_LCD_BL          5              // LEDC backlight, active high
#define LCD_BL_PWM_FREQ     5000
#define LCD_BL_PWM_BITS     10

// ---------- Shared I2C (touch + expander + codecs + RTC) ----------
#define PIN_I2C_SDA         11
#define PIN_I2C_SCL         10

// ---------- TCA9554 expander (EXIO numbering from 1, as in the demo: EXIO1 = P0) ----------
#define TCA9554_ADDR        0x20
#define TCA9554_OUTPUTS     0x03           // only P0/P1 become outputs; the rest stay as they boot
#define EXIO_TP_RST         1              // CST816 reset (P0)
#define EXIO_LCD_RST        2              // ST77916 reset (P1)

// ---------- Touch (CST816, same register protocol as the CST820) ----------
#define TOUCH_DRIVER_CST820 1
#define I2C_ADDR_TOUCH      0x15
#define PIN_TP_INT          -1             // polled
#define TP_MIRROR_X         false
#define TP_MIRROR_Y         false

// ---------- Audio (ES8311, clocked from BCLK) ----------
#define BOARD_HAS_PMIC      0
#define BOARD_HAS_AUDIO     1
#define PIN_I2S_MCLK        2
#define PIN_I2S_BCLK        48
#define PIN_I2S_LRCLK       38
#define PIN_I2S_DOUT        47
#define PIN_I2S_DIN         39
#define PIN_AUDIO_PA        15             // NS4150B enable

// I2C scan on hardware: 0x15 touch, 0x18 ES8311, 0x20 TCA9554, 0x40 ES7210, 0x51 PCF85063
// (its seconds register ticks). No IMU (nothing at 0x6A/0x6B) and no GPS module.
#define I2C_ADDR_RTC        0x51
#define BOARD_HAS_IMU       0
#define BOARD_HAS_GPS       0

#define PIN_BOOT_BUTTON     0
