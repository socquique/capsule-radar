#pragma once
// TCA9554PWR I2C GPIO expander (BOARD_LCD21 only).
//
// On the ESP32-S3-Touch-LCD-2.1 a few control lines the SoC GPIOs don't reach are wired
// to this expander: the ST7701 reset (EXIO1) + chip-select (EXIO3) and the CST820 touch
// reset (EXIO2). Ported from Waveshare's TCA9554PWR demo driver, trimmed to what we use
// and pointed at the shared global `Wire` bus (init'd on PIN_I2C_SDA/SCL by the display).
#include <stdint.h>

// Configure all 8 EXIO as outputs (mode bits: 0 = output). Call after Wire.begin().
void tca9554_init();

// Set one EXIO pin (1..8) high/low without disturbing the others.
void tca9554_set(uint8_t pin, uint8_t level);

// Read one EXIO pin's input level (1..8).
uint8_t tca9554_get(uint8_t pin);
