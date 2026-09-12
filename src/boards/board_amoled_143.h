#pragma once
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.43.
// ESP32-S3-PICO (8 MB PSRAM / 16 MB flash), CO5300 466x466 AMOLED (QSPI),
// FT3168 touch, QMI8658 IMU, PCF85063 RTC.  No PMIC and no audio codec.
//
// Same panel size and controller as the 1.75, but a COMPLETELY different pin map --
// flashing the 1.75 build here gives a black screen and a silent I2C bus.
//
// Pins come from the Arduino-ESP32 core's own board variant
// (variants/waveshare_esp32_s3_touch_amoled_143/pins_arduino.h, which the 1.64 board
// shares), and every one of them was then confirmed on real hardware:
//   - I2C scan on 47/48 answers 0x51 (PCF85063) + 0x6B (QMI8658) + 0x38 (FT3168)
//   - the panel drives correctly on CS=9 SCK=10 D0..D3=11..14 RST=21
// Do not "tidy" these against the Waveshare wiki pin table: that table is misaligned
// (it lists an I2C SCL of GPIO49, which does not exist on an ESP32-S3).

#define BOARD_NAME          "ESP32-S3-Touch-AMOLED-1.43"
#define BOARD_PIO_ENV       "esp32-s3-amoled-143"   // used in the OTA hint printed at boot

// ---------- Panel ----------
#define PIN_LCD_CS          9
#define PIN_LCD_RST         21
#define PIN_LCD_SCLK        10             // QSPI PCLK
#define PIN_LCD_D0          11
#define PIN_LCD_D1          12
#define PIN_LCD_D2          13
#define PIN_LCD_D3          14
// Same gap as the 1.75: the CO5300 has a 480-wide RAM behind a 466-wide panel.
// Dialled in on hardware with a pixel-accurate nudge tool (full frame composed in
// PSRAM and blitted in one aligned window, so a 1px step really is 1px); 6 centres
// the image, 7 and 8 visibly overshoot.
#define LCD_COL_OFFSET      6
#define LCD_ROW_OFFSET      0
#define LCD_QSPI_HZ         40000000       // vendor rate; verified. 80 MHz is untested here.

// ---------- Shared I2C (touch + IMU + RTC) ----------
#define PIN_I2C_SDA         47
#define PIN_I2C_SCL         48

// ---------- Touch ----------
// The FT3168 has no dedicated reset line broken out: its reset is tied to the panel
// reset (GPIO21), so it only appears on the bus AFTER the LCD has been initialised.
// display::begin() already calls gfx->begin() before touch_begin(), which is what
// makes this work -- do not reorder those two.
#define TOUCH_DRIVER_FT3168 1
#define I2C_ADDR_TOUCH      0x38           // confirmed; chip id reg 0xA3 reads 0x64
#define PIN_TP_INT          -1             // not broken out
#define PIN_TP_RST          -1             // shared with PIN_LCD_RST
// Raw controller coordinates already match the panel orientation (verified: the
// on-screen dot lands under the fingertip, x spans 24..443, y spans 0..459).
#define TP_MIRROR_X         false
#define TP_MIRROR_Y         false

// ---------- Peripherals ----------
// No AXP2101 and no ES8311 on this board (an I2C scan finds neither). Battery level
// is instead a divided analog reading on GPIO4, but the divider ratio is not
// documented, so battery reporting stays off rather than showing an invented number.
#define BOARD_HAS_PMIC      0
#define BOARD_HAS_AUDIO     0
#define PIN_BAT_ADC         4              // unused for now; see note above
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51

#define PIN_BOOT_BUTTON     0
