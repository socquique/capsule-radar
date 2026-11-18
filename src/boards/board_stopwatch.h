#pragma once
// Board: M5Stack StopWatch (C152).
// ESP32-S3R8 (8 MB OPI PSRAM / 16 MB flash), CO5300 466x466 AMOLED (QSPI), CST820 touch,
// M5PM1 PMIC (0x6E) + M5IOE1 IO expander (0x4F), BMI270 IMU, RX8130CE RTC, ES8311 audio.
//
// Pins, panel gap and the power/reset sequence come from M5GFX's board support
// (src/board_detect/m5/: generated/esp32s3_board_data.hpp "stopwatch", pmic_ops.hpp
// "stopwatch_power_on"). NOT yet confirmed on hardware.
//
// The panel and touch have no reset GPIO: both resets sit on the M5IOE1 expander and are
// pulsed by board_power_begin() (board_stopwatch.cpp), which must run before display::begin().

#define BOARD_NAME          "M5Stack-StopWatch"
#define BOARD_PIO_ENV       "esp32-s3-stopwatch"

// ---------- Panel ----------
#define PIN_LCD_CS          39
#define PIN_LCD_RST         -1             // on M5IOE1, see board_power_begin()
#define PIN_LCD_SCLK        40
#define PIN_LCD_D0          41
#define PIN_LCD_D1          42
#define PIN_LCD_D2          46
#define PIN_LCD_D3          45
// M5GFX: 480x480 RAM, offset_x 6 -- same as the Waveshare boards. Check on hardware (0..8).
#define LCD_COL_OFFSET      6
#define LCD_ROW_OFFSET      0
#define LCD_QSPI_HZ         40000000       // M5GFX uses 80 MHz; measured no fps gain here, so 40

// ---------- Shared I2C (touch + PMIC + expander + IMU + RTC) ----------
#define PIN_I2C_SDA         47
#define PIN_I2C_SCL         48
#define I2C_ADDR_PM1        0x6E
#define I2C_ADDR_IOE1       0x4F

// ---------- Touch ----------
// Most units have a CST820 (0x15); M5GFX notes some ship a CST9217 (0x5A) on the same pins.
// shortcut: only the CST820 is driven; if the boot log shows 0x5A, switch to TOUCH_DRIVER_CST9217.
#define TOUCH_DRIVER_CST820 1
#define I2C_ADDR_TOUCH      0x15           // reports full 0..466 coords (measured), not M5GFX's 0..233
#define PIN_TP_INT          13
#define TP_MIRROR_X         false
#define TP_MIRROR_Y         false

// ---------- Peripherals ----------
// Battery (M5PM1), audio (ES8311), IMU (BMI270, not the QMI8658 the driver probes) and
// RTC (RX8130CE, not a PCF85063) are not driven: the firmware runs without them.
#define BOARD_HAS_PMIC      0
#define BOARD_HAS_AUDIO     0
#define BOARD_HAS_RTC       0
#define I2C_ADDR_IMU        0x68           // BMI270; imu_qmi8658 finds no QMI8658 and stays off
#define I2C_ADDR_RTC        0x32           // RX8130CE; unused (BOARD_HAS_RTC 0)

#define PIN_KEY_A           2              // active low; radar: next zoom step
#define PIN_KEY_B           1              // active low; radar: next theme

void board_power_begin();                  // PM1 rails + IOE1 panel/touch reset (board_stopwatch.cpp)
