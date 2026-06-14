// CST9217 capacitive touch over I2C (Arduino). Ported from Waveshare's
// esp_lcd_touch_cst9217: read 10 bytes from reg 0xD000, validate ACK 0xAB,
// unpack the 12-bit X/Y of the first touch point. Single-touch is enough here.
#include "touch_cst9217.h"
#include "config.h"
#include "app_log.h"
#include <Arduino.h>
#include <Wire.h>

#define CST9217_REG_DATA 0xD000
#define CST9217_ACK      0xAB
#define CST9217_DATA_LEN 10        // 1 point * 5 + 5
static uint8_t s_touchPoints = 0;
static uint8_t s_failStreak = 0;
static uint32_t s_quietUntilMs = 0;
static bool s_seenGoodRead = false;

// Read `len` bytes from a 16-bit register (address sent big-endian, then STOP).
// The CST9217 on this board is happier with a completed register-address write;
// repeated-start routes through Wire's write/read non-stop path and can timeout noisily.
static bool cst_read_reg(uint16_t reg, uint8_t *data, uint8_t len) {
    Wire.beginTransmission((uint8_t)I2C_ADDR_TOUCH);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(true) != 0) return false;
    delayMicroseconds(500);                       // chip needs a moment before the read
    if (Wire.requestFrom((uint8_t)I2C_ADDR_TOUCH, len) < len) return false;
    for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
    return true;
}

bool touch_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_BUS_HZ);
    Wire.setTimeOut(20);

    // hardware reset (low 10 ms, high 50 ms)
    pinMode(PIN_TP_RST, OUTPUT);
    digitalWrite(PIN_TP_RST, LOW);
    delay(10);
    digitalWrite(PIN_TP_RST, HIGH);
    delay(50);
    pinMode(PIN_TP_INT, INPUT);

    // comms check: a valid (even untouched) data read carries ACK 0xAB at byte 6
    uint8_t d[CST9217_DATA_LEN] = {0};
    for (int i = 0; i < 3; ++i) {
        if (cst_read_reg(CST9217_REG_DATA, d, CST9217_DATA_LEN) && d[6] == CST9217_ACK) {
            s_seenGoodRead = true;
            app_log_println("[touch] CST9217 responding (ACK ok)");
            return true;
        }
        delay(5);
    }
    s_quietUntilMs = millis() + 1500;
    app_log_println("[touch] CST9217 not responding yet (will keep polling)");
    return true;
}

bool touch_read(uint16_t *ox, uint16_t *oy) {
    if (millis() < s_quietUntilMs) {
        s_touchPoints = 0;
        return false;
    }
    uint8_t d[CST9217_DATA_LEN];
    if (!cst_read_reg(CST9217_REG_DATA, d, CST9217_DATA_LEN)) {
        s_touchPoints = 0;
        if (s_failStreak < 255) s_failStreak++;
        if (s_failStreak >= 3) s_quietUntilMs = millis() + (s_seenGoodRead ? 750UL : 2500UL);
        return false;
    }
    s_failStreak = 0;
    s_seenGoodRead = true;
    if (d[6] != CST9217_ACK) { s_touchPoints = 0; return false; }

    const uint8_t points = d[5] & 0x7F;
    s_touchPoints = points;
    if (points == 0) return false;
    if ((d[0] & 0x0F) != 0x06) { s_touchPoints = 0; return false; }       // status of point 0 must be "down"

    uint16_t x = ((uint16_t)d[1] << 4) | (d[3] >> 4);
    uint16_t y = ((uint16_t)d[2] << 4) | (d[3] & 0x0F);

    if (x > SCREEN_W - 1) x = SCREEN_W - 1;
    if (y > SCREEN_H - 1) y = SCREEN_H - 1;
    if (TP_MIRROR_X) x = (SCREEN_W - 1) - x;
    if (TP_MIRROR_Y) y = (SCREEN_H - 1) - y;

    *ox = x;
    *oy = y;
    return true;
}

uint8_t touch_points() { return s_touchPoints; }
