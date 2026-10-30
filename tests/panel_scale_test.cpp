// UI -> smaller panel resampler (src/panel_scale.h). The UI reaches the panel in blocks of
// any size and order (LVGL strips, sweep boxes, touch-sized redraws). Whatever the blocks,
// the panel must end up as if the whole frame had been resampled at once: a panel pixel
// left stale at a block edge is a visible seam.
#include "panel_scale.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void push_frame(PanelScaler &s, const std::vector<uint16_t> &ui, int x, int y, int w, int h) {
    std::vector<uint16_t> blk((size_t)w * h);
    for (int r = 0; r < h; ++r) memcpy(&blk[(size_t)r * w], &ui[(size_t)(y + r) * SCREEN_W + x], (size_t)w * 2);
    s.push(x, y, blk.data(), w, h);
}

int main() {
    srand(1);
    std::vector<uint16_t> ui((size_t)SCREEN_W * SCREEN_H);
    for (auto &p : ui) p = (uint16_t)rand();

    Arduino_GFX whole(PANEL_W, PANEL_H), blocks(PANEL_W, PANEL_H);
    PanelScaler a, b;
    assert(a.begin(&whole) && b.begin(&blocks));
    push_frame(a, ui, 0, 0, SCREEN_W, SCREEN_H);

    // Same frame as random blocks over a stale (all-white) panel copy, then every pixel once more
    // in 1-px columns and rows at random places: any panel pixel a block misses keeps old data.
    std::vector<uint16_t> white(ui.size(), 0xFFFF);
    push_frame(b, white, 0, 0, SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y += 40) push_frame(b, ui, 0, y, SCREEN_W, std::min(40, SCREEN_H - y));
    for (int i = 0; i < 3000; ++i) {
        const int w = 1 + rand() % 60, h = 1 + rand() % 60;
        const int x = rand() % (SCREEN_W - w + 1), y = rand() % (SCREEN_H - h + 1);
        for (int r = y; r < y + h; ++r)
            for (int c = x; c < x + w; ++c) ui[(size_t)r * SCREEN_W + c] = (uint16_t)rand();
        push_frame(b, ui, x, y, w, h);
    }
    push_frame(a, ui, 0, 0, SCREEN_W, SCREEN_H);
    assert(whole.px == blocks.px && "block writes match a whole-frame resample");

    // A flat colour stays that colour (no channel bleed in the packed blend).
    const uint16_t flat[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x8410 };
    for (uint16_t c : flat) {
        std::vector<uint16_t> f(ui.size(), c);
        push_frame(a, f, 0, 0, SCREEN_W, SCREEN_H);
        for (uint16_t p : whole.px) assert(p == c && "flat colour survives resampling");
    }
    puts("panel_scale: OK");
    return 0;
}
