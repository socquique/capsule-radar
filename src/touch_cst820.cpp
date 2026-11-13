// CST820 / CST816 capacitive touch over I2C (Arduino). LCD 2.1 (CST820), LCD 1.85C V2 (CST816).
// Ported from Waveshare's Touch_CST820 demo: single touch point read from the data
// block at register 0x01 -> {gesture, points, xh, xl, yh, yl}, 12-bit coordinates.
// The controller's RST line is on the TCA9554 expander (EXIO_TP_RST), not a SoC GPIO.
#include "config.h"
#if TOUCH_DRIVER_CST820
#include "touch_cst820.h"
#include "tca9554.h"
#include <Arduino.h>
#include <Wire.h>

#define CST820_REG_DATA        0x01   // gesture,points,xh,xl,yh,yl (6 bytes)
#define CST820_REG_CHIPID      0xA7
#define CST820_REG_DISSLEEP    0xFE   // 1 = disable auto-sleep (keep reporting while polled)

// Standard CST816/CST820 access: write the 8-bit register pointer, repeated-start read.
// (Waveshare's demo writes a 16-bit pointer; the single-byte form below is the mainstream
// CST8xx protocol and unambiguous. If touch misbehaves on hardware, that's the first knob.)
static bool cst_read(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;   // repeated start
    if (Wire.requestFrom((uint8_t)I2C_ADDR_TOUCH, len) < len) return false;
    for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

static void cst_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

bool touch_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);   // idempotent (display already begins it)
    if (PIN_TP_INT >= 0) pinMode(PIN_TP_INT, INPUT);

    // hardware reset via the expander (low 10 ms, high 50 ms)
    tca9554_set(EXIO_TP_RST, 0);
    delay(10);
    tca9554_set(EXIO_TP_RST, 1);
    delay(50);

    cst_write(CST820_REG_DISSLEEP, 0x01);           // we poll, so keep the controller awake

    uint8_t id = 0;
    if (cst_read(CST820_REG_CHIPID, &id, 1)) Serial.printf("[touch] CST8xx chipID=0x%02X\n", id);
    else                                     Serial.println("[touch] CST8xx not responding yet (will keep polling)");
    return true;
}

TouchRead touch_read(uint16_t *ox, uint16_t *oy) {
    uint8_t d[6];
    if (!cst_read(CST820_REG_DATA, d, 6)) return TOUCH_NODATA;
    const uint8_t points = d[1];
    if (points == 0) return TOUCH_UP;

    uint16_t x = (uint16_t)(((d[2] & 0x0F) << 8) | d[3]);
    uint16_t y = (uint16_t)(((d[4] & 0x0F) << 8) | d[5]);

    if (x > PANEL_W - 1) x = PANEL_W - 1;
    if (y > PANEL_H - 1) y = PANEL_H - 1;
    if (TP_MIRROR_X) x = (PANEL_W - 1) - x;
    if (TP_MIRROR_Y) y = (PANEL_H - 1) - y;

    *ox = x;
    *oy = y;
    return TOUCH_DOWN;
}

#endif  // TOUCH_DRIVER_CST820
