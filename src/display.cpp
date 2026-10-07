// M0 bring-up: CO5300 466x466 AMOLED via Arduino_GFX (QSPI) + LVGL.
// Pins, panel gaps and the touch driver all come from the board header selected in
// config.h (see src/boards/). On boards with an AXP2101 the panel runs off the
// always-on DC1 rail, so it lights up without configuring the PMIC first.
// NOTE: gfx->begin() must stay ahead of touch_begin() — on the 1.43 the touch
// controller's reset is tied to the panel reset and it is absent from I2C until then.
// The actual UI is built by ui_boot_create() (shared with the native SDL sim).
#include "display.h"
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "touch.h"

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

static void panel_push(int16_t x, int16_t y, uint16_t *pixels, uint16_t w, uint16_t h) {
#if (LV_COLOR_16_SWAP != 0)
    s_gfx->draw16bitBeRGBBitmap(x, y, pixels, w, h);
#else
    s_gfx->draw16bitRGBBitmap(x, y, pixels, w, h);
#endif
}

// --- sweep compositor ----------------------------------------------------------------
// Idea and blend from ijord's 2.8C fork (46e0822e, upstream issue #14), re-written for
// LVGL 8 and this QSPI panel, which keeps its own GRAM (no scanout framebuffer).
// LVGL renders the scope WITHOUT the sweep. Every block pushed to the panel is also kept
// in s_phys (physical orientation, so any rotation works). Each sweep tick re-pushes only
// the box under the old + new wedge: s_phys plus a per-pixel angular fade. LVGL output
// that lands under the wedge between ticks is blended the same way before it is pushed.
// Pixels are LVGL's RGB565 (LV_COLOR_16_SWAP 0); a compile-time check is below.
#define ANG_UNITS      32768                     // 360 degrees in bearing-map units
#define ANG_OUTSIDE    0xFFFF                    // bearing-map value outside the scope disc
#define TRAIL_U        ((int)(SWEEP_TRAIL_DEG / 360.0f * ANG_UNITS))
#define FADE_SHIFT     3
#define FADE_N         ((TRAIL_U >> FADE_SHIFT) + 2)
#define COMP_ROWS      32                        // rows per push from the scratch buffer
#define SWEEP_MASKS    4
static_assert(LV_COLOR_16_SWAP == 0, "compositor assumes native-order RGB565");

struct Rect { int x1, y1, x2, y2; bool valid; };
static uint16_t *s_phys    = nullptr;   // last pushed LVGL content, physical orientation (PSRAM)
static uint16_t *s_angMap  = nullptr;   // per-pixel physical bearing, 0 = up, clockwise (PSRAM)
static uint16_t *s_compBuf = nullptr;   // push scratch, SCREEN_W x COMP_ROWS (PSRAM)
static uint8_t   s_fadeLut[FADE_N];
static uint16_t  s_swRing = 0x07E0, s_swLead = 0xFFFF;
static bool      s_swWanted  = false;   // radar_view: sweep on and the theme has one
static bool      s_swVisible = false;   // UI: radar tile shown, not mid-swipe
static bool      s_wedgeOn   = false;   // a wedge is currently on the panel at s_angU
static int       s_angU      = 0;
static float     s_pcx = 0, s_pcy = 0;  // scope centre, physical pixels
static float     s_leadUx = 0, s_leadUy = -1;   // unit vector along the lead line at s_angU
static Rect      s_wedge     = {};      // physical box of the wedge currently on the panel
// Objects the sweep must not paint over: the logical rounded rectangle (exact shape) and
// its physical bounding box (cheap first test).
struct SweepMask { Rect box; float x1, y1, x2, y2, r; };
static SweepMask s_mask[SWEEP_MASKS];
static int       s_maskN     = 0;
static uint32_t  s_lastComposeMs = 0;
static uint32_t  s_swPhase = 0;         // sweep angle in ANG_UNITS << 8 (sub-unit precision)
#define SWEEP_MAX_STEP_MS (2 * SWEEP_FRAME_MS)   // after a stall, resume instead of jumping ahead
// Centre ripple (the expanding ring at home), drawn here too so it moves at the sweep's rate.
static bool      s_pulseWanted = false; // radar_view: the theme has one
static bool      s_pulseOn   = false;   // a ring is currently on the panel
static uint16_t  s_pulseInk  = 0xFFFF;
static uint32_t  s_pulseMs   = 0;       // position in the expansion, 0..PULSE_PERIOD_MS
static float     s_pulseR    = 0;       // outer radius of the ring on the panel
static uint32_t  s_pulseOpa  = 0;
static Rect      s_pulseBox  = {};      // physical box of the ring currently on the panel


static inline void rect_grow(Rect &r, const Rect &o) {
    if (!o.valid) return;
    if (!r.valid) { r = o; return; }
    r.x1 = min(r.x1, o.x1); r.y1 = min(r.y1, o.y1);
    r.x2 = max(r.x2, o.x2); r.y2 = max(r.y2, o.y2);
}

// Logical point -> physical, the same mapping the flush paths use (rotation about the
// panel centre, (W-1)/2).
static inline void log_to_phys(float lx, float ly, float *px, float *py) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float rx = lx - c, ry = ly - c;
    *px = c + s_rotCos * rx - s_rotSin * ry;
    *py = c + s_rotSin * rx + s_rotCos * ry;
}

static void sweep_build_map() {
    if (!s_angMap) return;
    float pcx, pcy;
    log_to_phys((float)SCREEN_CX, (float)SCREEN_CY, &pcx, &pcy);
    s_pcx = pcx; s_pcy = pcy;
    const float rMax2 = (float)(RADAR_R_OUTER_PX + 2) * (float)(RADAR_R_OUTER_PX + 2);
    for (int y = 0; y < SCREEN_H; ++y) {
        uint16_t *row = s_angMap + (size_t)y * SCREEN_W;
        const float dy = (float)y - pcy;
        for (int x = 0; x < SCREEN_W; ++x) {
            const float dx = (float)x - pcx;
            if (dx * dx + dy * dy > rMax2) { row[x] = ANG_OUTSIDE; continue; }
            float a = atan2f(dx, -dy);                       // 0 = up, clockwise
            if (a < 0) a += 2.0f * (float)M_PI;
            int u = (int)(a * ((float)ANG_UNITS / (2.0f * (float)M_PI)));
            row[x] = (uint16_t)(u & (ANG_UNITS - 1));
        }
    }
}

static void sweep_build_lut() {
    for (int i = 0; i < FADE_N; ++i) {
        const float delta = (float)((i << FADE_SHIFT) + (1 << (FADE_SHIFT - 1)));
        float frac = 1.0f - delta / (float)TRAIL_U;          // 1 at the lead edge -> 0 at the tail
        if (frac < 0) frac = 0;
        s_fadeLut[i] = (uint8_t)(frac * frac * (float)SWEEP_TRAIL_OPA);
    }
}

// Blend `tint` over `dst` at `opa` (0..255), ordered-dithered back to RGB565 so the
// fade has no visible banding (ijord's blend565_dither, with a /256 approximation).
static const uint8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint16_t blend565(uint16_t dst, uint16_t tint, uint32_t opa, uint32_t d) {
    const uint32_t ia = 256 - opa;
    uint32_t dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    uint32_t tr = (tint >> 11) & 0x1F, tg = (tint >> 5) & 0x3F, tb = tint & 0x1F;
    dr = (dr << 3) | (dr >> 2); dg = (dg << 2) | (dg >> 4); db = (db << 3) | (db >> 2);
    tr = (tr << 3) | (tr >> 2); tg = (tg << 2) | (tg >> 4); tb = (tb << 3) | (tb >> 2);
    uint32_t r = ((tr * opa + dr * ia) >> 8) + (d >> 1);
    uint32_t g = ((tg * opa + dg * ia) >> 8) + (d >> 2);
    uint32_t b = ((tb * opa + db * ia) >> 8) + (d >> 1);
    r >>= 3; g >>= 2; b >>= 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Is physical pixel (x,y) inside the mask's rounded rectangle? Tested in logical
// coordinates (inverse rotation), so the shape is exact at any display angle.
static bool mask_hit(const SweepMask &m, int x, int y) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float rx = (float)x - c, ry = (float)y - c;
    const float lx = c + s_rotCos * rx + s_rotSin * ry;
    const float ly = c - s_rotSin * rx + s_rotCos * ry;
    const float cx = lx < m.x1 + m.r ? m.x1 + m.r : (lx > m.x2 - m.r ? m.x2 - m.r : lx);
    const float cy = ly < m.y1 + m.r ? m.y1 + m.r : (ly > m.y2 - m.r ? m.y2 - m.r : ly);
    const float dx = lx - cx, dy = ly - cy, rr = m.r + 0.5f;    // +0.5: keep the anti-aliased edge too
    return dx * dx + dy * dy <= rr * rr;
}

// Blend the wedge (at s_angU) into `buf`, which holds physical pixels starting at (bx,by)
// with row stride `stride`, limited to the physical box `r`.
static void sweep_blend(uint16_t *buf, int bx, int by, int stride, Rect r) {
    // The lead is a SWEEP_LEAD_W pixel line from the centre to the rim, drawn by distance
    // from the line (anti-aliased, round ends), not as an angular slice: a slice is under a
    // pixel wide near the centre, which made the lead look dotted there and shimmer as it
    // turned. The trail stays angular, since it fades smoothly anyway.
    const float hw = SWEEP_LEAD_W * 0.5f, reach = hw + 0.5f;
    const float ux = s_leadUx, uy = s_leadUy, R = (float)RADAR_R_OUTER_PX;
    for (int y = r.y1; y <= r.y2; ++y) {
        // per-row mask intervals
        const SweepMask *rowMask[SWEEP_MASKS];
        int mn = 0;
        for (int i = 0; i < s_maskN; ++i)
            if (y >= s_mask[i].box.y1 && y <= s_mask[i].box.y2) rowMask[mn++] = &s_mask[i];
        const uint16_t *am = s_angMap + (size_t)y * SCREEN_W;
        uint16_t *row = buf + (size_t)(y - by) * stride - bx;
        const uint8_t *bay = &BAYER4[(y & 3) << 2];
        // Columns of this row within `reach` of the lead's line: (x - cx)*uy - fy*ux = +-reach.
        const float fy = (float)y - s_pcy;
        int lx1 = r.x1, lx2 = r.x2;
        if (fabsf(uy) > 1e-4f) {
            float a = s_pcx + (fy * ux - reach) / uy, b = s_pcx + (fy * ux + reach) / uy;
            if (a > b) { const float t = a; a = b; b = t; }
            lx1 = max(lx1, (int)floorf(a)); lx2 = min(lx2, (int)ceilf(b));
        } else if (fabsf(fy * ux) >= reach) {
            lx1 = 1; lx2 = 0;                                    // horizontal lead, this row is off it
        }
        for (int x = r.x1; x <= r.x2; ++x) {
            const uint16_t m = am[x];
            if (m == ANG_OUTSIDE) continue;
            const int delta = (s_angU - (int)m) & (ANG_UNITS - 1);
            const bool nearLead = x >= lx1 && x <= lx2;
            if (delta >= TRAIL_U && !nearLead) continue;
            if (mn) {
                bool masked = false;
                for (int i = 0; i < mn; ++i)
                    if (x >= rowMask[i]->box.x1 && x <= rowMask[i]->box.x2 && mask_hit(*rowMask[i], x, y)) { masked = true; break; }
                if (masked) continue;
            }
            if (delta < TRAIL_U) {
                const uint8_t opa = s_fadeLut[delta >> FADE_SHIFT];
                if (opa >= 2) row[x] = blend565(row[x], s_swRing, opa, bay[x & 3]);
            }
            if (nearLead) {
                // distance to the segment centre..rim (clamping gives the round ends)
                const float fx = (float)x - s_pcx;
                float t = fx * ux + fy * uy;
                t = t < 0 ? 0 : (t > R ? R : t);
                const float ex = fx - t * ux, ey = fy - t * uy;
                const float cov = reach - sqrtf(ex * ex + ey * ey);
                if (cov > 0) {
                    const uint32_t opa = (uint32_t)((cov > 1 ? 1 : cov) * SWEEP_LEAD_OPA);
                    if (opa >= 2) row[x] = blend565(row[x], s_swLead, opa, bay[x & 3]);
                }
            }
        }
    }
}

// Blend the centre ring into `buf` (same layout as sweep_blend), limited to `r`. It is
// centred on the panel centre, which every rotation leaves in place, as LVGL centred it.
static void pulse_blend(uint16_t *buf, int bx, int by, int stride, Rect r) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float ro = s_pulseR, ri = ro - PULSE_W;
    for (int y = r.y1; y <= r.y2; ++y) {
        uint16_t *row = buf + (size_t)(y - by) * stride - bx;
        const uint8_t *bay = &BAYER4[(y & 3) << 2];
        const float fy = (float)y - c;
        // Only the columns within the ring's outer edge and outside its hole need the
        // per-pixel distance: two spans per row, found with two square roots.
        const float o2 = (ro + 0.5f) * (ro + 0.5f) - fy * fy;
        if (o2 <= 0) continue;
        const float ow = sqrtf(o2);
        const float i2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) - fy * fy : 0;
        const float iw = i2 > 0 ? sqrtf(i2) : -1;
        const int xa = max(r.x1, (int)floorf(c - ow)), xd = min(r.x2, (int)ceilf(c + ow));
        const int xb = iw < 0 ? xd : (int)ceilf(c - iw), xc = iw < 0 ? xd + 1 : (int)floorf(c + iw);
        for (int x = xa; x <= xd; ++x) {
            if (x > xb && x < xc) { x = xc - 1; continue; }   // jump over the hole
            const float fx = (float)x - c;
            const float d = sqrtf(fx * fx + fy * fy);
            float cov = min(d - ri, ro - d) + 0.5f;      // anti-aliased inner and outer edges
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            if (d < 4.5f) cov *= max(d - 3.5f, 0.0f);    // stay under the centre dot (7 px)
            const uint32_t opa = (uint32_t)(cov * (float)s_pulseOpa);
            if (opa < 2) continue;
            bool masked = false;
            for (int i = 0; i < s_maskN && !masked; ++i) {
                const Rect &b = s_mask[i].box;
                masked = x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2 && mask_hit(s_mask[i], x, y);
            }
            if (!masked) row[x] = blend565(row[x], s_pulseInk, opa, bay[x & 3]);
        }
    }
}

static Rect pulse_box(float ro) {
    const int c = (SCREEN_W - 1) / 2, e = (int)ceilf(ro) + 2;
    Rect r = { (c - e) & ~1, (c - e) & ~1, (c + e + 1) | 1, (c + e + 1) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    return r;
}

// Physical box of the wedge at angle `u`, padded and aligned for the CO5300 (even start,
// odd end).
static Rect wedge_box(int u) {
    float pcx, pcy;
    log_to_phys((float)SCREEN_CX, (float)SCREEN_CY, &pcx, &pcy);
    float minX = pcx, maxX = pcx, minY = pcy, maxY = pcy;
    const float toRad = 2.0f * (float)M_PI / (float)ANG_UNITS;
    for (int i = 0; i <= 12; ++i) {
        const float a = (float)(u - TRAIL_U * i / 12) * toRad;   // the 3 px pad covers the lead's far half
        const float x = pcx + (RADAR_R_OUTER_PX + 2) * sinf(a);
        const float y = pcy - (RADAR_R_OUTER_PX + 2) * cosf(a);
        minX = min(minX, x); maxX = max(maxX, x);
        minY = min(minY, y); maxY = max(maxY, y);
    }
    Rect r = { ((int)minX - 3) & ~1, ((int)minY - 3) & ~1, ((int)maxX + 3) | 1, ((int)maxY + 3) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    return r;
}

static inline Rect rect_and(const Rect &a, const Rect &b) {
    Rect r = { max(a.x1, b.x1), max(a.y1, b.y1), min(a.x2, b.x2), min(a.y2, b.y2), a.valid && b.valid };
    if (r.x1 > r.x2 || r.y1 > r.y2) r.valid = false;
    return r;
}

// Every panel write goes through here: remember it in s_phys, blend the wedge into the
// part that lies under it, then push.
static void draw_block(int16_t x, int16_t y, lv_color_t *pixels, uint16_t w, uint16_t h) {
    uint16_t *px = (uint16_t *)pixels;
    if (s_phys) {
        for (int row = 0; row < h; ++row)
            memcpy(s_phys + (size_t)(y + row) * SCREEN_W + x, px + (size_t)row * w, (size_t)w * 2);
        if (s_wedgeOn) {
            const Rect blk = { x, y, x + w - 1, y + h - 1, true };
            const Rect r = rect_and(blk, s_wedge);
            if (r.valid) sweep_blend(px, x, y, w, r);
        }
        if (s_pulseOn) {
            const Rect blk = { x, y, x + w - 1, y + h - 1, true };
            const Rect r = rect_and(blk, s_pulseBox);
            if (r.valid) pulse_blend(px, x, y, w, r);
        }
    }
    panel_push(x, y, px, w, h);
}

// Re-push a physical box from s_phys, with the wedge blended in if one is on.
static void push_region(Rect reg) {
    if (!reg.valid) return;
    const int w = reg.x2 - reg.x1 + 1;
    for (int y = reg.y1; y <= reg.y2; y += COMP_ROWS) {
        int rows = min(COMP_ROWS, reg.y2 - y + 1);
        for (int r = 0; r < rows; ++r)
            memcpy(s_compBuf + (size_t)r * w, s_phys + (size_t)(y + r) * SCREEN_W + reg.x1, (size_t)w * 2);
        if (s_wedgeOn) {
            const Rect chunk = { reg.x1, y, reg.x2, y + rows - 1, true };
            const Rect b = rect_and(chunk, s_wedge);
            if (b.valid) sweep_blend(s_compBuf, reg.x1, y, w, b);
        }
        if (s_pulseOn) {
            const Rect chunk = { reg.x1, y, reg.x2, y + rows - 1, true };
            const Rect b = rect_and(chunk, s_pulseBox);
            if (b.valid) pulse_blend(s_compBuf, reg.x1, y, w, b);
        }
        panel_push((int16_t)reg.x1, (int16_t)y, s_compBuf, (uint16_t)w, (uint16_t)rows);
    }
}

static void sweep_poll_ui() {
    s_swVisible = ui_sweep_visible();
    int16_t m[SWEEP_MASKS][5];
    const int n = ui_sweep_masks(m, SWEEP_MASKS);
    s_maskN = 0;
    for (int i = 0; i < n; ++i) {              // logical box -> physical bounding box
        const float xs[4] = { (float)m[i][0], (float)m[i][2], (float)m[i][2], (float)m[i][0] };
        const float ys[4] = { (float)m[i][1], (float)m[i][1], (float)m[i][3], (float)m[i][3] };
        float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
        for (int k = 0; k < 4; ++k) {
            float px, py;
            log_to_phys(xs[k], ys[k], &px, &py);
            x1 = min(x1, px); y1 = min(y1, py); x2 = max(x2, px); y2 = max(y2, py);
        }
        float rad = (float)m[i][4];                  // LVGL caps the radius at half the short side
        rad = min(rad, (float)(m[i][2] - m[i][0] + 1) * 0.5f);
        rad = min(rad, (float)(m[i][3] - m[i][1] + 1) * 0.5f);
        s_mask[s_maskN++] = { { (int)floorf(x1) - 1, (int)floorf(y1) - 1, (int)ceilf(x2) + 1, (int)ceilf(y2) + 1, true },
                              (float)m[i][0], (float)m[i][1], (float)m[i][2], (float)m[i][3], rad };
    }
}

static bool sweep_ready() { return s_phys && s_angMap && s_compBuf; }

static void compose_step() {
    if (!sweep_ready()) return;
    const bool on  = s_swWanted && s_swVisible;
    const bool pOn = s_pulseWanted && s_swVisible;
    if (!on && !pOn && !s_wedgeOn && !s_pulseOn) return;
    const uint32_t now = millis();
    const bool changed = on != s_wedgeOn || pOn != s_pulseOn;
    if (!changed && now - s_lastComposeMs < SWEEP_FRAME_MS) return;
    // Advance by elapsed time, but never more than SWEEP_MAX_STEP_MS per tick: a blocked
    // loop (GPS read, full repaint) then pauses the sweep briefly instead of making it jump.
    uint32_t dt = (s_wedgeOn || s_pulseOn) ? now - s_lastComposeMs : 0;
    if (dt > SWEEP_MAX_STEP_MS) dt = SWEEP_MAX_STEP_MS;
    s_lastComposeMs = now;

    // Old + new boxes, pushed separately: their union would be mostly empty.
    Rect regW = s_wedgeOn ? s_wedge : Rect{};
    Rect regP = s_pulseOn ? s_pulseBox : Rect{};
    if (on) {
        s_swPhase = (s_swPhase + (uint32_t)(((uint64_t)dt * ANG_UNITS * 256) / SWEEP_COMP_PERIOD_MS)) & (ANG_UNITS * 256 - 1);
        const int u = (int)((s_swPhase >> 8) + ((uint32_t)s_rot * ANG_UNITS) / 360) & (ANG_UNITS - 1);
        const Rect nw = wedge_box(u);
        rect_grow(regW, nw);
        s_angU = u;
        const float a = (float)u * (2.0f * (float)M_PI / (float)ANG_UNITS);
        s_leadUx = sinf(a); s_leadUy = -cosf(a);   // 0 = up, clockwise, as in the bearing map
        s_wedge = nw;
    } else {
        s_wedge.valid = false;
    }
    s_wedgeOn = on;
    if (pOn) {
        s_pulseMs = (s_pulseMs + dt) % PULSE_PERIOD_MS;
        const float f = (float)s_pulseMs / (float)PULSE_PERIOD_MS;
        s_pulseR = (PULSE_D_MIN + f * (PULSE_D_MAX - PULSE_D_MIN)) * 0.5f;
        s_pulseOpa = (uint32_t)((1.0f - f) * PULSE_OPA);
        s_pulseBox = pulse_box(s_pulseR);
        rect_grow(regP, s_pulseBox);
    } else {
        s_pulseBox.valid = false;
    }
    s_pulseOn = pOn;
    push_region(regW);                         // old wedge area restored, new wedge drawn
    push_region(regP);                         // the same for the ring
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
static void compose_step();
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    // Let the sweep tick between strips of a long LVGL frame (a full repaint takes ~200 ms).
    // Safe mid-frame: strips already flushed are in s_phys and on the panel; the rest of
    // s_phys still matches what the panel shows, so the composite is never wrong.
    compose_step();
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
    // Sweep compositor buffers (PSRAM; ~0.9 MB). Without them the sweep stays in LVGL.
    s_phys    = (uint16_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H, 2, MALLOC_CAP_SPIRAM);
    s_angMap  = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
    s_compBuf = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * COMP_ROWS * 2, MALLOC_CAP_SPIRAM);
    if (sweep_ready()) {
        const uint32_t t0 = millis();
        sweep_build_map();
        sweep_build_lut();
        Serial.printf("[display] sweep compositor ready (map %lu ms)\n", (unsigned long)(millis() - t0));
    } else {
        heap_caps_free(s_phys); heap_caps_free(s_angMap); heap_caps_free(s_compBuf);
        s_phys = s_angMap = s_compBuf = nullptr;
        Serial.println("[display] WARNING: compositor buffers failed; sweep stays in LVGL");
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
        Serial.println("[display] touch registered (" BOARD_NAME ")");
    }

    Serial.printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();                   // M3: radar/list/stats views + tap-to-inspect
    Serial.println("[display] LVGL ready");
    return true;
}

void loop() {
    if (sweep_ready()) sweep_poll_ui();
    lv_timer_handler();
    compose_step();
}

void sweepConfig(uint16_t ring565, uint16_t lead565, uint16_t ink565, bool sweep, bool pulse) {
    s_swRing = ring565;
    s_swLead = lead565;
    s_swWanted = sweep;
    s_pulseInk = ink565;
    s_pulseWanted = pulse;
}
void sweepTick() { compose_step(); }
bool sweepAvailable() { return sweep_ready(); }

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
    if (sweep_ready()) {                         // the compositor's copy must match the panel
        memset(s_phys, 0, (size_t)SCREEN_W * SCREEN_H * 2);
        s_wedgeOn = false;
        s_wedge.valid = false;
        s_pulseOn = false;
        s_pulseBox.valid = false;
        sweep_build_map();                       // the scope centre moves by a fraction of a pixel
    }
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);   // full repaint in the new orientation
}
uint16_t rotation() { return s_rot; }

uint32_t inactiveMs() { return lv_disp_get_inactive_time(NULL); }

} // namespace display
