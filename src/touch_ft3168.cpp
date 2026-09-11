// FT3168 capacitive touch over I2C (Arduino) — Waveshare ESP32-S3-Touch-AMOLED-1.43.
// Standard FocalTech register layout: one burst read from 0x00 gives the touch count
// and the 12-bit X/Y of the first point. Single-touch is all the radar UI needs.
//
// This chip has no reset line of its own on this board — it is wired to the panel
// reset — so it only answers on I2C once the display has been initialised. That is
// why display::begin() must call gfx->begin() before touch_begin(); with the stock
// 1.75 firmware's pin map nothing on the bus answered at all.
#include "touch.h"
#include "config.h"

#if TOUCH_DRIVER_FT3168
#include <Arduino.h>
#include <Wire.h>

#define FT_REG_DATA     0x00       // burst: [mode][gest][count][xh][xl][yh][yl]
#define FT_REG_CHIPID   0xA3       // reads 0x64 on the FT3168
#define FT_DATA_LEN     7
#define FT_CHIPID_FT3168 0x64

#define FT_EVT_LIFT_UP  1          // top 2 bits of the xh byte

static bool ft_read_reg(uint8_t reg, uint8_t *data, uint8_t len) {
    Wire.beginTransmission((uint8_t)I2C_ADDR_TOUCH);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;   // repeated START
    if (Wire.requestFrom((uint8_t)I2C_ADDR_TOUCH, len) < len) return false;
    for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
    return true;
}

bool touch_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

    uint8_t id[1] = {0};
    if (ft_read_reg(FT_REG_CHIPID, id, 1) && id[0] == FT_CHIPID_FT3168) {
        Serial.println("[touch] FT3168 responding (chip id 0x64)");
    } else {
        // Not fatal: the panel reset may still be settling. touch_read() retries forever.
        Serial.printf("[touch] FT3168 not responding yet (id=0x%02X, will keep polling)\n", id[0]);
    }
    return true;
}

bool touch_read(uint16_t *ox, uint16_t *oy) {
    uint8_t d[FT_DATA_LEN];
    if (!ft_read_reg(FT_REG_DATA, d, FT_DATA_LEN)) return false;

    const uint8_t points = d[2] & 0x0F;
    if (points == 0 || points > 5) return false;          // 0x0F = no valid data
    if ((d[3] >> 6) == FT_EVT_LIFT_UP) return false;      // finger leaving, treat as released

    uint16_t x = (uint16_t)((d[3] & 0x0F) << 8 | d[4]);
    uint16_t y = (uint16_t)((d[5] & 0x0F) << 8 | d[6]);

    if (x > SCREEN_W - 1) x = SCREEN_W - 1;
    if (y > SCREEN_H - 1) y = SCREEN_H - 1;
    if (TP_MIRROR_X) x = (SCREEN_W - 1) - x;
    if (TP_MIRROR_Y) y = (SCREEN_H - 1) - y;

    *ox = x;
    *oy = y;
    return true;
}

#endif  // TOUCH_DRIVER_FT3168
