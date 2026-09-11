#pragma once
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (the reference board).
// ESP32-S3R8, 8 MB PSRAM / 16 MB flash, CO5300 466x466 AMOLED (QSPI),
// CST9217 touch, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 audio.
//
// Pins verified against the ESPHome definition, the Waveshare board definition in
// xiaozhi-esp32, and a working Arduino_GFX port for this exact panel.

#define BOARD_NAME          "ESP32-S3-Touch-AMOLED-1.75"
#define BOARD_PIO_ENV       "esp32-s3-amoled-175"   // used in the OTA hint printed at boot

// ---------- Panel ----------
#define PIN_LCD_CS          12
#define PIN_LCD_RST         39
#define PIN_LCD_SCLK        38             // QSPI PCLK
#define PIN_LCD_D0          4
#define PIN_LCD_D1          5
#define PIN_LCD_D2          6
#define PIN_LCD_D3          7
#define LCD_COL_OFFSET      6              // CO5300 column (x) gap on this panel (esp_lcd set_gap 0x06)
#define LCD_ROW_OFFSET      0              // no row (y) gap
#define LCD_QSPI_HZ         80000000       // CO5300 QSPI clock (vendor uses 40 MHz; 80 = faster, verify no artifacts)

// ---------- Shared I2C (touch + IMU + RTC + PMIC + audio codec) ----------
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         14

// ---------- Touch ----------
#define TOUCH_DRIVER_CST9217 1
#define I2C_ADDR_TOUCH      0x5A           // CST9217 (corrected from vendor driver; was 0x15)
#define PIN_TP_INT          11
#define PIN_TP_RST          40
#define TP_MIRROR_X         true
#define TP_MIRROR_Y         true

// ---------- Peripherals present on this board ----------
#define BOARD_HAS_PMIC      1              // AXP2101 battery gauge
#define BOARD_HAS_AUDIO     1              // ES8311 codec + speaker
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51
#define I2C_ADDR_PMIC       0x34

// ---------- ES8311 codec over I2S (M4 alert ping) ----------
#define PIN_I2S_MCLK        42
#define PIN_I2S_BCLK        9
#define PIN_I2S_LRCLK       45             // a.k.a. WS
#define PIN_I2S_DOUT        8              // ESP32 -> codec (speaker)
#define PIN_I2S_DIN         10             // codec -> ESP32 (mics)
#define PIN_AUDIO_PA        46             // speaker amp enable

#define PIN_BOOT_BUTTON     0              // BOOT button (held on boot = captive portal, later)
