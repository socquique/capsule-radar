// Radar scope (M1) + aircraft (M2) + selection (M3) + selectable themes (M4) + idle clock.
// Pure LVGL, portable. Visual reference: assets/plane_radar_2.0_mockup.html
//   THEME_PHOSPHOR : green-on-black radar scope (rings, sweep, altitude glyphs)
//   THEME_ORB   : Orb scope: green gradient, square grid, the 7 nearest
//                    aircraft as yellow balls (emitting waves) + off-range arrows.
// Idle clock (clock_face.h has the animation): this file dims the scene in a few steps and
// draws the dial; the compositor (display.cpp) draws the moving hands on the device, and
// this file draws them itself when there is no compositor (the SDL simulator).
#include "radar_view.h"
#include "config.h"
#include "geo.h"
#include "coastline.h"
#include "airports.h"
#include "clock_face.h"
#include <lvgl.h>
#include "src/draw/sw/lv_draw_sw.h"    // lv_draw_sw_ctx_t: the cache renderer's draw context
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- phosphor palette (mockup) ----
#define COL_GREEN  lv_color_hex(0x1DFF86)
#define COL_LEAD   lv_color_hex(0x3DFF9A)
#define COL_INK    lv_color_hex(0xEAFFF3)
#define COL_SOFT   lv_color_hex(0x9AFFC8)
#define COL_EMERG  lv_color_hex(0xFF5A3C)
// Emergency glyph CORE. White on purpose: no altitude band uses white, so a red-ringed
// white glyph cannot be confused with the plain red sub-3000 ft fill — two users (#25)
// read low-altitude red + the new-contact ping as an emergency.
#define COL_EMERG_CORE lv_color_hex(0xFFFFFF)
// coastline outline — steel blue, deliberately off the red/amber/lime/green/cyan
// altitude-trail palette so land never reads as an aircraft track.
#define COAST_COLOR lv_color_hex(0x4E86C6)
// airport markers — a neutral muted grey-blue so they sit quietly under the traffic.
#define AIRPORT_COLOR lv_color_hex(0x8A93A6)
// ---- orb palette (Orb) ----
#define ORB_BLIP   lv_color_hex(0xFFE11A)
#define ORB_EMERG  lv_color_hex(0xFF4D2E)
#define ORB_ACCENT lv_color_hex(0xFF8A1E)
#define ORB_GRID   lv_color_hex(0x3F8B30)
#define ORB_BG_TOP lv_color_hex(0x18540F)
#define ORB_BG_BOT lv_color_hex(0x09250A)
#define ORB_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config: SWEEP_* live in config.h (shared with the compositor in display.cpp) ----

// ---- aircraft / flow / orb config ----
#define TRAIL_MAX         7
#define TAP_RADIUS_PX     40    // generous finger-tap catch radius (picks the nearest glyph within it)
#define FLOW_MAX          700
#define INV_MERGE_PAD_PX  24                 // partial invalidation: merge areas this close (px)
#define INV_MAX_AREAS     12                 // past this many areas, take their union instead
#define FLOW_OPA          55
#define ORB_BLIPS      7
#define ORB_ARROWS     8
#define BALL_R            9
#define WAVE_EXPAND       28.0f

static int        s_theme    = THEME_PHOSPHOR;
static void      (*s_themeCb)(int) = nullptr;
// scope "chrome" palette (rings/sweep/crosshair/labels) — retinted per theme
static lv_color_t s_cRing = COL_GREEN, s_cLead = COL_LEAD, s_cInk = COL_INK, s_cSoft = COL_SOFT;
static lv_obj_t  *s_parent   = nullptr;
static lv_obj_t  *s_bgImg    = nullptr;      // scope background cache (bottom of the tile)
static lv_color_t *s_cacheBuf = nullptr;     // its RGB565 pixels, SCREEN_W x SCREEN_H
static lv_img_dsc_t s_cacheDsc;              // wraps s_cacheBuf for lv_img
static lv_obj_t  *s_sweep     = nullptr;
static lv_obj_t  *s_acLayer   = nullptr;
static lv_obj_t  *s_rose[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t  *s_centerDot = nullptr;
static lv_obj_t  *s_pulse     = nullptr;
static lv_obj_t  *s_rangeLbl  = nullptr;
static bool       s_rangeLblVisible = true;
static bool       s_sweepEnabled    = true;
static bool       s_airportsEnabled = true;
static int        s_maxOnScreen     = 20;          // how many (nearest) aircraft to draw (web-configurable)
static bool       s_bigText         = false;       // accessibility: bigger glyph labels (set before init)
static int        s_units    = 0;           // scope altitude labels: 0 = ft · 1 = m (setUnits)
static int        s_trailMax        = TRAIL_MAX;   // per-aircraft trail length (0 = off)
static int        s_flowMax         = FLOW_MAX;    // persistent flow-layer segments, count cap (0 = off)
static int        s_flowGenMax      = 14;          // ...and an age cap in polls (~2 s each) so tracks fade out
static lv_timer_t *s_timer    = nullptr;
static float       s_sweepDeg = 0.0f;
// Sweep compositor hooks (device only; main.cpp sets them when display.cpp can draw the
// sweep per pixel). Unset, e.g. in the SDL simulator, LVGL draws the sweep as before.
static void (*s_compConfig)(uint16_t ring565, uint16_t lead565, uint16_t ink565, bool sweep, bool pulse) = nullptr;
static void (*s_compTick)() = nullptr;
static inline void comp_tick() { if (s_compTick) s_compTick(); }   // cheap unless a sweep tick is due
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = POLL_INTERVAL_MS;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static std::string s_selHex;

// ---- idle clock ----
static ClockFace   s_clock;
static int         s_clockStage = 0;            // scene step applied (ClockFrame::stage; 0 = radar)
static lv_obj_t   *s_clockLayer = nullptr;      // dial (+ the hands when there is no compositor)
static ClockFrame  s_lvClock    = {};           // last frame, for the LVGL-drawn hands (simulator)
static lv_opa_t    s_dimOpa     = LV_OPA_COVER; // scene behind the clock: planes, trails, rings, map
static bool        s_labelsOn   = true;         // plane names and altitudes
static lv_opa_t    s_dialOpa    = 0;            // dial, as a fraction of its own (dim) levels
static float     (*s_compAngle)() = nullptr;    // the compositor's sweep angle (setSweepCompositor)
static void      (*s_clockStageCb)(int) = nullptr;
static bool        clock_time_now(ClockHands &out);
static bool      (*s_clockTime)(ClockHands &) = clock_time_now;
static ClockHands  s_wall = {};                 // last good wall time

// Scale an opacity by the clock's scene dim (255 = radar as usual).
static inline lv_opa_t dim(int opa) { return (lv_opa_t)((opa * s_dimOpa + 127) / 255); }

// Default time source: the system clock (RTC-seeded, NTP-synced) in the configured zone.
// Before 2023-11-14 the clock was never set: no clock face then.
static bool clock_time_now(ClockHands &out) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec <= 1700000000L) return false;
    const time_t t = tv.tv_sec;
    struct tm lt;
    localtime_r(&t, &lt);
    out = clock_hands(lt.tm_hour, lt.tm_min, lt.tm_sec, (int)(tv.tv_usec / 1000));
    return true;
}
static const ClockHands &clock_wall() {
    ClockHands h;
    if (s_clockTime(h)) s_wall = h;
    return s_wall;
}

struct FlowSeg { lv_point_t a, b; uint16_t gen; };   // gen = the poll it was laid down on
static std::deque<FlowSeg> s_flow;
static uint16_t s_flowGen = 0;        // ++ each update(); flow segments fade out after s_flowGenMax polls

struct AcDraw {
    lv_point_t pos;            // current (animated) screen position — what gets drawn
    lv_point_t from, to;       // smooth-motion glide endpoints (M4 interpolation)
    float      track;
    lv_color_t color;
    bool       emergency;
    bool       inRange;
    char       hex[8];
    char       call[12];
    char       type[8];
    char       altTxt[12];
    float      altFt;
    bool       onGround;
    float      vsFpm, gsKt, distKm, bearingDeg;
    int        squawk;
    const std::vector<lv_point_t> *trail;    // points into s_trailStore (null: no history)
};
static std::vector<AcDraw> s_acs;
// Per-aircraft trail history in fixed slots keyed by hex (n <= ADSB_MAX_AIRCRAFT: no map
// nodes or std::string keys to build and free on every poll; the per-poll AcDraw list points
// at a slot instead of copying the vector). Slots never move.
struct TrailSlot { char hex[8]; bool used; std::vector<lv_point_t> pts; };
static TrailSlot s_trailStore[ADSB_MAX_AIRCRAFT];
static TrailSlot *trail_slot(const char *hex) {
    TrailSlot *free = nullptr;
    for (TrailSlot &t : s_trailStore) {
        if (t.used) { if (strcmp(t.hex, hex) == 0) return &t; }
        else if (!free) free = &t;
    }
    // Null only when every slot is held. At the feed cap, a contact that arrives in the poll
    // another one leaves starts its trail a poll later (update() frees departed slots afterwards).
    return free;
}

static const float GX[4] = { 0.0f,  7.0f, 0.0f, -7.0f };
static const float GY[4] = { -11.0f, 5.0f, 8.0f, 5.0f };

static inline bool orb() { return s_theme == THEME_ORB; }
// The second hand's colour. Orb has no sweep and a green lead on a green scope: orange there.
static inline lv_color_t clock_lead_color() { return orb() ? ORB_ACCENT : s_cLead; }

// The aircraft the scope draws, and so the ones a tap can hit: the nearest s_maxOnScreen,
// then Orb's own caps (ORB_BLIPS in-range balls, ORB_ARROWS off-range arrows); phosphor
// draws in-range traffic only. The selected aircraft is drawn wherever it sits, past every
// cap: the list shows the whole feed, so a row picked there must still get its glyph, its
// ring and a tap target. ac_draw_cb, hitTest and the Orb wave refresh all walk this one
// list, so they cannot disagree about what is on screen.
template <typename F>
static void for_each_drawn(F &&fn) {
    const bool drg = orb();
    int shown = 0, balls = 0, arrows = 0;
    for (size_t i = 0; i < s_acs.size(); ++i) {
        const AcDraw &ac = s_acs[i];
        const bool sel = !s_selHex.empty() && s_selHex == ac.hex;
        if (++shown > s_maxOnScreen && !sel) continue;
        if (drg) {
            int &n = ac.inRange ? balls : arrows;
            if (n >= (ac.inRange ? ORB_BLIPS : ORB_ARROWS) && !sel) continue;
            ++n;
        } else if (!ac.inRange) {
            continue;
        }
        fn(i, ac);
    }
}

static void show(lv_obj_t *o, bool v) {
    if (!o) return;
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_color_t alt_color(float altFt, bool onGround) {
    if (onGround)      return lv_color_hex(0x888888);
    if (altFt < 3000)  return lv_color_hex(0xFF5A3C);
    if (altFt < 10000) return lv_color_hex(0xFFB23C);
    if (altFt < 20000) return lv_color_hex(0xC8FF3C);
    if (altFt < 30000) return lv_color_hex(0x39FF14);
    return lv_color_hex(0x3CE0FF);
}
// Per-aircraft altitude label under the glyph. Same ft/m rule as ui.cpp fmt_alt().
static void fmt_alt_txt(char *dst, size_t n, float altFt, bool onGround) {
    if (onGround)          snprintf(dst, n, "GND");
    else if (s_units == 1) snprintf(dst, n, "%.0f m", (double)altFt * 0.3048f);
    else                   snprintf(dst, n, "%.0f ft", (double)altFt);
}

static inline lv_point_t rim_point(float bearingDeg, float r) {
    const float a = bearingDeg * (float)M_PI / 180.0f;
    lv_point_t p;
    p.x = (lv_coord_t)lroundf((float)s_cx + r * sinf(a));
    p.y = (lv_coord_t)lroundf((float)s_cy - r * cosf(a));
    return p;
}

// rotate the local point (px,py) by `deg` (clockwise, screen coords) and offset to (ox,oy)
static inline lv_point_t rot_pt(float px, float py, float deg, lv_coord_t ox, lv_coord_t oy) {
    const float a = deg * (float)M_PI / 180.0f;
    const float c = cosf(a), s = sinf(a);
    lv_point_t p;
    p.x = (lv_coord_t)(ox + (lv_coord_t)lroundf(px * c - py * s));
    p.y = (lv_coord_t)(oy + (lv_coord_t)lroundf(px * s + py * c));
    return p;
}

// ======================= scope background cache ==============================
// Everything under the aircraft that does not move between polls -- the background (black;
// Orb's gradient), the persistent flow tracks and the chrome the grid layer used to draw
// (coastline, airports, rings, crosshair; Orb: grid, coastline, airports, centre triangle)
// -- lives in ONE full-screen RGB565 buffer shown by s_bgImg at the bottom of the tile.
// TRUE_COLOR at img_opa COVER passes LVGL's cover check, so any repaint of a scope area
// starts at the image: the layers under it are skipped and the cache pixels are a straight
// copy. cache_render_rect() renders any rectangle with LVGL's own software renderer aimed at
// the buffer (the trick behind lv_canvas_draw_*), so re-rendering a rect (added or pruned
// flow, dim change) is exact and repeatable.
static void cache_bg_fill(lv_draw_ctx_t *d, const lv_area_t *clip) {
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();          // the tileview's black, under the tile: the dim
    bg.bg_opa = LV_OPA_COVER;                // gradient blends onto it, not onto the cache's
    lv_draw_rect(d, &bg, clip);              // previous content (V blended over V stays V)
    if (orb()) {                             // the radar tile's gradient, dimmed behind the
        bg.bg_grad.dir = LV_GRAD_DIR_VER;    // clock exactly as the tile's bg_opa did
        bg.bg_grad.stops[0].color = ORB_BG_TOP;
        bg.bg_grad.stops[1].color = ORB_BG_BOT;
        bg.bg_opa = s_dimOpa;
        lv_draw_rect(d, &bg, clip);
    }
}

// Flow segments whose 2 px anti-aliased line can touch the clip rect.
static void cache_flow_draw(lv_draw_ctx_t *d, const lv_area_t *clip) {
    lv_draw_line_dsc_t fl;
    lv_draw_line_dsc_init(&fl);
    fl.color = orb() ? ORB_FLOW : s_cRing;
    fl.width = 2;
    fl.opa = dim(FLOW_OPA);
    uint32_t n = 0;
    for (const FlowSeg &s : s_flow) {
        if ((++n & 31) == 0) comp_tick();    // keep the sweep moving through long passes
        if (LV_MAX(s.a.x, s.b.x) + 2 < clip->x1 || LV_MIN(s.a.x, s.b.x) - 2 > clip->x2 ||
            LV_MAX(s.a.y, s.b.y) + 2 < clip->y1 || LV_MIN(s.a.y, s.b.y) - 2 > clip->y2) continue;
        lv_point_t pts[2] = { s.a, s.b };
        lv_draw_line(d, &fl, &pts[0], &pts[1]);
    }
}

// Ring and crosshair sizes were laid out for the AMOLED scope (outer ring 218 px); scale
// them with the board's outer ring so the spacing stays even on other panels (rounded,
// exact on the AMOLED boards).
#define SCOPE_PX(px) ((lv_coord_t)(((px) * RADAR_R_OUTER_PX + 109) / 218))

// The chrome the former grid layer drew, in the same z-order: coastline, airports, rings and crosshair.
static void cache_chrome_draw(lv_draw_ctx_t *d, const lv_area_t *clip) {
    const lv_point_t c = { s_cx, s_cy };

    if (orb()) {
        lv_draw_line_dsc_t gl;
        lv_draw_line_dsc_init(&gl);
        gl.color = ORB_GRID;
        gl.width = 1;
        gl.opa = dim(120);
        const int step = 38;
        for (int x = s_cx % step; x < SCREEN_W; x += step) {
            lv_point_t p1 = { (lv_coord_t)x, 0 }, p2 = { (lv_coord_t)x, SCREEN_H - 1 };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        for (int y = s_cy % step; y < SCREEN_H; y += step) {
            lv_point_t p1 = { 0, (lv_coord_t)y }, p2 = { SCREEN_W - 1, (lv_coord_t)y };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        // center "you are here" triangle (orange, pointing up)
        lv_point_t tri[3] = { rot_pt(0, -11, 0, s_cx, s_cy),
                              rot_pt(10, 8, 0, s_cx, s_cy),
                              rot_pt(-10, 8, 0, s_cx, s_cy) };
        lv_draw_rect_dsc_t td;
        lv_draw_rect_dsc_init(&td);
        td.bg_color = ORB_ACCENT;
        td.bg_opa = dim(LV_OPA_COVER);
        td.border_color = lv_color_hex(0x8A4A00);
        td.border_width = 1;
        td.border_opa = dim(160);
        coastline_draw(d, COAST_COLOR, dim(170), 2, clip);   // landmass outline under the triangle
        if (s_airportsEnabled) airports_draw(d, AIRPORT_COLOR, dim(150));
        lv_draw_polygon(d, &td, tri, 3);
        return;
    }

    // coastline first, so the rings/crosshair sit cleanly on top of it.
    // Steel blue + 2 px so it reads as a map outline, distinct from the green altitude trails.
    coastline_draw(d, COAST_COLOR, dim(165), 2, clip);
    if (s_airportsEnabled) airports_draw(d, AIRPORT_COLOR, dim(150));

    // phosphor: concentric rings + crosshair
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = s_cRing;
    ad.width = 2;
    const lv_coord_t rr[4] = { SCOPE_PX(50), SCOPE_PX(104), SCOPE_PX(160), RADAR_R_OUTER_PX };
    const lv_opa_t   ro[4] = { 66, 66, 66, 87 };
    for (int i = 0; i < 4; ++i) { ad.opa = dim(ro[i]); lv_draw_arc(d, &ad, &c, rr[i], 0, 360); }

    lv_draw_line_dsc_t ll;
    lv_draw_line_dsc_init(&ll);
    ll.color = s_cRing;
    ll.width = 2;
    ll.opa = dim(41);
    const lv_coord_t ch = SCOPE_PX(211);
    lv_point_t h1 = { (lv_coord_t)(s_cx - ch), s_cy }, h2 = { (lv_coord_t)(s_cx + ch), s_cy };
    lv_point_t v1 = { s_cx, (lv_coord_t)(s_cy - ch) }, v2 = { s_cx, (lv_coord_t)(s_cy + ch) };
    lv_draw_line(d, &ll, &h1, &h2);
    lv_draw_line(d, &ll, &v1, &v2);
}

// Render one rect of the cache (screen coordinates, clipped to the screen).
static void cache_render_rect(const lv_area_t *rect) {
    if (!s_cacheBuf) return;
    lv_area_t clip;                                   // stays alive for the whole render
    clip.x1 = LV_MAX(rect->x1, 0);
    clip.y1 = LV_MAX(rect->y1, 0);
    clip.x2 = LV_MIN(rect->x2, SCREEN_W - 1);
    clip.y2 = LV_MIN(rect->y2, SCREEN_H - 1);
    if (clip.x2 < clip.x1 || clip.y2 < clip.y1) return;

    // A stand-in display aimed at the cache buffer, the way lv_canvas_draw_* does it
    // (init_fake_disp): with no set_px_cb the software renderer takes the fast path and
    // blends straight into s_cacheBuf. Static storage: this runs on the LVGL thread only.
    static lv_disp_t fake;
    static lv_disp_drv_t drv;
    static lv_area_t buf_area = { 0, 0, SCREEN_W - 1, SCREEN_H - 1 };
    static lv_draw_sw_ctx_t sw_ctx;
    lv_memset_00(&fake, sizeof(fake));
    fake.driver = &drv;
    lv_disp_drv_init(&drv);                           // antialiasing on, like the real driver
    drv.hor_res = SCREEN_W;
    drv.ver_res = SCREEN_H;
    drv.draw_ctx = (lv_draw_ctx_t *)&sw_ctx;
    lv_draw_sw_init_ctx(&drv, &sw_ctx.base_draw);
    sw_ctx.base_draw.clip_area = &clip;
    sw_ctx.base_draw.buf_area = &buf_area;
    sw_ctx.base_draw.buf = s_cacheBuf;

    lv_disp_t *refr_ori = _lv_refr_get_disp_refreshing();
    _lv_refr_set_disp_refreshing(&fake);
    cache_bg_fill(&sw_ctx.base_draw, &clip);
    comp_tick();
    cache_flow_draw(&sw_ctx.base_draw, &clip);
    comp_tick();
    cache_chrome_draw(&sw_ctx.base_draw, &clip);
    comp_tick();
    _lv_refr_set_disp_refreshing(refr_ori);
    lv_draw_sw_deinit_ctx(&drv, &sw_ctx.base_draw);
}

// Whole-cache re-render: init, theme / airports / trail-length / clock-stage change, and a
// scope geometry change in update() -- the events that used to repaint the grid layer or
// rebuild the flow canvas.
static void cache_full_rerender(void) {
    if (!s_cacheBuf || !s_bgImg) return;
    const lv_area_t all = { 0, 0, SCREEN_W - 1, SCREEN_H - 1 };
    cache_render_rect(&all);
    lv_obj_invalidate(s_bgImg);
}

// =============================== sweep =======================================
static void sweep_draw_cb(lv_event_t *e) {
    if (orb() || s_compConfig) return;           // the compositor draws it
    lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);
    const lv_point_t center = { s_cx, s_cy };
    const float R = (float)RADAR_R_OUTER_PX;

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = s_cRing;
    ld.width = 5;
    ld.round_start = 1;
    ld.round_end = 1;
    // While the clock is up (simulator only: s_compConfig is null here) the trail dims and
    // follows the second hand, which clock_draw_cb draws on top of the planes.
    const bool clockUp = s_clockStage != 0;
    const float trailMul = clockUp ? (float)s_lvClock.trailOpa / 255.0f : 1.0f;
    for (int i = SWEEP_TRAIL_STEPS; i >= 1; --i) {
        const float frac = 1.0f - (float)i / (float)SWEEP_TRAIL_STEPS;
        const float ang  = s_sweepDeg - (float)i * (SWEEP_TRAIL_DEG / (float)SWEEP_TRAIL_STEPS);
        ld.opa = (lv_opa_t)(frac * frac * (float)SWEEP_TRAIL_OPA * trailMul);
        if (ld.opa < 2) continue;
        lv_point_t p2 = rim_point(ang, R);
        lv_draw_line(dctx, &ld, &center, &p2);
    }
    if (clockUp) return;
    lv_draw_line_dsc_t le;
    lv_draw_line_dsc_init(&le);
    le.color = s_cLead;
    le.width = 2;
    le.opa = SWEEP_LEAD_OPA;
    le.round_start = 1;
    le.round_end = 1;
    lv_point_t lead = rim_point(s_sweepDeg, R);
    lv_draw_line(dctx, &le, &center, &lead);
}

static void wedge_bbox(float deg, lv_area_t *out) {
    lv_coord_t minx = s_cx, maxx = s_cx, miny = s_cy, maxy = s_cy;
    const int steps = 10;
    for (int i = 0; i <= steps; ++i) {
        const float a = deg - SWEEP_TRAIL_DEG * (float)i / (float)steps;
        const lv_point_t p = rim_point(a, (float)RADAR_R_OUTER_PX);
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    const lv_coord_t pad = 6;
    out->x1 = minx - pad; out->y1 = miny - pad;
    out->x2 = maxx + pad; out->y2 = maxy + pad;
}

// glyph + label bounding box (for partial invalidation during the glide).
// Must cover the label areas drawn in the aircraft layer (they grew for large-text mode).
static inline lv_area_t glyph_bbox(lv_point_t p) {
    lv_area_t a;
    if (orb()) { a.x1 = p.x - 30; a.y1 = p.y - 30; a.x2 = p.x + 30;  a.y2 = p.y + 30; }
    else          { a.x1 = p.x - 22; a.y1 = p.y - 22; a.x2 = p.x + 174; a.y2 = p.y + 32; }
    return a;
}
static inline void area_union(lv_area_t &d, const lv_area_t &s) {
    d.x1 = LV_MIN(d.x1, s.x1); d.y1 = LV_MIN(d.y1, s.y1);
    d.x2 = LV_MAX(d.x2, s.x2); d.y2 = LV_MAX(d.y2, s.y2);
}

// ---- partial invalidation (update()) ----
// Areas nearer than INV_MERGE_PAD_PX merge into one; past INV_MAX_AREAS the union is taken.
// Keeps the pending-area count far under LVGL's LV_INV_BUF_SIZE (32), above which LVGL
// repaints the whole screen anyway.
static int inv_merge(lv_area_t *a, int n) {
    if (n <= 1) return n;
    for (;;) {
        bool merged = false;
        for (int i = 0; i < n && !merged; ++i) {
            for (int j = i + 1; j < n; ++j) {
                if (a[i].x1 - INV_MERGE_PAD_PX > a[j].x2 || a[j].x1 - INV_MERGE_PAD_PX > a[i].x2 ||
                    a[i].y1 - INV_MERGE_PAD_PX > a[j].y2 || a[j].y1 - INV_MERGE_PAD_PX > a[i].y2) continue;
                area_union(a[i], a[j]);
                a[j] = a[--n];
                merged = true;
                break;
            }
        }
        if (!merged) break;
    }
    if (n > INV_MAX_AREAS) {
        for (int i = 1; i < n; ++i) area_union(a[0], a[i]);
        n = 1;
    }
    return n;
}

// Everything one drawn aircraft paints: the glyph with its selection/emergency rings
// (glyph_bbox; Orb's waves add the 44 px box) and the trail polyline (2 px line). The
// labels sit inside glyph_bbox (x +12..+168, y -14..+26), so they need no box of their own.
static void drawn_bounds(const AcDraw &ac, lv_area_t &a) {
    a = glyph_bbox(ac.pos);
    if (orb() && ac.inRange) {
        const lv_area_t w = { (lv_coord_t)(ac.pos.x - 44), (lv_coord_t)(ac.pos.y - 44),
                              (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y + 44) };
        area_union(a, w);
    }
    if (ac.trail && !ac.trail->empty()) {
        lv_area_t t = { ac.trail->front().x, ac.trail->front().y,
                        ac.trail->front().x, ac.trail->front().y };
        for (const lv_point_t &p : *ac.trail) {
            if (p.x < t.x1) t.x1 = p.x;
            if (p.y < t.y1) t.y1 = p.y;
            if (p.x > t.x2) t.x2 = p.x;
            if (p.y > t.y2) t.y2 = p.y;
        }
        t.x1 -= 2; t.y1 -= 2; t.x2 += 2; t.y2 += 2;
        area_union(a, t);
    }
}

// Per-hex drawn bounds of whatever for_each_drawn yields right now.
struct HexArea { char hex[8]; lv_area_t a; };
static int collect_drawn_bounds(HexArea *out, int max) {
    int n = 0;
    for_each_drawn([&](size_t, const AcDraw &ac) {
        if (n >= max) return;
        snprintf(out[n].hex, sizeof(out[n].hex), "%s", ac.hex);
        drawn_bounds(ac, out[n].a);
        ++n;
    });
    return n;
}

// A flow segment's dirty rect: its line, padded for the 2 px anti-aliased stroke.
static inline void flow_seg_rect(const FlowSeg &s, lv_area_t &r) {
    r.x1 = LV_MIN(s.a.x, s.b.x) - 2; r.y1 = LV_MIN(s.a.y, s.b.y) - 2;
    r.x2 = LV_MAX(s.a.x, s.b.x) + 2; r.y2 = LV_MAX(s.a.y, s.b.y) + 2;
}

// Advance each glyph from its previous position toward the new target (ease-out),
// invalidating only the small region each one occupies. Self-limiting: when a plane
// barely moves (far away / slow), nx==pos and it's skipped — near-zero cost.
static void interp_step(void) {
#if MOTION_INTERP
    if (!s_acLayer || s_acs.empty()) return;
    const uint32_t now = lv_tick_get();
    float t = s_pollMs ? (float)(now - s_animStartMs) / (float)s_pollMs : 1.0f;
    if (t > 1.0f) t = 1.0f;
    const float e = t * (2.0f - t);                  // ease-out quad
    for (AcDraw &ac : s_acs) {
        const lv_coord_t nx = ac.from.x + (lv_coord_t)lroundf((float)(ac.to.x - ac.from.x) * e);
        const lv_coord_t ny = ac.from.y + (lv_coord_t)lroundf((float)(ac.to.y - ac.from.y) * e);
        if (nx == ac.pos.x && ny == ac.pos.y) continue;
        lv_point_t np; np.x = nx; np.y = ny;
        lv_area_t inv = glyph_bbox(ac.pos);
        area_union(inv, glyph_bbox(np));
        ac.pos = np;
        lv_obj_invalidate_area(s_acLayer, &inv);
    }
#endif
}

namespace radar { static void sweep_sync(); }

// One scene step of the idle clock (ClockFrame::stage): dim the scene, hide the labels, swap
// N/E/S/W for the dial. LVGL repaints take ~150-200 ms on the device, so the scene changes in
// a few steps, each one full repaint; what moves is drawn every frame (by the compositor, or
// by clock_draw_cb without one). Only ever called from the LVGL timer, never mid-render.
static void apply_clock_stage(int stage) {
    if (stage == s_clockStage) return;
    s_clockStage = stage;
    s_dimOpa   = stage == 0 ? LV_OPA_COVER : (stage == 1 ? CLOCK_RADAR_OPA_STEP1 : CLOCK_RADAR_OPA);
    s_labelsOn = stage == 0;
    s_dialOpa  = stage >= 3 ? LV_OPA_COVER : (stage == 2 ? LV_OPA_50 : LV_OPA_TRANSP);
    const lv_opa_t letters = stage == 0 ? LV_OPA_COVER : (stage == 1 ? LV_OPA_50 : LV_OPA_TRANSP);
    for (lv_obj_t *r : s_rose) if (r) lv_obj_set_style_text_opa(r, letters, 0);
    cache_full_rerender();                     // the dim applies to everything in the cache
    if (s_parent && orb()) lv_obj_set_style_bg_opa(s_parent, s_dimOpa, 0);
    show(s_rangeLbl, stage == 0 && s_rangeLblVisible && !orb());
    show(s_clockLayer, stage != 0);
    radar::sweep_sync();                       // the centre ripple pauses while the clock is up
    if (s_parent) lv_obj_invalidate(s_parent);
    if (s_clockStageCb) s_clockStageCb(stage);
}

// Idle clock, once per sweep tick: apply a scene step when it changes; without a compositor
// (simulator) also move the LVGL-drawn hands, and keep the LVGL sweep on the second hand so
// that, after the exit, the radar sweep goes on from where the hand was.
static void clock_tick() {
    if (s_clock.phase() == ClockFace::OFF && s_clockStage == 0) return;
    const ClockFrame f = s_clock.frame(lv_tick_get(), clock_wall());
    apply_clock_stage(f.stage);
    if (s_compConfig) return;                  // the compositor draws everything that moves
    s_lvClock = f;
    const float prev = s_sweepDeg;
    s_sweepDeg = f.leadDeg;
    if (s_sweep && s_sweepEnabled && !orb()) {
        lv_area_t a, b;
        wedge_bbox(prev, &a);
        wedge_bbox(s_sweepDeg, &b);
        area_union(a, b);
        lv_obj_invalidate_area(s_sweep, &a);
    }
    if (s_clockLayer && f.stage != 0) lv_obj_invalidate(s_clockLayer);
}

static void sweep_timer_cb(lv_timer_t *t) {
    (void)t;
    if (++s_frameCtr % 3 == 0) interp_step();         // smooth glyph motion (~90 ms cadence)
    clock_tick();                                      // before the Orb / sweep-off returns below
    if (orb()) {
        // animate the blip waves (invalidate only the ball areas)
        s_wavePhase += 0.05f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_acLayer) return;
        for_each_drawn([](size_t, const AcDraw &ac) {
            if (!ac.inRange) return;                 // only the balls carry waves
            lv_area_t a = { (lv_coord_t)(ac.pos.x - 44), (lv_coord_t)(ac.pos.y - 44),
                            (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y + 44) };
            lv_obj_invalidate_area(s_acLayer, &a);
        });
        return;
    }
    if (!s_sweepEnabled) return;          // sweep disabled: glyph interpolation above still runs
    if (s_clockStage != 0 && !s_compConfig) return;   // clock_tick() drives the LVGL sweep
    s_prevSweepDeg = s_sweepDeg;
    s_sweepDeg += 360.0f * (float)SWEEP_FRAME_MS / (float)SWEEP_PERIOD_MS;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
    if (!s_sweep || s_compConfig) return;   // compositor: no LVGL repaint per tick
    lv_area_t a, b, area;
    wedge_bbox(s_prevSweepDeg, &a);
    wedge_bbox(s_sweepDeg, &b);
    area.x1 = LV_MIN(a.x1, b.x1);
    area.y1 = LV_MIN(a.y1, b.y1);
    area.x2 = LV_MAX(a.x2, b.x2);
    area.y2 = LV_MAX(a.y2, b.y2);
    lv_obj_invalidate_area(s_sweep, &area);
}

// =============================== aircraft ====================================
static void draw_trail(lv_draw_ctx_t *d, const AcDraw &ac, lv_color_t col) {
    const int n = ac.trail ? (int)ac.trail->size() : 0;
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 2;
    for (int i = 1; i < n; ++i) {
        t.opa = dim(10 + 45 * i / n);
        lv_point_t a = (*ac.trail)[i - 1], b = (*ac.trail)[i];
        lv_draw_line(d, &t, &a, &b);
    }
}

static void draw_ball(lv_draw_ctx_t *d, const AcDraw &ac) {
    // emitted waves: several expanding rings (sonar-ping look)
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = ORB_ACCENT;
    w.width = 3;
    for (int wv = 0; wv < 3; ++wv) {
        float ph = s_wavePhase + (float)wv * 0.34f;
        if (ph >= 1.0f) ph -= 1.0f;
        w.opa = dim((int)((1.0f - ph) * 245.0f));
        if (w.opa > 6) lv_draw_arc(d, &w, &ac.pos, (uint16_t)(BALL_R + 3 + ph * WAVE_EXPAND), 0, 360);
    }

    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = dim(LV_OPA_COVER);
    b.radius = LV_RADIUS_CIRCLE;
    b.border_color = lv_color_hex(0x7A5A00);
    b.border_width = 1;
    b.border_opa = dim(150);
    lv_area_t r = { (lv_coord_t)(ac.pos.x - BALL_R), (lv_coord_t)(ac.pos.y - BALL_R),
                    (lv_coord_t)(ac.pos.x + BALL_R), (lv_coord_t)(ac.pos.y + BALL_R) };
    lv_draw_rect(d, &b, &r);

    // glossy highlight
    lv_draw_rect_dsc_t hl;
    lv_draw_rect_dsc_init(&hl);
    hl.bg_color = lv_color_hex(0xFFFBCC);
    hl.bg_opa = dim(170);
    hl.radius = LV_RADIUS_CIRCLE;
    lv_area_t hr = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 6),
                     (lv_coord_t)(ac.pos.x - 1), (lv_coord_t)(ac.pos.y - 2) };
    lv_draw_rect(d, &hl, &hr);
}

static void draw_offrange(lv_draw_ctx_t *d, const AcDraw &ac) {
    // small ball at the rim
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = dim(LV_OPA_COVER);
    b.radius = LV_RADIUS_CIRCLE;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 5),
                    (lv_coord_t)(ac.pos.x + 5), (lv_coord_t)(ac.pos.y + 5) };
    lv_draw_rect(d, &b, &r);

    // small orange triangle just outside it, pointing toward the aircraft's bearing
    const lv_coord_t ox = (lv_coord_t)lroundf(ac.pos.x + 12.0f * sinf(ac.bearingDeg * (float)M_PI / 180.0f));
    const lv_coord_t oy = (lv_coord_t)lroundf(ac.pos.y - 12.0f * cosf(ac.bearingDeg * (float)M_PI / 180.0f));
    lv_point_t tri[3] = { rot_pt(0, -7, ac.bearingDeg, ox, oy),
                          rot_pt(5, 4, ac.bearingDeg, ox, oy),
                          rot_pt(-5, 4, ac.bearingDeg, ox, oy) };
    lv_draw_rect_dsc_t td;
    lv_draw_rect_dsc_init(&td);
    td.bg_color = ORB_ACCENT;
    td.bg_opa = dim(LV_OPA_COVER);
    lv_draw_polygon(d, &td, tri, 3);
}

static void ac_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const bool drg = orb();

    for_each_drawn([&](size_t, const AcDraw &ac) {
        if (drg) {
            if (ac.inRange) {
                draw_trail(d, ac, ORB_FLOW);
                draw_ball(d, ac);
            } else {
                draw_offrange(d, ac);
            }
        } else {
            draw_trail(d, ac, ac.color);
            const float th = ((ac.track != ac.track) ? 0.0f : ac.track) * (float)M_PI / 180.0f;
            const float c = cosf(th), s = sinf(th);
            lv_point_t pts[4];
            for (int i = 0; i < 4; ++i) {
                const float x = GX[i] * c - GY[i] * s;
                const float y = GX[i] * s + GY[i] * c;
                pts[i].x = (lv_coord_t)(ac.pos.x + (lv_coord_t)lroundf(x));
                pts[i].y = (lv_coord_t)(ac.pos.y + (lv_coord_t)lroundf(y));
            }
            lv_draw_rect_dsc_t g;
            lv_draw_rect_dsc_init(&g);
            g.bg_color = ac.emergency ? COL_EMERG_CORE : ac.color;   // white core, see define
            g.bg_opa = dim(LV_OPA_COVER);
            lv_draw_polygon(d, &g, pts, 4);
            if (ac.emergency) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = COL_EMERG; h.width = 3; h.opa = dim(255);
                lv_draw_arc(d, &h, &ac.pos, 16, 0, 360);
            }
        }

        // selection ring(s)
        if (!s_selHex.empty() && s_selHex == ac.hex) {
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.width = 2;
            sr.opa = dim(240);
            if (drg) {
                sr.color = ORB_ACCENT;
                lv_draw_arc(d, &sr, &ac.pos, 15, 0, 360);
                lv_draw_arc(d, &sr, &ac.pos, 23, 0, 360);
            } else {
                sr.color = ac.emergency ? COL_EMERG : s_cInk;
                lv_draw_arc(d, &sr, &ac.pos, 19, 0, 360);
            }
        }

        // floating labels (phosphor only; orb keeps clean balls + the tap card; the clock
        // hides them so the hands read cleanly)
        if (!drg && s_labelsOn) {
            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = s_bigText ? &lv_font_montserrat_18 : &lv_font_montserrat_14;
            lc.color = s_cInk;
            lv_area_t a1 = { (lv_coord_t)(ac.pos.x + 12), (lv_coord_t)(ac.pos.y - 14),
                             (lv_coord_t)(ac.pos.x + 168), (lv_coord_t)(ac.pos.y + 4) };
            if (ac.call[0]) lv_draw_label(d, &lc, &a1, ac.call, NULL);
            lv_draw_label_dsc_t la;
            lv_draw_label_dsc_init(&la);
            la.font = s_bigText ? &lv_font_montserrat_16 : &lv_font_montserrat_12;
            la.color = ac.color;
            lv_area_t a2 = { a1.x1, (lv_coord_t)(ac.pos.y + 4), a1.x2, (lv_coord_t)(ac.pos.y + 26) };
            if (ac.altTxt[0]) lv_draw_label(d, &la, &a2, ac.altTxt, NULL);
        }
    });
}

// =============================== idle clock ==================================
// An hour or minute hand from the centre: a dark outline under the ink, so it reads over
// the (dimmed) planes. Simulator only; the compositor draws the same on the device.
static void draw_clock_hand(lv_draw_ctx_t *d, float deg, float len, int w) {
    const lv_point_t c = { s_cx, s_cy };
    const lv_point_t tip = rim_point(deg, len);
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.round_start = 1;
    l.round_end = 1;
    l.color = lv_color_black();
    l.width = (lv_coord_t)(w + CLOCK_OUTLINE_W);
    l.opa = CLOCK_OUTLINE_OPA;
    lv_draw_line(d, &l, &c, &tip);
    l.color = s_cInk;
    l.width = (lv_coord_t)w;
    l.opa = LV_OPA_COVER;
    lv_draw_line(d, &l, &c, &tip);
}

// The dial: fine minute marks, hour marks, and 12/3/6/9 where N/E/S/W were. It never
// moves, so it stays dim (AMOLED burn-in). Without a compositor this layer also draws the
// hands, the second hand and the centre cap from the last ClockFrame.
static void clock_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const float R = (float)RADAR_R_OUTER_PX;
    if (s_dialOpa) {
        const auto scaled = [](int opa) { return (lv_opa_t)((opa * s_dialOpa + 127) / 255); };
        lv_draw_line_dsc_t m;
        lv_draw_line_dsc_init(&m);
        m.color = s_cSoft;
        m.width = 1;
        m.opa = scaled(CLOCK_MTICK_OPA);
        for (int i = 0; i < 60; ++i) {
            if (i % 5 == 0) continue;                    // the hours have their own marks
            const lv_point_t a = rim_point(i * 6.0f, R * CLOCK_MTICK_R0);
            const lv_point_t b = rim_point(i * 6.0f, R * CLOCK_TICK_R1);
            lv_draw_line(d, &m, &a, &b);
        }
        lv_draw_line_dsc_t h;
        lv_draw_line_dsc_init(&h);
        h.color = s_cInk;
        h.width = 3;
        h.opa = scaled(CLOCK_HTICK_OPA);
        for (int i = 0; i < 12; ++i) {
            if (i % 3 == 0) continue;                    // 12, 3, 6 and 9 are numbers
            const lv_point_t a = rim_point(i * 30.0f, R * CLOCK_HTICK_R0);
            const lv_point_t b = rim_point(i * 30.0f, R * CLOCK_TICK_R1);
            lv_draw_line(d, &h, &a, &b);
        }
        lv_draw_label_dsc_t n;
        lv_draw_label_dsc_init(&n);
        n.font = &lv_font_montserrat_28;
        n.color = s_cInk;
        n.opa = scaled(CLOCK_NUM_OPA);
        n.align = LV_TEXT_ALIGN_CENTER;
        const lv_coord_t lh = lv_font_get_line_height(n.font);
        static const char *const NUM[4] = { "12", "3", "6", "9" };
        for (int i = 0; i < 4; ++i) {
            const lv_point_t c = rim_point(i * 90.0f, R * CLOCK_NUM_R);
            const lv_area_t a = { (lv_coord_t)(c.x - 30), (lv_coord_t)(c.y - lh / 2),
                                  (lv_coord_t)(c.x + 30), (lv_coord_t)(c.y - lh / 2 + lh - 1) };
            lv_draw_label(d, &n, &a, NUM[i], NULL);
        }
    }
    if (s_compConfig) return;                            // device: the compositor draws the hands
    const ClockFrame &f = s_lvClock;
    if (f.grow > 0.001f) {
        draw_clock_hand(d, f.hourDeg, R * CLOCK_HOUR_LEN * f.grow, CLOCK_HOUR_W);
        draw_clock_hand(d, f.minDeg, R * CLOCK_MIN_LEN * f.grow, CLOCK_MIN_W);
    }
    if (f.leadLen > 0.001f) {
        lv_draw_line_dsc_t s;
        lv_draw_line_dsc_init(&s);
        s.color = clock_lead_color();
        s.width = (lv_coord_t)lroundf(f.leadW);
        s.opa = SWEEP_LEAD_OPA;
        s.round_start = 1;
        s.round_end = 1;
        const lv_point_t tail = rim_point(f.leadDeg + 180.0f, R * f.leadTail);
        const lv_point_t tip = rim_point(f.leadDeg, R * f.leadLen);
        lv_draw_line(d, &s, &tail, &tip);
    }
    if (f.capR > 0) {
        lv_draw_rect_dsc_t cap;
        lv_draw_rect_dsc_init(&cap);
        cap.radius = LV_RADIUS_CIRCLE;
        cap.bg_color = s_cInk;
        cap.bg_opa = LV_OPA_COVER;
        const lv_coord_t r = (lv_coord_t)lroundf(f.capR);
        lv_area_t a = { (lv_coord_t)(s_cx - r), (lv_coord_t)(s_cy - r), (lv_coord_t)(s_cx + r), (lv_coord_t)(s_cy + r) };
        lv_draw_rect(d, &cap, &a);
        cap.bg_color = clock_lead_color();
        cap.bg_opa = (lv_opa_t)lroundf(f.capDot * 255.0f);
        a = { (lv_coord_t)(s_cx - CLOCK_CAP_DOT_R), (lv_coord_t)(s_cy - CLOCK_CAP_DOT_R),
              (lv_coord_t)(s_cx + CLOCK_CAP_DOT_R), (lv_coord_t)(s_cy + CLOCK_CAP_DOT_R) };
        lv_draw_rect(d, &cap, &a);
    }
}

// =============================== helpers =====================================
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                            lv_color_t color, lv_align_t align, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, dx, dy);
    return l;
}

static lv_obj_t *make_layer(lv_obj_t *parent, lv_event_cb_t draw_cb) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_center(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (draw_cb) lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

static void pulse_anim_cb(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;   // the compositor draws it
    const lv_coord_t dia = PULSE_D_MIN + (lv_coord_t)((v * (PULSE_D_MAX - PULSE_D_MIN)) / 100);
    lv_obj_set_size(o, dia, dia);
    lv_obj_center(o);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(PULSE_OPA - v * PULSE_OPA / 100), 0);
}

namespace radar {

void setTheme(int t) {
    s_theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    const bool drg = orb();

    switch (s_theme) {                          // pick the scope chrome palette
        case THEME_AMBER:
            s_cRing = lv_color_hex(0xFFB23C); s_cLead = lv_color_hex(0xFFD27A);
            s_cInk  = lv_color_hex(0xFFE9C2); s_cSoft = lv_color_hex(0xFFC98A); break;
        case THEME_MILITARY:
            s_cRing = lv_color_hex(0x49C46B); s_cLead = lv_color_hex(0x76E08C);
            s_cInk  = lv_color_hex(0xE0FFE6); s_cSoft = lv_color_hex(0x9FD7A8); break;
        default:                                // phosphor (orb uses its own colors)
            s_cRing = COL_GREEN; s_cLead = COL_LEAD; s_cInk = COL_INK; s_cSoft = COL_SOFT; break;
    }

    if (s_parent) {
        if (drg) {
            lv_obj_set_style_bg_color(s_parent, ORB_BG_TOP, 0);
            lv_obj_set_style_bg_grad_color(s_parent, ORB_BG_BOT, 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_VER, 0);
        } else {
            lv_obj_set_style_bg_color(s_parent, lv_color_black(), 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_NONE, 0);
        }
        lv_obj_set_style_bg_opa(s_parent, drg ? s_dimOpa : LV_OPA_COVER, 0);   // Orb's gradient dims behind the clock
    }
    for (int i = 0; i < 4; ++i) show(s_rose[i], !drg);   // hide compass in Orb
    show(s_rangeLbl, !drg && s_rangeLblVisible && s_clockStage == 0);
    show(s_centerDot, !drg);                             // orb draws an orange triangle instead

    // retint the persistent chrome objects for the active palette
    if (s_rose[0]) lv_obj_set_style_text_color(s_rose[0], s_cInk, 0);
    for (int i = 1; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cSoft, 0);
    if (s_centerDot) lv_obj_set_style_bg_color(s_centerDot, s_cInk, 0);
    if (s_pulse)     lv_obj_set_style_border_color(s_pulse, s_cInk, 0);
    if (s_rangeLbl)  lv_obj_set_style_text_color(s_rangeLbl, s_cRing, 0);

    cache_full_rerender();                             // flow colour + chrome follow the theme
    sweep_sync();
    if (s_parent) lv_obj_invalidate(s_parent);
    if (s_themeCb) s_themeCb(s_theme);
}

int  theme() { return s_theme; }
void cycleTheme() { setTheme(s_theme + 1); }
void setThemeChangedCb(void (*cb)(int)) { s_themeCb = cb; }
void setRangeLabelVisible(bool v) { s_rangeLblVisible = v; if (s_rangeLbl) show(s_rangeLbl, v && !orb() && s_clockStage == 0); }

// Show the LVGL sweep and centre ripple, or hand colours + on/off to the compositor when
// it owns them. The ripple pauses while the clock is up (the cap sits there). The lead
// colour is also the clock's second hand, which needs orange in Orb (no sweep there).
static void sweep_sync() {
    const bool clockUp = s_clockStage != 0;
    if (s_compConfig)
        s_compConfig(lv_color_to16(s_cRing), lv_color_to16(clock_lead_color()), lv_color_to16(s_cInk),
                     s_sweepEnabled && !orb(), !orb() && !clockUp);
    if (s_sweep) {
        show(s_sweep, s_sweepEnabled && !s_compConfig);
        lv_obj_invalidate(s_sweep);            // clear any LVGL wedge currently painted
    }
    if (s_pulse) show(s_pulse, !orb() && !s_compConfig && !clockUp);   // orb draws an orange triangle instead
}

void setSweepEnabled(bool on) {
    s_sweepEnabled = on;
    sweep_sync();
}

void setSweepCompositor(void (*config)(uint16_t ring565, uint16_t lead565, uint16_t ink565, bool sweep, bool pulse),
                        void (*tick)(), float (*angle)()) {
    s_compConfig = config;
    s_compTick = tick;
    s_compAngle = angle;
    sweep_sync();
}
bool sweepEnabled() { return s_sweepEnabled; }

void setAirportsEnabled(bool on) {
    s_airportsEnabled = on;
    cache_full_rerender();                             // repaint the chrome with/without markers
}
bool airportsEnabled() { return s_airportsEnabled; }

// 0 = off, 1 = short, 2 = medium (default), 3 = long. Controls both the per-aircraft
// trail and the persistent flow layer (the long-lived "where everything has been" tracks).
void setTrailLength(int level) {
    switch (level) {
        case 0: s_trailMax = 0;  s_flowMax = 0;    s_flowGenMax = 0;  break;
        case 1: s_trailMax = 3;  s_flowMax = 150;  s_flowGenMax = 8;  break;   // ~16 s
        case 3: s_trailMax = 12; s_flowMax = 1500; s_flowGenMax = 30; break;   // ~60 s
        default: s_trailMax = 7; s_flowMax = 700;  s_flowGenMax = 14; break;   // ~28 s
    }
    if (s_flowMax == 0) {
        s_flow.clear();
        for (TrailSlot &t : s_trailStore) { t.used = false; t.hex[0] = 0; t.pts.clear(); }
    } else while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
    cache_full_rerender();                          // repaint the flow layer at the new length
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setMaxOnScreen(int n) {
    s_maxOnScreen = (n < 1) ? 1 : (n > ADSB_MAX_AIRCRAFT ? ADSB_MAX_AIRCRAFT : n);  // never more than the feed pulls
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setLargeText(bool on) {
    s_bigText = on;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

// Scope altitude labels (ft/m). Refreshes the baked label text so a change shows without
// waiting for the next poll.
void setUnits(int preset) {
    const int u = (preset == 1) ? 1 : 0;   // only ft/m differ on the scope
    if (u == s_units) return;
    s_units = u;
    for (AcDraw &a : s_acs) fmt_alt_txt(a.altTxt, sizeof(a.altTxt), a.altFt, a.onGround);
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_acs.clear();
    for (TrailSlot &t : s_trailStore) { t.used = false; t.hex[0] = 0; t.pts.clear(); }
    s_flow.clear();
    s_selHex.clear();

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_cacheBuf) {
        const size_t sz = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_cacheBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_cacheBuf = (lv_color_t *)malloc(sz);
#endif
        if (s_cacheBuf) memset(s_cacheBuf, 0, sz);
    }
    if (s_cacheBuf) {
        memset(&s_cacheDsc, 0, sizeof(s_cacheDsc));
        s_cacheDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_cacheDsc.header.w  = SCREEN_W;
        s_cacheDsc.header.h  = SCREEN_H;
        s_cacheDsc.data_size = (uint32_t)((size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t));
        s_cacheDsc.data      = (const uint8_t *)s_cacheBuf;
    }
    s_bgImg = lv_img_create(parent);              // the scope cache: first child, bottom of the tile
    lv_obj_clear_flag(s_bgImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (s_cacheBuf) lv_img_set_src(s_bgImg, &s_cacheDsc);
    lv_obj_center(s_bgImg);

    s_sweep     = make_layer(parent, sweep_draw_cb);
    s_acLayer   = make_layer(parent, ac_draw_cb);

    s_rose[0] = make_label(parent, "N", &lv_font_montserrat_28, COL_INK,  LV_ALIGN_TOP_MID,    0, 12);
    s_rose[1] = make_label(parent, "S", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_rose[2] = make_label(parent, "E", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_RIGHT_MID, -12, 0);
    s_rose[3] = make_label(parent, "W", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_LEFT_MID,   12, 0);

    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f km", (double)RANGE_KM_DEFAULT);
    s_rangeLbl = make_label(parent, rng, &lv_font_montserrat_14, COL_GREEN, LV_ALIGN_CENTER, 92, -8);
    lv_obj_set_style_text_opa(s_rangeLbl, 128, 0);

    s_pulse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pulse);
    lv_obj_set_size(s_pulse, 12, 12);
    lv_obj_center(s_pulse);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pulse, COL_INK, 0);
    lv_obj_set_style_border_width(s_pulse, PULSE_W, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, PULSE_PERIOD_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_centerDot = lv_obj_create(parent);
    lv_obj_remove_style_all(s_centerDot);
    lv_obj_set_size(s_centerDot, 7, 7);
    lv_obj_center(s_centerDot);
    lv_obj_set_style_radius(s_centerDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_centerDot, COL_INK, 0);
    lv_obj_set_style_bg_opa(s_centerDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_centerDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_clockLayer = make_layer(parent, clock_draw_cb);   // idle clock: on top of the scope
    show(s_clockLayer, false);

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);

    setTheme(s_theme);
}

void update(const std::vector<Aircraft> &aircraft, const RadarSettings &s) {
    static std::vector<AcDraw> out;         // reused across polls (no per-poll allocation)
    out.clear();
    const float R = (float)RADAR_R_OUTER_PX;
    ++s_flowGen;                            // one tick per poll; flow segments age in these units

    // Reproject the coastline only when the scope geometry actually changes (home
    // moved or range zoomed) — never per frame. Then re-render the whole scope cache.
    static double s_coLat = 1e9, s_coLon = 1e9; static float s_coRange = -1.0f;
    bool fullLayer = false;
    if (s.homeLat != s_coLat || s.homeLon != s_coLon || s.rangeKm != s_coRange) {
        const bool firstFix = (s_coRange < 0.0f);
        s_coLat = s.homeLat; s_coLon = s.homeLon; s_coRange = s.rangeKm;
        coastline_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        airports_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        if (!firstFix) {
            // Scope scale/center changed: old trails were plotted at the previous
            // projection and would be wrong now — drop them and clear the flow layer.
            for (TrailSlot &t : s_trailStore) { t.used = false; t.hex[0] = 0; t.pts.clear(); }
            s_flow.clear();
            fullLayer = true;               // every glyph jumps: repaint the whole aircraft layer
        }
        cache_full_rerender();
    }

    // What is on screen right now (old list, old selection, old trails — before this
    // poll touches anything): per-hex unions of everything each drawn aircraft paints.
    static HexArea before[ADSB_MAX_AIRCRAFT];
    const int nBefore = fullLayer ? 0 : collect_drawn_bounds(before, ADSB_MAX_AIRCRAFT);

    // Flow segments this poll adds or prunes dirty exactly their rects in the cache.
    static lv_area_t flowDirty[ADSB_MAX_AIRCRAFT];
    int nFlow = 0;
    auto addFlowRect = [&](const FlowSeg &seg) {
        if (nFlow < ADSB_MAX_AIRCRAFT) flow_seg_rect(seg, flowDirty[nFlow++]);
        else { lv_area_t r; flow_seg_rect(seg, r); area_union(flowDirty[0], r); }
    };

    comp_tick();
    for (const Aircraft &ac : aircraft) {
        comp_tick();
        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const geo::Point p = geo::projectToScreen(distKm, brg, s.rangeKm, s_cx, s_cy, R, s.rotationDeg);

        AcDraw d;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;
        {
            const AcDraw *prev = nullptr;   // smooth-motion: the glide starts at the old glyph
            for (const AcDraw &o : s_acs)
                if (strcmp(o.hex, ac.hex.c_str()) == 0) { prev = &o; break; }
            if (prev) {
                const long dx = (long)target.x - prev->pos.x;
                const long dy = (long)target.y - prev->pos.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : prev->pos;  // snap if it jumped
            } else d.from = target;                                              // new contact: appear in place
        }
#if MOTION_INTERP
        d.pos = d.from;                  // begin the glide at the previous position
#else
        d.pos = target;
        d.from = target;
#endif
        d.inRange = p.inRange;
        d.track = ac.track;
        d.color = alt_color(ac.altBaro, ac.onGround);
        d.emergency = acIsEmergency(ac.squawk);
        snprintf(d.hex,  sizeof(d.hex),  "%s", ac.hex.c_str());
        snprintf(d.call, sizeof(d.call), "%s", ac.flight.c_str());
        snprintf(d.type, sizeof(d.type), "%s", ac.type.c_str());
        d.altFt = ac.altBaro;
        d.onGround = ac.onGround;
        d.vsFpm = ac.baroRate;
        d.gsKt = ac.gs;
        d.distKm = (float)distKm;
        d.bearingDeg = (float)brg;
        d.squawk = ac.squawk;
        d.trail = nullptr;
        fmt_alt_txt(d.altTxt, sizeof(d.altTxt), ac.altBaro, ac.onGround);

        if (d.inRange) {
            TrailSlot *slot = trail_slot(ac.hex.c_str());
            if (slot) {
                if (!slot->used) { slot->used = true; snprintf(slot->hex, sizeof(slot->hex), "%s", ac.hex.c_str()); }
                std::vector<lv_point_t> &hist = slot->pts;
                const bool moved = hist.empty() ||
                                   abs((int)hist.back().x - (int)target.x) > 0 ||
                                   abs((int)hist.back().y - (int)target.y) > 0;
                if (moved) {
                    if (s_flowMax > 0 && !hist.empty()) {
                        FlowSeg seg = { hist.back(), target, s_flowGen };
                        s_flow.push_back(seg);
                        while ((int)s_flow.size() > s_flowMax) { addFlowRect(s_flow.front()); s_flow.pop_front(); }
                        addFlowRect(seg);
                    }
                    if (s_trailMax > 0) {
                        hist.push_back(target);
                        while ((int)hist.size() > s_trailMax) hist.erase(hist.begin());
                    } else {
                        hist.clear();
                    }
                }
                d.trail = &hist;
            }
        }
        out.push_back(std::move(d));
    }

    for (TrailSlot &t : s_trailStore) {    // forget the histories of contacts that left
        if (!t.used) continue;
        bool present = false;
        for (const Aircraft &ac : aircraft)
            if (strcmp(ac.hex.c_str(), t.hex) == 0) { present = true; break; }
        if (!present) { t.used = false; t.hex[0] = 0; t.pts.clear(); }
    }
    if (!s_selHex.empty()) {
        bool selPresent = false;
        for (const Aircraft &ac : aircraft)
            if (s_selHex == ac.hex.c_str()) { selPresent = true; break; }
        if (!selPresent) s_selHex.clear();
    }

    // Fade the flow layer by AGE, not just count: drop segments older than s_flowGenMax
    // polls so old tracks self-clear even in busy airspace (a 5 nm view doesn't stay caked
    // in green). Each dropped segment dirties exactly its rect in the cache.
    if (s_flowGenMax > 0 && !s_flow.empty()) {
        while (!s_flow.empty() && (uint16_t)(s_flowGen - s_flow.front().gen) > (uint16_t)s_flowGenMax) {
            addFlowRect(s_flow.front());
            s_flow.pop_front();
        }
    }

    // nearest first (the blips + the list). s_maxOnScreen is applied only when drawing /
    // hit-testing (the nearest N) — count/list/stats/info must see the full set.
    std::sort(out.begin(), out.end(),
              [](const AcDraw &a, const AcDraw &b) { return a.distKm < b.distKm; });

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        snprintf(r, sizeof(r), "%.0f km", (double)s.rangeKm);
        if (strcmp(lv_label_get_text(s_rangeLbl), r) != 0) lv_label_set_text(s_rangeLbl, r);
    }

    const uint32_t now = lv_tick_get();              // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)POLL_INTERVAL_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    s_acs.swap(out);                                 // out keeps the old buffer for the next poll

    // Invalidate only what a poll changed on the aircraft layer: per hex, the union of
    // the drawn bounds before and after (glyph + rings/waves, trail).
    if (s_acLayer && !fullLayer) {
        static HexArea after[ADSB_MAX_AIRCRAFT];
        const int nAfter = collect_drawn_bounds(after, ADSB_MAX_AIRCRAFT);
        static lv_area_t inv[2 * ADSB_MAX_AIRCRAFT];
        int nInv = 0;
        for (int i = 0; i < nBefore; ++i) {
            int j;
            for (j = 0; j < nAfter; ++j)
                if (strcmp(after[j].hex, before[i].hex) == 0) { area_union(before[i].a, after[j].a); break; }
            inv[nInv++] = before[i].a;                // left, moved, or re-labelled
        }
        for (int j = 0; j < nAfter; ++j) {
            bool seen = false;
            for (int i = 0; i < nBefore; ++i)
                if (strcmp(before[i].hex, after[j].hex) == 0) { seen = true; break; }
            if (!seen) inv[nInv++] = after[j].a;     // newly drawn this poll
        }
        nInv = inv_merge(inv, nInv);
        for (int i = 0; i < nInv; ++i) lv_obj_invalidate_area(s_acLayer, &inv[i]);
    } else if (s_acLayer) {
        lv_obj_invalidate(s_acLayer);
    }

    // Re-render the cache where flow segments were added or pruned, and show the change.
    nFlow = inv_merge(flowDirty, nFlow);
    for (int i = 0; i < nFlow; ++i) {
        cache_render_rect(&flowDirty[i]);
        if (s_bgImg) lv_obj_invalidate_area(s_bgImg, &flowDirty[i]);
    }
}

int hitTest(int x, int y) {
    int best = -1;
    long bestD = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
    for_each_drawn([&](size_t i, const AcDraw &ac) {   // exactly what ac_draw_cb drew
        const long dx = (long)ac.pos.x - x;
        const long dy = (long)ac.pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    });
    return best;
}

static void fill_info(const AcDraw &a, AcInfo &out) {
    snprintf(out.hex, sizeof(out.hex), "%s", a.hex);
    snprintf(out.call, sizeof(out.call), "%s", a.call);
    snprintf(out.type, sizeof(out.type), "%s", a.type);
    out.altFt = a.altFt; out.onGround = a.onGround;
    out.vsFpm = a.vsFpm; out.gsKt = a.gsKt;
    out.distKm = a.distKm; out.bearingDeg = a.bearingDeg; out.track = a.track;
    out.squawk = a.squawk; out.emergency = a.emergency;
}

void select(int idx) {
    if (idx < 0 || idx >= (int)s_acs.size()) s_selHex.clear();
    else s_selHex = s_acs[idx].hex;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

bool selectHex(const char *hex) {
    for (const AcDraw &a : s_acs)
        if (strcmp(a.hex, hex) == 0) {
            s_selHex = a.hex;
            if (s_acLayer) lv_obj_invalidate(s_acLayer);
            return true;
        }
    return false;
}

bool selected(AcInfo &out) {
    if (s_selHex.empty()) return false;
    for (const AcDraw &a : s_acs)
        if (s_selHex == a.hex) { fill_info(a, out); return true; }
    return false;
}

int count() { return (int)s_acs.size(); }

int countInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange) ++n;
    return n;
}

bool info(int idx, AcInfo &out) {
    if (idx < 0 || idx >= (int)s_acs.size()) return false;
    fill_info(s_acs[idx], out);
    return true;
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

// ---- idle clock ----
bool setClock(bool on) {
    const uint32_t now = lv_tick_get();
    if (!on) {
        if (s_clock.requested()) s_clock.exit(now, clock_wall());
        return true;
    }
    if (s_clock.requested()) return true;
    ClockHands wall;
    if (!s_clockTime(wall)) return false;                // time unknown: no clock face
    s_wall = wall;
    // Morph from whichever sweep is on screen: the compositor's, or LVGL's in the simulator.
    const bool comp = s_compConfig != nullptr;
    const float deg = comp ? (s_compAngle ? s_compAngle() : 0.0f) : s_sweepDeg;
    const float dps = 360000.0f / (float)(comp ? SWEEP_COMP_PERIOD_MS : SWEEP_PERIOD_MS);
    return s_clock.enter(now, deg, dps, s_sweepEnabled && !orb(), wall);   // false while still leaving
}

bool clockRequested() { return s_clock.requested(); }

float clockProgress() {
    if (s_clock.phase() == ClockFace::OFF) return 0.0f;
    return s_clock.frame(lv_tick_get(), clock_wall()).p;
}

bool clockFrame(ClockFrame &out) {
    if (s_clock.phase() == ClockFace::OFF) return false;
    out = s_clock.frame(lv_tick_get(), clock_wall());
    return true;
}

bool emergencyInRange() {
    for (const AcDraw &a : s_acs)
        if (a.inRange && a.emergency) return true;
    return false;
}

void setClockTimeSource(bool (*fn)(ClockHands &out)) { s_clockTime = fn ? fn : clock_time_now; }
void setClockStageCb(void (*cb)(int stage)) { s_clockStageCb = cb; }

} // namespace radar
