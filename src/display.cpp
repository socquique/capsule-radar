// M0 bring-up: CO5300 466x466 AMOLED via Arduino_GFX (QSPI) + LVGL.
// Pins come from config.h (confirmed against the Waveshare board definition and a
// working Arduino_GFX port for this exact panel). The panel runs off the always-on
// DC1 rail, so it lights up without configuring the AXP2101 PMIC.
// The actual UI is built by ui_boot_create() (shared with the native SDL sim).
#include "display.h"
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "touch_cst9217.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// --- Arduino_GFX panel -------------------------------------------------------
// Typed as Arduino_CO5300* (not Arduino_GFX*) so setBrightness() — declared on
// Arduino_OLED, not the GFX base — is reachable.
static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;

// --- LVGL plumbing -----------------------------------------------------------
#define LVGL_BUF_LINES 40    // partial draw-buffer height (lines); kept in fast internal RAM
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_color_t        *s_buf1 = nullptr;
static lv_color_t        *s_buf2 = nullptr;

static volatile uint32_t s_frameCount = 0;   // rendered frames (last-flush), for FPS measurement
uint32_t display_frames() { return s_frameCount; }

static volatile uint16_t s_rot = 0;          // clockwise display rotation, 0..359 degrees
static float      s_rotCos = 1.0f;
static float      s_rotSin = 0.0f;
static int32_t    s_rotCosQ16 = 65536;       // fixed-point values used in the per-pixel hot path
static int32_t    s_rotSinQ16 = 0;
static lv_color_t *s_rotBuf = nullptr;       // PSRAM scratch for rotated output (see begin())
static lv_color_t *s_frameBuf = nullptr;     // full logical framebuffer for arbitrary-angle sampling

static void draw_block(int16_t x, int16_t y, lv_color_t *pixels, uint16_t w, uint16_t h) {
#if (LV_COLOR_16_SWAP != 0)
    s_gfx->draw16bitBeRGBBitmap(x, y, (uint16_t *)pixels, w, h);
#else
    s_gfx->draw16bitRGBBitmap(x, y, (uint16_t *)pixels, w, h);
#endif
}

// Render the physical bounding box affected by a logical dirty rectangle. Sampling
// from the full logical framebuffer avoids gaps/overdraw artifacts that forward-mapping
// individual source pixels would create at non-cardinal angles.
static void flush_arbitrary(const lv_area_t *area) {
    const float cx = (SCREEN_W - 1) * 0.5f;
    const float cy = (SCREEN_H - 1) * 0.5f;
    const float xs[4] = {(float)area->x1, (float)area->x2, (float)area->x2, (float)area->x1};
    const float ys[4] = {(float)area->y1, (float)area->y1, (float)area->y2, (float)area->y2};
    float minX = (float)SCREEN_W, minY = (float)SCREEN_H, maxX = -1.0f, maxY = -1.0f;
    for (int i = 0; i < 4; ++i) {
        const float rx = xs[i] - cx;
        const float ry = ys[i] - cy;
        const float dx = cx + s_rotCos * rx - s_rotSin * ry;
        const float dy = cy + s_rotSin * rx + s_rotCos * ry;
        if (dx < minX) minX = dx;
        if (dx > maxX) maxX = dx;
        if (dy < minY) minY = dy;
        if (dy > maxY) maxY = dy;
    }

    // Include nearest-neighbour edge pixels and preserve the panel's required 2-pixel
    // alignment (even start, odd end) in both axes.
    int x1 = (int)floorf(minX) - 1;
    int y1 = (int)floorf(minY) - 1;
    int x2 = (int)ceilf(maxX) + 1;
    int y2 = (int)ceilf(maxY) + 1;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    x1 &= ~1;
    y1 &= ~1;
    x2 |= 1;
    y2 |= 1;
    if (x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    if (y2 >= SCREEN_H) y2 = SCREEN_H - 1;
    if (x1 > x2 || y1 > y2) return;

    const int outW = x2 - x1 + 1;
    const lv_color_t black = lv_color_black();
    const int64_t centerX2Q16 = (int64_t)(SCREEN_W - 1) * 65536;
    const int64_t centerY2Q16 = (int64_t)(SCREEN_H - 1) * 65536;
    const int32_t stepX = s_rotCosQ16 * 2;
    const int32_t stepY = -s_rotSinQ16 * 2;

    // Batch scanlines while keeping every physical write 2-pixel aligned. Fewer QSPI
    // transactions matter here because an arbitrary-angle dirty box can be fairly large.
    for (int dy = y1; dy <= y2; ) {
        int rows = y2 - dy + 1;
        if (rows > LVGL_BUF_LINES) rows = LVGL_BUF_LINES;
        rows &= ~1;
        for (int row = 0; row < rows; ++row) {
            const int py = dy + row;
            const int relX2 = 2 * x1 - (SCREEN_W - 1);
            const int relY2 = 2 * py - (SCREEN_H - 1);
            int64_t srcX2Q16 = centerX2Q16 + (int64_t)s_rotCosQ16 * relX2
                                                + (int64_t)s_rotSinQ16 * relY2;
            int64_t srcY2Q16 = centerY2Q16 - (int64_t)s_rotSinQ16 * relX2
                                                + (int64_t)s_rotCosQ16 * relY2;
            lv_color_t *out = s_rotBuf + row * outW;
            for (int dx = 0; dx < outW; ++dx) {
                const int sx = (int)((srcX2Q16 + 65536) >> 17);
                const int sy = (int)((srcY2Q16 + 65536) >> 17);
                out[dx] = (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H)
                            ? s_frameBuf[sy * SCREEN_W + sx]
                            : black;
                srcX2Q16 += stepX;
                srcY2Q16 += stepY;
            }
        }
        draw_block((int16_t)x1, (int16_t)dy, s_rotBuf, (uint16_t)outW, (uint16_t)rows);
        dy += rows;
    }
}

// LVGL -> panel, applying the chosen rotation while pushing.
//   0°   : straight through.
//   180° : reverse the flat block in place — no scratch buffer.
//   90°/270° : the block transposes (w<->h), so it can't be reversed in place; copy it
//              rotated into a PSRAM scratch buffer. That buffer MUST live in PSRAM — an
//              internal-RAM one starves the mbedTLS handshake and kills the ADS-B feed.
//   other: update the logical framebuffer and inverse-sample the rotated dirty bounds.
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const int w = (int)(area->x2 - area->x1 + 1);
    const int h = (int)(area->y2 - area->y1 + 1);
    const uint16_t angle = s_rot;
    const bool arbitrary = (angle != 0 && angle != 90 && angle != 180 && angle != 270);
    // Mirror into the logical framebuffer ONLY at non-cardinal angles (it's what
    // flush_arbitrary samples from). At 0/90/180/270 this copy would be pure overhead
    // on every frame for every user; skipping it keeps those paths exactly as before.
    // Switching TO an arbitrary angle invalidates the whole screen (see setRotation),
    // so the framebuffer is fully repopulated before it is ever sampled.
    if (arbitrary && s_frameBuf) {
        for (int row = 0; row < h; ++row) {
            memcpy(s_frameBuf + (area->y1 + row) * SCREEN_W + area->x1,
                   px + row * w, (size_t)w * sizeof(lv_color_t));
        }
    }

    if (arbitrary && s_frameBuf && s_rotBuf) {
        flush_arbitrary(area);
        if (lv_disp_flush_is_last(drv)) s_frameCount++;
        lv_disp_flush_ready(drv);
        return;
    }

    lv_color_t *out = px;
    int16_t  dx = area->x1, dy = area->y1;
    uint16_t dw = (uint16_t)w, dh = (uint16_t)h;

    switch (angle) {
        case 180:
            for (int i = 0, j = w * h - 1; i < j; ++i, --j) { lv_color_t t = px[i]; px[i] = px[j]; px[j] = t; }
            dx = (int16_t)(SCREEN_W - 1 - area->x2);
            dy = (int16_t)(SCREEN_H - 1 - area->y2);
            break;
        case 90:
            if (s_rotBuf) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        s_rotBuf[i * h + (h - 1 - j)] = px[j * w + i];
                out = s_rotBuf; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = (int16_t)(SCREEN_H - 1 - area->y2); dy = area->x1;
            }
            break;
        case 270:
            if (s_rotBuf) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        s_rotBuf[(w - 1 - i) * h + j] = px[j * w + i];
                out = s_rotBuf; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = area->y1; dy = (int16_t)(SCREEN_W - 1 - area->x2);
            }
            break;
        default: break;  // 0°
    }
    draw_block(dx, dy, out, dw, dh);
    if (lv_disp_flush_is_last(drv)) s_frameCount++;
    lv_disp_flush_ready(drv);
}

// CO5300 (QSPI) requires 2-pixel-aligned flush windows: even start, odd end.
// Without this, partial-area updates (e.g. the radar sweep) tear / ghost / flicker.
static void rounder_cb(lv_disp_drv_t *drv, lv_area_t *area) {
    (void)drv;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// CST9217 touch -> LVGL pointer. LVGL keeps the last point on release.
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        int lx = x, ly = y;                              // physical touch -> logical (inverse rotation)
        const uint16_t angle = s_rot;
        switch (angle) {
            case 90:  lx = y;                        ly = SCREEN_H - 1 - x; break;
            case 180: lx = SCREEN_W - 1 - x;         ly = SCREEN_H - 1 - y; break;
            case 270: lx = SCREEN_W - 1 - y;         ly = x; break;
            default:
                if (angle != 0) {
                    const int relX2 = 2 * (int)x - (SCREEN_W - 1);
                    const int relY2 = 2 * (int)y - (SCREEN_H - 1);
                    const int64_t sx2q = (int64_t)(SCREEN_W - 1) * 65536
                                       + (int64_t)s_rotCosQ16 * relX2
                                       + (int64_t)s_rotSinQ16 * relY2;
                    const int64_t sy2q = (int64_t)(SCREEN_H - 1) * 65536
                                       - (int64_t)s_rotSinQ16 * relX2
                                       + (int64_t)s_rotCosQ16 * relY2;
                    lx = (int)((sx2q + 65536) >> 17);
                    ly = (int)((sy2q + 65536) >> 17);
                }
                break;
        }
        if (lx < 0 || lx >= SCREEN_W || ly < 0 || ly >= SCREEN_H) {
            data->state = LV_INDEV_STATE_RELEASED;        // black corners are outside the logical UI
            return;
        }
        data->point.x = (lv_coord_t)lx;
        data->point.y = (lv_coord_t)ly;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

namespace display {

bool begin() {
    Serial.println("[display] init CO5300 QSPI...");
    s_bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK,
                                  PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RST, 0 /*rotation*/,
                               SCREEN_W, SCREEN_H,
                               LCD_COL_OFFSET, LCD_ROW_OFFSET, 0, 0);
    if (!s_gfx->begin(LCD_QSPI_HZ)) {
        Serial.println("[display] gfx->begin() FAILED");
        return false;
    }
    s_gfx->fillScreen(RGB565_BLACK);
    s_gfx->setBrightness(BRIGHTNESS_DEFAULT);
    Serial.println("[display] panel up; init LVGL...");

    lv_init();

    // Draw scratch in INTERNAL DMA RAM: rendering anti-aliased graphics into PSRAM is
    // slow (that, not QSPI bandwidth, was the bottleneck). Keep the active buffer in fast
    // internal SRAM; single partial buffer to stay within the internal-RAM budget.
    const size_t buf_px = (size_t)SCREEN_W * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = nullptr;
    if (!s_buf1) {
        Serial.println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_px);

    // Rotation buffers live in PSRAM so the internal contiguous block needed by TLS remains
    // available. The full logical framebuffer makes inverse sampling at arbitrary angles
    // possible without holes; the smaller scratch holds transposed blocks or output batches.
    s_rotBuf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_frameBuf = (lv_color_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H,
                                                sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_rotBuf || !s_frameBuf) {
        Serial.println("[display] WARNING: rotation buffer allocation failed; arbitrary angles unavailable");
    }

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = SCREEN_W;
    s_disp_drv.ver_res  = SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.rounder_cb = rounder_cb;     // CO5300 needs 2-px-aligned windows
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    // Touch input (CST9217) -> LVGL pointer indev (drives tap-to-inspect + swipe).
    if (touch_begin()) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_indev_drv);
        Serial.println("[display] CST9217 touch registered");
    }

    Serial.printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();                   // M3: radar/list/stats views + tap-to-inspect
    Serial.println("[display] LVGL ready");
    return true;
}

void loop() { lv_timer_handler(); }

void setBrightness(uint8_t v) { if (s_gfx) s_gfx->setBrightness(v); }

void setRotation(uint16_t degrees) {
    uint16_t normalized = (uint16_t)(degrees % 360);
    if ((normalized == 90 || normalized == 270) && !s_rotBuf) {
        Serial.println("[display] quarter-turn rotation unavailable without the PSRAM scratch buffer");
        normalized = 0;
    }
    if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270
        && (!s_frameBuf || !s_rotBuf)) {
        Serial.println("[display] arbitrary rotation unavailable without both PSRAM buffers");
        normalized = 0;
    }
    if (normalized == s_rot) return;
    const float radians = normalized * ((float)M_PI / 180.0f);
    s_rotCos = cosf(radians);
    s_rotSin = sinf(radians);
    s_rotCosQ16 = (int32_t)lroundf(s_rotCos * 65536.0f);
    s_rotSinQ16 = (int32_t)lroundf(s_rotSin * 65536.0f);
    s_rot = normalized;
    if (s_gfx) s_gfx->fillScreen(RGB565_BLACK);  // clear pixels no longer covered after an angle change
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);   // full repaint in the new orientation
}
uint16_t rotation() { return s_rot; }

uint32_t inactiveMs() { return lv_disp_get_inactive_time(NULL); }

} // namespace display
