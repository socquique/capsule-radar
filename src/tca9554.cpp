// TCA9554PWR I2C GPIO expander driver. See tca9554.h. Boards that define TCA9554_ADDR.
#include "config.h"
#if defined(TCA9554_ADDR)
#include "tca9554.h"
#include <Arduino.h>
#include <Wire.h>

#define TCA_INPUT_REG   0x00   // input levels (read-only)
#define TCA_OUTPUT_REG  0x01   // output latch
#define TCA_CONFIG_REG  0x03   // direction: 1 = input, 0 = output

// False on a failed read. A read-modify-write must then skip its write: writing back a 0
// makes every pin an output (direction) or drives every other output low (latch).
static bool read_reg(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission() != 0 || Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1) != 1) {
        Serial.println("[tca9554] read: no ACK");
        return false;
    }
    *val = Wire.read();
    return true;
}

static void write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    if (Wire.endTransmission() != 0) Serial.println("[tca9554] write: no ACK");
}

void tca9554_init() {
    // On a failed read, start from the power-on default (all inputs): the board still gets
    // its outputs, and only those. Skipping the write would leave the LCD 2.1's resets undriven.
    uint8_t cfg = 0xFF;
    read_reg(TCA_CONFIG_REG, &cfg);
    write_reg(TCA_CONFIG_REG, cfg & (uint8_t)~TCA9554_OUTPUTS);
    Serial.printf("[tca9554] init @0x%02X (outputs 0x%02X)\n", TCA9554_ADDR, TCA9554_OUTPUTS);
}

void tca9554_set(uint8_t pin, uint8_t level) {
    if (pin < 1 || pin > 8) return;
    uint8_t out;
    if (!read_reg(TCA_OUTPUT_REG, &out)) return;
    const uint8_t mask = (uint8_t)(1 << (pin - 1));
    if (level) out |= mask;
    else       out &= (uint8_t)~mask;
    write_reg(TCA_OUTPUT_REG, out);
}

uint8_t tca9554_get(uint8_t pin) {
    if (pin < 1 || pin > 8) return 0;
    uint8_t in = 0;
    read_reg(TCA_INPUT_REG, &in);
    return (in >> (pin - 1)) & 0x01;
}

#endif  // TCA9554_ADDR
