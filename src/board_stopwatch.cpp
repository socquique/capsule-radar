// M5Stack StopWatch power-up: M5PM1 rails on, then the panel/touch resets on the M5IOE1
// expander. A register-for-register copy of M5GFX's "stopwatch_power_on"
// (src/board_detect/m5/pmic_ops.hpp) -- raw writes instead of the M5PM1/M5IOE1 libraries,
// which would only wrap these same ~15 registers.
#include "config.h"
#if defined(BOARD_STOPWATCH)
#include <Arduino.h>
#include <Wire.h>

// Both chips may NACK the first transfer after idle (they wake on it): retry.
static bool reg_read(uint8_t addr, uint8_t reg, uint8_t *v) {
    for (int i = 0; i < 5; ++i) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(addr, (uint8_t)1) == 1) {
            *v = Wire.read();
            return true;
        }
        delay(2);
    }
    Serial.printf("[board] I2C 0x%02X reg 0x%02X read failed\n", addr, reg);
    return false;
}

static void reg_write(uint8_t addr, uint8_t reg, uint8_t v) {
    for (int i = 0; i < 5; ++i) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        Wire.write(v);
        if (Wire.endTransmission() == 0) return;
        delay(2);
    }
    Serial.printf("[board] I2C 0x%02X reg 0x%02X write failed\n", addr, reg);
}

static void bits(uint8_t addr, uint8_t reg, uint8_t mask, bool on) {
    uint8_t v;
    if (!reg_read(addr, reg, &v)) return;
    reg_write(addr, reg, on ? (v | mask) : (v & ~mask));
}

static bool ack(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void board_power_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);   // PM1/IOE1 run at 100 kHz in M5GFX

    // M5PM1: clear 0x09/0x0A, enable CHG + DCDC 5V + LDO 3.3V (+bit4) in PWR_CFG 0x06
    reg_write(I2C_ADDR_PM1, 0x09, 0x00);
    reg_write(I2C_ADDR_PM1, 0x0A, 0x00);
    bits(I2C_ADDR_PM1, 0x06, 0x17, true);

    pinMode(PIN_LCD_CS, OUTPUT);
    digitalWrite(PIN_LCD_CS, HIGH);

    // IOE1 boots after PM1 powers it: wait up to 500 ms for it to answer
    const uint32_t t0 = millis();
    while (!ack(I2C_ADDR_IOE1) && millis() - t0 < 500) delay(5);

    reg_write(I2C_ADDR_IOE1, 0x23, 0x00);
    bits(I2C_ADDR_IOE1, 0x13, 0x9D, false);
    bits(I2C_ADDR_IOE1, 0x03, 0x9D, true);     // outputs
    bits(I2C_ADDR_IOE1, 0x05, 0x99, true);     // high (panel power on, resets released)
    delay(10);
    bits(I2C_ADDR_IOE1, 0x05, 0x18, false);    // panel + touch reset low
    delay(8);
    bits(I2C_ADDR_IOE1, 0x05, 0x18, true);     // ...and released
    delay(2);
    bits(I2C_ADDR_IOE1, 0x14, 0x02, false);
    bits(I2C_ADDR_IOE1, 0x04, 0x02, true);
    bits(I2C_ADDR_IOE1, 0x06, 0x02, false);

    Serial.print("[board] I2C scan:");
    for (uint8_t a = 0x08; a < 0x78; ++a) if (ack(a)) Serial.printf(" 0x%02X", a);
    Serial.println();

    Wire.setClock(400000);
}
#endif  // BOARD_STOPWATCH
