// QMI8658 accelerometer over I2C (Arduino). Shares the bus with the touch (SDA/SCL
// in config.h). We only need gravity on the Z axis to detect a face-down board.
#include "imu_qmi8658.h"
#include "config.h"
#include "app_log.h"
#include <Arduino.h>
#include <Wire.h>

#define QMI_WHOAMI  0x00   // -> 0x05
#define QMI_CTRL1   0x02   // serial interface / addr auto-increment
#define QMI_CTRL2   0x03   // accel: full-scale + ODR
#define QMI_CTRL7   0x08   // sensor enable
#define QMI_AZ_L    0x39   // accel Z low byte (high byte at 0x3A)

// ±2g full scale -> 16384 LSB/g. On this board the IMU Z is inverted, so face-down
// is strongly POSITIVE Z.
#define FACEDOWN_THRESHOLD (9800)   // ~ +0.6 g

static uint8_t s_addr = 0x6B;
static bool    s_ok   = false;

static bool wr(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t *buf, uint8_t len) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        Wire.beginTransmission(s_addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(s_addr, len) >= len) {
            for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
            return true;
        }
        delayMicroseconds(500);
    }
    return false;
}

bool imu_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_BUS_HZ);   // idempotent (touch also begins it)
    Wire.setTimeOut(20);
    const uint8_t addrs[2] = { 0x6B, 0x6A };
    for (uint8_t i = 0; i < 2; ++i) {
        s_addr = addrs[i];
        uint8_t who = 0;
        if (rd(QMI_WHOAMI, &who, 1) && who == 0x05) {
            wr(QMI_CTRL1, 0x40);   // address auto-increment
            wr(QMI_CTRL2, 0x06);   // accel: ±2 g, ~125 Hz
            wr(QMI_CTRL7, 0x01);   // enable accelerometer
            app_log_printf("[imu] QMI8658 ready at 0x%02X\n", s_addr);
            s_ok = true;
            return true;
        }
    }
    app_log_println("[imu] QMI8658 not found");
    s_ok = false;
    return false;
}

bool imu_facedown() {
    if (!s_ok) return false;
    uint8_t b[2];
    if (!rd(QMI_AZ_L, b, 2)) return false;
    const int16_t az = (int16_t)((b[1] << 8) | b[0]);
    return az > FACEDOWN_THRESHOLD;
}
