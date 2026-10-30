// Arduino_GFX stub for host tests of src/panel_scale.h: a panel that keeps what it is sent.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>

class Arduino_GFX {
public:
    Arduino_GFX(int w, int h) : w(w), h(h), px((size_t)w * h, 0) {}
    void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t *bmp, int16_t bw, int16_t bh) {
        for (int r = 0; r < bh; ++r) memcpy(&px[(size_t)(y + r) * w + x], bmp + (size_t)r * bw, (size_t)bw * 2);
    }
    int w, h;
    std::vector<uint16_t> px;
};
