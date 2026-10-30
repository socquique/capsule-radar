#pragma once
// Fits the UI onto a smaller square panel (the 1.85C V2: a 466x466 UI on a 360x360 LCD).
// Callers keep working in UI pixels. Each write also lands in a copy of the whole UI
// frame, and the panel pixels it touches are resampled from that copy (bilinear,
// weights in 1/32), so a write that only partly covers a panel pixel's 2x2 source still
// blends with the pixels around it. Device-only; display.cpp is the one user.
#include "config.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <algorithm>

static_assert(PANEL_W == PANEL_H && SCREEN_W == SCREEN_H, "the resampler assumes square frames");

class PanelScaler {
public:
    bool begin(Arduino_GFX *gfx) {
        _gfx = gfx;
        _ui  = (uint16_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H, 2, MALLOC_CAP_SPIRAM);
        _out = (uint16_t *)heap_caps_malloc((size_t)PANEL_W * ROWS * 2, MALLOC_CAP_SPIRAM);
        for (int i = 0; i < PANEL_W; ++i) {
            const float s = (i + 0.5f) * SCREEN_W / PANEL_W - 0.5f;
            int s0 = (int)floorf(s);
            if (s0 > SCREEN_W - 2) s0 = SCREEN_W - 2;
            _src0[i] = (uint16_t)s0;
            _srcW[i] = (uint8_t)lroundf((s - s0) * 32.0f);
        }
        return _ui && _out;
    }

    // Native-order RGB565 (LV_COLOR_16_SWAP 0), UI coordinates.
    void push(int16_t x, int16_t y, const uint16_t *px, uint16_t w, uint16_t h) {
        for (int row = 0; row < h; ++row)
            memcpy(_ui + (size_t)(y + row) * SCREEN_W + x, px + (size_t)row * w, (size_t)w * 2);
        int px0, px1, py0, py1;
        if (!span(x, x + w - 1, &px0, &px1) || !span(y, y + h - 1, &py0, &py1)) return;
        const int pw = px1 - px0 + 1;
        for (int py = py0; py <= py1; py += ROWS) {
            const int rows = std::min(ROWS, py1 - py + 1);
            for (int r = 0; r < rows; ++r) {
                const uint16_t *top = _ui + (size_t)_src0[py + r] * SCREEN_W;
                const uint16_t *bot = top + SCREEN_W;
                const uint32_t ty = _srcW[py + r];
                uint16_t *out = _out + (size_t)r * pw;
                for (int i = 0; i < pw; ++i) {
                    const int sx = _src0[px0 + i];
                    const uint32_t tx = _srcW[px0 + i];
                    const uint32_t m = lerp(lerp(spread(top[sx]), spread(top[sx + 1]), tx),
                                            lerp(spread(bot[sx]), spread(bot[sx + 1]), tx), ty);
                    out[i] = (uint16_t)(m | (m >> 16));
                }
            }
            _gfx->draw16bitRGBBitmap((int16_t)px0, (int16_t)py, _out, (uint16_t)pw, (uint16_t)rows);
        }
    }

private:
    static constexpr int ROWS = 16;      // panel rows per write

    // RGB565 spread over 32 bits (G in the high half) so one multiply blends all three channels.
    static uint32_t spread(uint16_t c) { return (c | ((uint32_t)c << 16)) & 0x07E0F81F; }
    static uint32_t lerp(uint32_t a, uint32_t b, uint32_t t) {
        return ((a * (32 - t) + b * t) >> 5) & 0x07E0F81F;
    }

    // The panel pixels [*p0, *p1] whose samples read any UI pixel in [v0, v1].
    bool span(int v0, int v1, int *p0, int *p1) const {
        int a = 0, b = PANEL_W - 1;
        while (a < PANEL_W && _src0[a] + 1 < v0) ++a;
        while (b >= 0 && _src0[b] > v1) --b;
        *p0 = a;
        *p1 = b;
        return a <= b;
    }

    Arduino_GFX *_gfx = nullptr;
    uint16_t *_ui  = nullptr;            // SCREEN_W x SCREEN_H (PSRAM)
    uint16_t *_out = nullptr;            // PANEL_W x ROWS (PSRAM)
    uint16_t _src0[PANEL_W];             // panel pixel i samples UI pixels _src0[i] and +1,
    uint8_t  _srcW[PANEL_W];             // the second with weight _srcW[i] / 32
};
