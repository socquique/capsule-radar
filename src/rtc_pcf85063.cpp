#include "rtc_pcf85063.h"
#include "config.h"
#include "app_log.h"
#include <Arduino.h>
#include <Wire.h>

// PCF85063 register map (time block starts at 0x04):
#define REG_SECONDS 0x04   // bit7 = VL: 1 => oscillator stopped, time not guaranteed
// 0x05 minutes, 0x06 hours(24h), 0x07 days, 0x08 weekday, 0x09 months, 0x0A years(00-99, base 2000)

static bool s_ok = false;

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

bool rtc_begin() {
    Wire.setTimeOut(20);
    Wire.beginTransmission(I2C_ADDR_RTC);
    s_ok = (Wire.endTransmission() == 0);
    app_log_printf("[rtc] PCF85063 %s\n", s_ok ? "ready" : "not found");
    return s_ok;
}

bool rtc_present() { return s_ok; }

bool rtc_read(struct tm *out) {
    if (!s_ok || !out) return false;
    bool readOk = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Wire.beginTransmission(I2C_ADDR_RTC);
        Wire.write(REG_SECONDS);
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)I2C_ADDR_RTC, 7) == 7) {
            readOk = true;
            break;
        }
        delayMicroseconds(500);
    }
    if (!readOk) return false;
    const uint8_t s  = Wire.read();
    const uint8_t mi = Wire.read();
    const uint8_t h  = Wire.read();
    const uint8_t d  = Wire.read();
    (void)Wire.read();                                           // weekday (unused)
    const uint8_t mo = Wire.read();
    const uint8_t y  = Wire.read();
    if (s & 0x80) return false;                                  // VL set => invalid
    out->tm_sec   = bcd2dec(s  & 0x7F);
    out->tm_min   = bcd2dec(mi & 0x7F);
    out->tm_hour  = bcd2dec(h  & 0x3F);
    out->tm_mday  = bcd2dec(d  & 0x3F);
    out->tm_mon   = bcd2dec(mo & 0x1F) - 1;
    out->tm_year  = bcd2dec(y) + 100;                            // tm_year = years since 1900
    out->tm_isdst = 0;
    return out->tm_year >= 124;                                  // < 2024 => never set
}

bool rtc_write(const struct tm *t) {
    if (!s_ok || !t) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Wire.beginTransmission(I2C_ADDR_RTC);
        Wire.write(REG_SECONDS);
        Wire.write(dec2bcd((uint8_t)t->tm_sec) & 0x7F);              // also clears VL
        Wire.write(dec2bcd((uint8_t)t->tm_min));
        Wire.write(dec2bcd((uint8_t)t->tm_hour));
        Wire.write(dec2bcd((uint8_t)t->tm_mday));
        Wire.write(0);                                               // weekday (unused)
        Wire.write(dec2bcd((uint8_t)(t->tm_mon + 1)));
        Wire.write(dec2bcd((uint8_t)(t->tm_year - 100)));
        if (Wire.endTransmission() == 0) return true;
        delayMicroseconds(500);
    }
    return false;
}
