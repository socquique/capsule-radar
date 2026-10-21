// TCA9554PWR I2C GPIO expander driver. See tca9554.h. BOARD_LCD21 only.
#include "config.h"
#if defined(BOARD_LCD21)
#include "tca9554.h"
#include <Arduino.h>
#include <Wire.h>

#define TCA_INPUT_REG   0x00   // input levels (read-only)
#define TCA_OUTPUT_REG  0x01   // output latch
#define TCA_CONFIG_REG  0x03   // direction: 1 = input, 0 = output

static uint8_t read_reg(uint8_t reg) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) {
        Serial.println("[tca9554] read: no ACK");
        return 0;
    }
    Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

static void write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    if (Wire.endTransmission() != 0) Serial.println("[tca9554] write: no ACK");
}

void tca9554_init() {
    write_reg(TCA_CONFIG_REG, 0x00);   // all pins output
    Serial.printf("[tca9554] init @0x%02X (all outputs)\n", TCA9554_ADDR);
}

void tca9554_set(uint8_t pin, uint8_t level) {
    if (pin < 1 || pin > 8) return;
    uint8_t out = read_reg(TCA_OUTPUT_REG);
    const uint8_t mask = (uint8_t)(1 << (pin - 1));
    if (level) out |= mask;
    else       out &= (uint8_t)~mask;
    write_reg(TCA_OUTPUT_REG, out);
}

uint8_t tca9554_get(uint8_t pin) {
    if (pin < 1 || pin > 8) return 0;
    return (read_reg(TCA_INPUT_REG) >> (pin - 1)) & 0x01;
}

#endif  // BOARD_LCD21
