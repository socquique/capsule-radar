// Radar scope (M1) + aircraft (M2) + selection (M3) + selectable themes (M4).
// Pure LVGL, portable. Visual reference: assets/plane_radar_2.0_mockup.html
//   THEME_PHOSPHOR : green-on-black radar scope (rings, sweep, altitude glyphs)
//   THEME_DRAGON   : DBZ "Dragon Radar": green gradient, square grid, the 7 nearest
//                    aircraft as yellow balls (emitting waves) + off-range arrows.
#include "radar_view.h"
#include "config.h"
#include "geo.h"
#include "coastline.h"
#include "airports.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include <stdlib.h>
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
#define COL_MIL    lv_color_hex(0xFFB23C)
// coastline outline — steel blue, deliberately off the red/amber/lime/green/cyan
// altitude-trail palette so land never reads as an aircraft track.
#define COAST_COLOR lv_color_hex(0x4E86C6)
// airport markers — a neutral muted grey-blue so they sit quietly under the traffic.
#define AIRPORT_COLOR lv_color_hex(0x8A93A6)
// ---- dragon palette (DBZ) ----
#define DRG_BLIP   lv_color_hex(0xFFE11A)
#define DRG_EMERG  lv_color_hex(0xFF4D2E)
#define DRG_ACCENT lv_color_hex(0xFF8A1E)
#define DRG_GRID   lv_color_hex(0x3F8B30)
#define DRG_BG_TOP lv_color_hex(0x18540F)
#define DRG_BG_BOT lv_color_hex(0x09250A)
#define DRG_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config ----
#define SWEEP_PERIOD_MS   10000
#define SWEEP_FRAME_MS    20
#define SWEEP_MAX_ELAPSED_MS 50
#define SWEEP_TRAIL_DEG   24.0f
#define SWEEP_TRAIL_STEPS 8
#define SWEEP_TRAIL_OPA   72

// ---- aircraft / flow / dragon config ----
#define TRAIL_MAX         10
#define TRAIL_TTL_MS      180000UL
#define TAP_RADIUS_PX     40    // generous finger-tap catch radius (picks the nearest glyph within it)
#define FLOW_CANVAS_ENABLED 1    // dim fading "frequently traveled path" layer
#define FLOW_MAX          700
#define FLOW_TTL_MS       180000UL
#define FLOW_OPA          58
#define DRAGON_BALLS      7
#define DRAGON_ARROWS     8
#define BALL_R            9
#define WAVE_EXPAND       28.0f

static int        s_theme    = THEME_PHOSPHOR;
static void      (*s_themeCb)(int) = nullptr;
// scope "chrome" palette (rings/sweep/crosshair/labels) — retinted per theme
static lv_color_t s_cRing = COL_GREEN, s_cLead = COL_LEAD, s_cInk = COL_INK, s_cSoft = COL_SOFT;
static lv_obj_t  *s_parent   = nullptr;
static lv_obj_t  *s_gridLayer = nullptr;
static lv_obj_t  *s_sweep     = nullptr;
static lv_obj_t  *s_acLayer   = nullptr;
static lv_obj_t  *s_flowCanvas = nullptr;
static lv_color_t *s_flowBuf  = nullptr;
static lv_obj_t  *s_rose[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t  *s_centerDot = nullptr;
static lv_obj_t  *s_pulse     = nullptr;
static lv_obj_t  *s_rangeLbl  = nullptr;
static bool       s_rangeLblVisible = true;
static bool       s_sweepEnabled    = true;
static bool       s_airportsEnabled = true;
static bool       s_groundAircraftEnabled = false;
static lv_timer_t *s_timer    = nullptr;
static lv_anim_t   s_pulseAnim;
static bool        s_pulseAnimRunning = false;
static float       s_sweepDeg = 0.0f;
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastSweepTickMs = 0;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = POLL_INTERVAL_MS;
static uint32_t    s_lastFlowRefreshMs = 0;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static std::string s_selHex;

struct FlowSeg { lv_point_t a, b; uint32_t bornMs; };
static FlowSeg *s_flow = nullptr;
static uint16_t s_flowHead = 0;
static uint16_t s_flowCount = 0;

struct TrailPt { lv_point_t p; uint32_t bornMs; };

struct AcDraw {
    lv_point_t pos;            // current (animated) screen position — what gets drawn
    lv_point_t from, to;       // smooth-motion glide endpoints (M4 interpolation)
    float      track;
    lv_color_t color;
    bool       emergency;
    bool       military;
    bool       inRange;
    double     lat, lon;
    char       hex[8];
    char       call[12];
    char       type[8];
    char       altTxt[12];
    float      altFt;
    bool       onGround;
    float      vsFpm, gsKt, distKm, bearingDeg;
    int        squawk;
    TrailPt    trail[TRAIL_MAX];
    uint8_t    trailCount;
};
static std::vector<AcDraw> s_acs;
using HexKey = std::array<char, 8>;
struct TrailTrack {
    char hex[8];
    TrailPt pts[TRAIL_MAX];
    uint8_t count;
    bool seen;
};
static std::vector<TrailTrack> s_trails;

static const float GX[4] = { 0.0f,  7.0f, 0.0f, -7.0f };
static const float GY[4] = { -11.0f, 5.0f, 8.0f, 5.0f };

static inline bool dragon() { return s_theme == THEME_DRAGON; }

static bool contains_hex(const std::vector<HexKey> &keys, const char *hex) {
    for (const HexKey &key : keys) {
        if (strcmp(key.data(), hex) == 0) return true;
    }
    return false;
}

static TrailTrack *find_trail(const char *hex) {
    for (TrailTrack &t : s_trails) {
        if (strcmp(t.hex, hex) == 0) return &t;
    }
    return nullptr;
}

static TrailTrack *find_or_add_trail(const char *hex) {
    if (TrailTrack *t = find_trail(hex)) return t;
    TrailTrack t{};
    snprintf(t.hex, sizeof(t.hex), "%s", hex ? hex : "");
    s_trails.push_back(t);
    return &s_trails.back();
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

// =============================== flow map ====================================
static FlowSeg &flow_at(uint16_t i) {
    return s_flow[(uint16_t)((s_flowHead + i) % FLOW_MAX)];
}

static void flow_clear(void) {
    s_flowHead = 0;
    s_flowCount = 0;
}

static void flow_prune(uint32_t now) {
#if FLOW_CANVAS_ENABLED
    if (!s_flow) return;
    while (s_flowCount > 0 && (uint32_t)(now - flow_at(0).bornMs) > FLOW_TTL_MS) {
        s_flowHead = (uint16_t)((s_flowHead + 1) % FLOW_MAX);
        s_flowCount--;
    }
#else
    (void)now;
#endif
}

static void flow_push(const FlowSeg &seg) {
#if FLOW_CANVAS_ENABLED
    if (!s_flow) return;
    if (s_flowCount >= FLOW_MAX) {
        s_flowHead = (uint16_t)((s_flowHead + 1) % FLOW_MAX);
        s_flowCount--;
    }
    s_flow[(uint16_t)((s_flowHead + s_flowCount) % FLOW_MAX)] = seg;
    s_flowCount++;
#else
    (void)seg;
#endif
}

static void flow_draw_seg(const FlowSeg &s, uint32_t now) {
#if FLOW_CANVAS_ENABLED
    if (!s_flowCanvas) return;
    const uint32_t age = now - s.bornMs;
    if (age > FLOW_TTL_MS) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = dragon() ? DRG_FLOW : s_cRing;
    d.width = 3;
    d.opa = (lv_opa_t)(FLOW_OPA - (FLOW_OPA * age / FLOW_TTL_MS));
    if (d.opa < 3) return;
    lv_point_t pts[2] = { s.a, s.b };
    lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);
#else
    (void)s;
    (void)now;
#endif
}

static void flow_redraw_all(void) {
#if FLOW_CANVAS_ENABLED
    if (!s_flowCanvas) return;
    const uint32_t now = lv_tick_get();
    flow_prune(now);
    lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    for (uint16_t i = 0; i < s_flowCount; ++i) flow_draw_seg(flow_at(i), now);
#endif
}

// =============================== grid ========================================
static void grid_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const lv_point_t c = { s_cx, s_cy };

    if (dragon()) {
        lv_draw_line_dsc_t gl;
        lv_draw_line_dsc_init(&gl);
        gl.color = DRG_GRID;
        gl.width = 1;
        gl.opa = 120;
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
        td.bg_color = DRG_ACCENT;
        td.bg_opa = LV_OPA_COVER;
        td.border_color = lv_color_hex(0x8A4A00);
        td.border_width = 1;
        td.border_opa = 160;
        coastline_draw(d, COAST_COLOR, 170, 2);    // landmass outline under the triangle
        lv_draw_polygon(d, &td, tri, 3);
        if (s_airportsEnabled) airports_draw(d, AIRPORT_COLOR, 150);
        return;
    }

    // coastline first, so the rings/crosshair sit cleanly on top of it.
    // Steel blue + 2 px so it reads as a map outline, distinct from the green altitude trails.
    coastline_draw(d, COAST_COLOR, 165, 2);

    // phosphor: concentric rings + crosshair
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = s_cRing;
    ad.width = 2;
    const lv_coord_t rr[4] = { 50, 104, 160, RADAR_R_OUTER_PX };
    const lv_opa_t   ro[4] = { 66, 66, 66, 87 };
    for (int i = 0; i < 4; ++i) { ad.opa = ro[i]; lv_draw_arc(d, &ad, &c, rr[i], 0, 360); }

    lv_draw_line_dsc_t ll;
    lv_draw_line_dsc_init(&ll);
    ll.color = s_cRing;
    ll.width = 2;
    ll.opa = 41;
    lv_point_t h1 = { (lv_coord_t)(s_cx - 211), s_cy }, h2 = { (lv_coord_t)(s_cx + 211), s_cy };
    lv_point_t v1 = { s_cx, (lv_coord_t)(s_cy - 211) }, v2 = { s_cx, (lv_coord_t)(s_cy + 211) };
    lv_draw_line(d, &ll, &h1, &h2);
    lv_draw_line(d, &ll, &v1, &v2);

    if (s_airportsEnabled) airports_draw(d, AIRPORT_COLOR, 150);
}

// =============================== sweep =======================================
static void sweep_draw_cb(lv_event_t *e) {
    if (dragon()) return;
    lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);
    const lv_point_t center = { s_cx, s_cy };
    const float R = (float)RADAR_R_OUTER_PX;

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = s_cRing;
    ld.width = 3;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int i = SWEEP_TRAIL_STEPS; i >= 1; --i) {
        const float frac = 1.0f - (float)i / (float)SWEEP_TRAIL_STEPS;
        const float ang  = s_sweepDeg - (float)i * (SWEEP_TRAIL_DEG / (float)SWEEP_TRAIL_STEPS);
        ld.opa = (lv_opa_t)(frac * frac * (float)SWEEP_TRAIL_OPA);
        if (ld.opa < 2) continue;
        lv_point_t p2 = rim_point(ang, R);
        lv_draw_line(dctx, &ld, &center, &p2);
    }
    lv_draw_line_dsc_t le;
    lv_draw_line_dsc_init(&le);
    le.color = s_cLead;
    le.width = 2;
    le.opa = 217;
    le.round_start = 1;
    le.round_end = 1;
    lv_point_t lead = rim_point(s_sweepDeg, R);
    lv_draw_line(dctx, &le, &center, &lead);
}

static void wedge_bbox(float deg, lv_area_t *out) {
    lv_coord_t minx = s_cx, maxx = s_cx, miny = s_cy, maxy = s_cy;
    const int steps = 6;
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

// glyph + label bounding box (for partial invalidation during the glide)
static inline lv_area_t glyph_bbox(lv_point_t p) {
    lv_area_t a;
    if (dragon()) { a.x1 = p.x - 30; a.y1 = p.y - 30; a.x2 = p.x + 30;  a.y2 = p.y + 30; }
    else          { a.x1 = p.x - 22; a.y1 = p.y - 22; a.x2 = p.x + 148; a.y2 = p.y + 26; }
    return a;
}
static inline void area_union(lv_area_t &d, const lv_area_t &s) {
    d.x1 = LV_MIN(d.x1, s.x1); d.y1 = LV_MIN(d.y1, s.y1);
    d.x2 = LV_MAX(d.x2, s.x2); d.y2 = LV_MAX(d.y2, s.y2);
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

static void sweep_timer_cb(lv_timer_t *t) {
    (void)t;
    const uint32_t now = lv_tick_get();
    if (s_lastSweepTickMs == 0) s_lastSweepTickMs = now;
    uint32_t elapsed = now - s_lastSweepTickMs;
    s_lastSweepTickMs = now;
    if (elapsed > SWEEP_MAX_ELAPSED_MS) elapsed = SWEEP_MAX_ELAPSED_MS; // slow instead of jumping after a stall

    if (++s_frameCtr % 4 == 0) interp_step();         // smooth glyph motion (~80 ms cadence)
    if (dragon()) {
        // animate the dragon-ball waves (invalidate only the ball areas)
        s_wavePhase += (float)elapsed / 600.0f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_acLayer) return;
        int balls = 0;
        for (const AcDraw &ac : s_acs) {
            if (!ac.inRange) continue;
            if (balls >= DRAGON_BALLS) break;
            balls++;
            lv_area_t a = { (lv_coord_t)(ac.pos.x - 44), (lv_coord_t)(ac.pos.y - 44),
                            (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y + 44) };
            lv_obj_invalidate_area(s_acLayer, &a);
        }
        return;
    }
    if (!s_sweepEnabled) return;          // sweep disabled: glyph interpolation above still runs
    s_prevSweepDeg = s_sweepDeg;
    s_sweepDeg += 360.0f * (float)elapsed / (float)SWEEP_PERIOD_MS;
    while (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
    if (!s_sweep) return;
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
    const int n = (int)ac.trailCount;
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 1;
    for (int i = 1; i < n; ++i) {
        t.opa = (lv_opa_t)(6 + 28 * i / n);
        lv_point_t a = ac.trail[i - 1].p, b = ac.trail[i].p;
        lv_draw_line(d, &t, &a, &b);
    }
}

static bool alert_style(const AcDraw &ac, const char **label, lv_color_t *color);

static void draw_ball(lv_draw_ctx_t *d, const AcDraw &ac) {
    // emitted waves: several expanding rings (sonar-ping look)
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = DRG_ACCENT;
    w.width = 3;
    for (int wv = 0; wv < 3; ++wv) {
        float ph = s_wavePhase + (float)wv * 0.34f;
        if (ph >= 1.0f) ph -= 1.0f;
        w.opa = (lv_opa_t)((1.0f - ph) * 245.0f);
        if (w.opa > 6) lv_draw_arc(d, &w, &ac.pos, (uint16_t)(BALL_R + 3 + ph * WAVE_EXPAND), 0, 360);
    }

    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    lv_color_t alertCol = DRG_BLIP;
    const bool alert = alert_style(ac, nullptr, &alertCol);
    b.bg_color = alert ? alertCol : DRG_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    b.border_color = lv_color_hex(0x7A5A00);
    b.border_width = 1;
    b.border_opa = 150;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - BALL_R), (lv_coord_t)(ac.pos.y - BALL_R),
                    (lv_coord_t)(ac.pos.x + BALL_R), (lv_coord_t)(ac.pos.y + BALL_R) };
    lv_draw_rect(d, &b, &r);

    // glossy highlight
    lv_draw_rect_dsc_t hl;
    lv_draw_rect_dsc_init(&hl);
    hl.bg_color = lv_color_hex(0xFFFBCC);
    hl.bg_opa = 170;
    hl.radius = LV_RADIUS_CIRCLE;
    lv_area_t hr = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 6),
                     (lv_coord_t)(ac.pos.x - 1), (lv_coord_t)(ac.pos.y - 2) };
    lv_draw_rect(d, &hl, &hr);
}

static void draw_offrange(lv_draw_ctx_t *d, const AcDraw &ac) {
    // small ball at the rim
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    lv_color_t alertCol = DRG_BLIP;
    const bool alert = alert_style(ac, nullptr, &alertCol);
    b.bg_color = alert ? alertCol : DRG_BLIP;
    b.bg_opa = LV_OPA_COVER;
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
    td.bg_color = DRG_ACCENT;
    td.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &td, tri, 3);
}

static bool alert_style(const AcDraw &ac, const char **label, lv_color_t *color) {
    if (ac.emergency) {
        if (label) *label = "EMR";
        if (color) *color = COL_EMERG;
        return true;
    }
    if (ac.military) {
        if (label) *label = "MIL";
        if (color) *color = COL_MIL;
        return true;
    }
    return false;
}

static void draw_alert_badge(lv_draw_ctx_t *d, const AcDraw &ac, bool dragonTheme) {
    const char *txt = nullptr;
    lv_color_t col = COL_EMERG;
    if (!alert_style(ac, &txt, &col)) return;

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = col;
    ring.width = dragonTheme ? 3 : 2;
    ring.opa = LV_OPA_COVER;
    lv_draw_arc(d, &ring, &ac.pos, dragonTheme ? 20 : 17, 0, 360);
    if (ac.emergency) lv_draw_arc(d, &ring, &ac.pos, dragonTheme ? 29 : 25, 0, 360);

    lv_draw_rect_dsc_t chip;
    lv_draw_rect_dsc_init(&chip);
    chip.bg_color = lv_color_black();
    chip.bg_opa = 190;
    chip.border_color = col;
    chip.border_opa = 210;
    chip.border_width = 1;
    chip.radius = 4;
    lv_area_t ca = { (lv_coord_t)(ac.pos.x + 13), (lv_coord_t)(ac.pos.y - 32),
                     (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y - 15) };
    lv_draw_rect(d, &chip, &ca);

    lv_draw_label_dsc_t lb;
    lv_draw_label_dsc_init(&lb);
    lb.font = &lv_font_montserrat_12;
    lb.color = col;
    lb.opa = LV_OPA_COVER;
    lv_area_t la = { (lv_coord_t)(ca.x1 + 4), (lv_coord_t)(ca.y1 + 2),
                     (lv_coord_t)(ca.x2 + 18), (lv_coord_t)(ca.y2 + 2) };
    lv_draw_label(d, &lb, &la, txt, NULL);
}

static void ac_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const bool drg = dragon();
    int balls = 0, arrows = 0;

    for (const AcDraw &ac : s_acs) {
        if (drg) {
            if (ac.inRange) {
                if (balls >= DRAGON_BALLS) continue;   // up to 7 in-range balls
                draw_trail(d, ac, DRG_FLOW);
                draw_ball(d, ac);
                draw_alert_badge(d, ac, true);
                balls++;
            } else {
                if (arrows >= DRAGON_ARROWS) continue;  // up to 8 off-range arrows
                draw_offrange(d, ac);
                arrows++;
            }
        } else {
            if (!ac.inRange) continue;            // phosphor shows in-range traffic only
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
            g.bg_color = ac.color;
            g.bg_opa = LV_OPA_COVER;
            lv_draw_polygon(d, &g, pts, 4);
            draw_alert_badge(d, ac, false);
        }

        // selection ring(s)
        if (!s_selHex.empty() && s_selHex == ac.hex) {
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.width = 2;
            sr.opa = 240;
            if (drg) {
                sr.color = DRG_ACCENT;
                lv_draw_arc(d, &sr, &ac.pos, 15, 0, 360);
                lv_draw_arc(d, &sr, &ac.pos, 23, 0, 360);
            } else {
                lv_color_t alertCol = s_cInk;
                sr.color = alert_style(ac, nullptr, &alertCol) ? alertCol : s_cInk;
                lv_draw_arc(d, &sr, &ac.pos, 19, 0, 360);
            }
        }

        // floating labels (phosphor only; dragon keeps clean balls + the tap card)
        if (!drg) {
            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = &lv_font_montserrat_14;
            lv_color_t alertCol = s_cInk;
            const bool alert = alert_style(ac, nullptr, &alertCol);
            lc.color = alert ? alertCol : s_cInk;
            lv_area_t a1 = { (lv_coord_t)(ac.pos.x + 12), (lv_coord_t)(ac.pos.y - 14),
                             (lv_coord_t)(ac.pos.x + 142), (lv_coord_t)(ac.pos.y + 2) };
            if (ac.call[0]) lv_draw_label(d, &lc, &a1, ac.call, NULL);
            lv_draw_label_dsc_t la;
            lv_draw_label_dsc_init(&la);
            la.font = &lv_font_montserrat_12;
            la.color = ac.color;
            lv_area_t a2 = { a1.x1, (lv_coord_t)(ac.pos.y + 2), a1.x2, (lv_coord_t)(ac.pos.y + 20) };
            if (ac.altTxt[0]) lv_draw_label(d, &la, &a2, ac.altTxt, NULL);
        }
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
    const lv_coord_t dia = 10 + (lv_coord_t)((v * 44) / 100);
    lv_obj_set_size(o, dia, dia);
    lv_obj_center(o);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 220 / 100), 0);
}

static void start_pulse_anim(void) {
    if (!s_pulse || s_pulseAnimRunning) return;
    lv_anim_init(&s_pulseAnim);
    lv_anim_set_var(&s_pulseAnim, s_pulse);
    lv_anim_set_exec_cb(&s_pulseAnim, pulse_anim_cb);
    lv_anim_set_values(&s_pulseAnim, 0, 100);
    lv_anim_set_time(&s_pulseAnim, 2600);
    lv_anim_set_repeat_count(&s_pulseAnim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_pulseAnim);
    s_pulseAnimRunning = true;
}

static void stop_pulse_anim(void) {
    if (!s_pulse) return;
    lv_anim_del(s_pulse, pulse_anim_cb);
    s_pulseAnimRunning = false;
    lv_obj_set_size(s_pulse, 12, 12);
    lv_obj_center(s_pulse);
    lv_obj_set_style_border_opa(s_pulse, LV_OPA_COVER, 0);
}

namespace radar {

void setTheme(int t) {
    s_theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    const bool drg = dragon();

    switch (s_theme) {                          // pick the scope chrome palette
        case THEME_AMBER:
            s_cRing = lv_color_hex(0xFFB23C); s_cLead = lv_color_hex(0xFFD27A);
            s_cInk  = lv_color_hex(0xFFE9C2); s_cSoft = lv_color_hex(0xFFC98A); break;
        case THEME_MILITARY:
            s_cRing = lv_color_hex(0x49C46B); s_cLead = lv_color_hex(0x76E08C);
            s_cInk  = lv_color_hex(0xE0FFE6); s_cSoft = lv_color_hex(0x9FD7A8); break;
        default:                                // phosphor (dragon uses its own colors)
            s_cRing = COL_GREEN; s_cLead = COL_LEAD; s_cInk = COL_INK; s_cSoft = COL_SOFT; break;
    }

    if (s_parent) {
        if (drg) {
            lv_obj_set_style_bg_color(s_parent, DRG_BG_TOP, 0);
            lv_obj_set_style_bg_grad_color(s_parent, DRG_BG_BOT, 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_VER, 0);
        } else {
            lv_obj_set_style_bg_color(s_parent, lv_color_black(), 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_NONE, 0);
        }
        lv_obj_set_style_bg_opa(s_parent, LV_OPA_COVER, 0);
    }
    for (int i = 0; i < 4; ++i) show(s_rose[i], !drg);   // hide compass in DBZ
    show(s_rangeLbl, !drg && s_rangeLblVisible);
    show(s_centerDot, !drg);                             // dragon draws an orange triangle instead
    show(s_pulse, !drg && s_sweepEnabled);
    if (s_sweepEnabled && !drg) start_pulse_anim();
    else stop_pulse_anim();

    // retint the persistent chrome objects for the active palette
    for (int i = 0; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cInk, 0);
    if (s_centerDot) lv_obj_set_style_bg_color(s_centerDot, s_cInk, 0);
    if (s_pulse)     lv_obj_set_style_border_color(s_pulse, s_cInk, 0);
    if (s_rangeLbl)  lv_obj_set_style_text_color(s_rangeLbl, s_cRing, 0);

    flow_redraw_all();
    if (s_parent) lv_obj_invalidate(s_parent);
    if (s_themeCb) s_themeCb(s_theme);
}

int  theme() { return s_theme; }
void cycleTheme() { setTheme(s_theme + 1); }
void setThemeChangedCb(void (*cb)(int)) { s_themeCb = cb; }
void setRangeLabelVisible(bool v) { s_rangeLblVisible = v; if (s_rangeLbl) show(s_rangeLbl, v && !dragon()); }

void setSweepEnabled(bool on) {
    s_sweepEnabled = on;
    if (s_sweep) {
        show(s_sweep, on);
        if (!on) lv_obj_invalidate(s_sweep);   // clear any wedge currently painted
    }
    if (s_pulse) {
        show(s_pulse, on && !dragon());
        if (on && !dragon()) start_pulse_anim();
        else stop_pulse_anim();
    }
}
bool sweepEnabled() { return s_sweepEnabled; }

void setAirportsEnabled(bool on) {
    s_airportsEnabled = on;
    if (s_gridLayer) lv_obj_invalidate(s_gridLayer);   // repaint the chrome with/without markers
}
bool airportsEnabled() { return s_airportsEnabled; }

void setGroundAircraftEnabled(bool on) {
    s_groundAircraftEnabled = on;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}
bool groundAircraftEnabled() { return s_groundAircraftEnabled; }

void setAircraftLayerVisible(bool on) {
    show(s_acLayer, on);
}

void setMapPanOffset(int dx, int dy) {
    coastline_set_offset((lv_coord_t)dx, (lv_coord_t)dy);
    airports_set_offset((lv_coord_t)dx, (lv_coord_t)dy);
    if (s_gridLayer) lv_obj_invalidate(s_gridLayer);
}

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_acs.clear();
    s_trails.clear();
    flow_clear();
    s_selHex.clear();
    s_acs.reserve(ADSB_MAX_AIRCRAFT);
    s_trails.reserve(ADSB_MAX_AIRCRAFT);

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (FLOW_CANVAS_ENABLED && !s_flow) {
#if defined(ESP_PLATFORM)
        s_flow = (FlowSeg *)heap_caps_malloc(sizeof(FlowSeg) * FLOW_MAX, MALLOC_CAP_SPIRAM);
#else
        s_flow = (FlowSeg *)malloc(sizeof(FlowSeg) * FLOW_MAX);
#endif
    }
    if (FLOW_CANVAS_ENABLED && !s_flowBuf) {
        const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
        s_flowBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_flowBuf = (lv_color_t *)malloc(sz);
#endif
    }
    if (FLOW_CANVAS_ENABLED) {
        s_flowCanvas = lv_canvas_create(parent);
        lv_obj_clear_flag(s_flowCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        if (s_flowBuf) {
            lv_canvas_set_buffer(s_flowCanvas, s_flowBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
            lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
        }
        lv_obj_center(s_flowCanvas);
    } else {
        s_flowCanvas = nullptr;
    }

    s_gridLayer = make_layer(parent, grid_draw_cb);
    if (s_flowCanvas) lv_obj_move_foreground(s_flowCanvas);
    s_sweep     = make_layer(parent, sweep_draw_cb);
    s_acLayer   = make_layer(parent, ac_draw_cb);

    s_rose[0] = make_label(parent, "N", &lv_font_montserrat_28, COL_INK, LV_ALIGN_TOP_MID,    0, 12);
    s_rose[1] = make_label(parent, "S", &lv_font_montserrat_28, COL_INK, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_rose[2] = make_label(parent, "E", &lv_font_montserrat_28, COL_INK, LV_ALIGN_RIGHT_MID, -12, 0);
    s_rose[3] = make_label(parent, "W", &lv_font_montserrat_28, COL_INK, LV_ALIGN_LEFT_MID,   12, 0);

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
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    start_pulse_anim();

    s_centerDot = lv_obj_create(parent);
    lv_obj_remove_style_all(s_centerDot);
    lv_obj_set_size(s_centerDot, 7, 7);
    lv_obj_center(s_centerDot);
    lv_obj_set_style_radius(s_centerDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_centerDot, COL_INK, 0);
    lv_obj_set_style_bg_opa(s_centerDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_centerDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    s_lastSweepTickMs = lv_tick_get();
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);
    else lv_timer_set_period(s_timer, SWEEP_FRAME_MS);

    setTheme(s_theme);
}

void update(const std::vector<Aircraft> &aircraft, const RadarSettings &s) {
    static std::vector<AcDraw> out;
    static std::vector<HexKey> present;
    out.clear();
    present.clear();
    out.reserve(aircraft.size());
    present.reserve(aircraft.size());
    const float R = (float)RADAR_R_OUTER_PX;
    const uint32_t now = lv_tick_get();

    // Reproject the coastline only when the scope geometry actually changes (home
    // moved or range zoomed) — never per frame. Then repaint the static chrome layer.
    static double s_coLat = 1e9, s_coLon = 1e9; static float s_coRange = -1.0f;
    if (s.homeLat != s_coLat || s.homeLon != s_coLon || s.rangeKm != s_coRange) {
        const bool firstFix = (s_coRange < 0.0f);
        s_coLat = s.homeLat; s_coLon = s.homeLon; s_coRange = s.rangeKm;
        coastline_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        airports_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        if (s_gridLayer) lv_obj_invalidate(s_gridLayer);
        if (!firstFix) {
            // Scope scale/center changed: old trails were plotted at the previous
            // projection and would be wrong now — drop them and clear the flow layer.
            s_trails.clear();
            flow_clear();
            flow_redraw_all();
        }
    }

    for (const Aircraft &ac : aircraft) {
        if (ac.onGround && !s_groundAircraftEnabled) continue;     // parked/taxiing contacts can obscure airport labels

        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const geo::Point p = geo::projectToScreen(distKm, brg, s.rangeKm, s_cx, s_cy, R, s.rotationDeg);

        AcDraw d;
        d.trailCount = 0;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;
        {
            const AcDraw *prev = nullptr;
            for (const AcDraw &old : s_acs) {
                if (strcmp(old.hex, ac.hex) == 0) { prev = &old; break; }
            }
            if (prev) {
                const long dx = (long)target.x - prev->pos.x;
                const long dy = (long)target.y - prev->pos.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : prev->pos;  // snap if it jumped
            } else d.from = target;                                               // new contact: appear in place
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
        d.military = ac.military;
        snprintf(d.hex,  sizeof(d.hex),  "%s", ac.hex);
        snprintf(d.call, sizeof(d.call), "%s", ac.flight);
        snprintf(d.type, sizeof(d.type), "%s", ac.type);
        d.altFt = ac.altBaro;
        d.onGround = ac.onGround;
        d.vsFpm = ac.baroRate;
        d.gsKt = ac.gs;
        d.distKm = (float)distKm;
        d.bearingDeg = (float)brg;
        d.lat = ac.lat;
        d.lon = ac.lon;
        d.squawk = ac.squawk;
        if (ac.onGround) snprintf(d.altTxt, sizeof(d.altTxt), "GND");
        else             snprintf(d.altTxt, sizeof(d.altTxt), "%.0f ft", (double)ac.altBaro);

        HexKey keyBuf{};
        snprintf(keyBuf.data(), keyBuf.size(), "%s", ac.hex);
        present.push_back(keyBuf);
        if (d.inRange) {
            if (s_trails.capacity() < (size_t)ADSB_MAX_AIRCRAFT) s_trails.reserve(ADSB_MAX_AIRCRAFT);
            TrailTrack *hist = find_or_add_trail(ac.hex);
            hist->seen = true;

            uint8_t kept = 0;
            for (uint8_t i = 0; i < hist->count; ++i) {
                if ((uint32_t)(now - hist->pts[i].bornMs) <= TRAIL_TTL_MS) hist->pts[kept++] = hist->pts[i];
            }
            hist->count = kept;

            const bool moved = hist->count == 0 ||
                               abs((int)hist->pts[hist->count - 1].p.x - (int)target.x) > 0 ||
                               abs((int)hist->pts[hist->count - 1].p.y - (int)target.y) > 0;
            if (moved) {
#if FLOW_CANVAS_ENABLED
                if (hist->count > 0) {
                    FlowSeg seg = { hist->pts[hist->count - 1].p, target, now };
                    flow_push(seg);
                    flow_draw_seg(seg, now);
                }
#endif
                if (hist->count >= TRAIL_MAX) {
                    memmove(&hist->pts[0], &hist->pts[1], sizeof(TrailPt) * (TRAIL_MAX - 1));
                    hist->count = TRAIL_MAX - 1;
                }
                hist->pts[hist->count++] = { target, now };
            }
            d.trailCount = hist->count;
            for (uint8_t i = 0; i < d.trailCount; ++i) {
                d.trail[i] = hist->pts[i];
            }
        } else {
            if (TrailTrack *hist = find_trail(ac.hex)) hist->seen = false;
        }
        out.push_back(std::move(d));
    }

    for (auto it = s_trails.begin(); it != s_trails.end();) {
        if (!it->seen || !contains_hex(present, it->hex)) it = s_trails.erase(it);
        else ++it;
    }
    if (!s_selHex.empty() && !contains_hex(present, s_selHex.c_str())) s_selHex.clear();

    // nearest first (the dragon balls + the list); cap to keep work bounded
    std::sort(out.begin(), out.end(),
              [](const AcDraw &a, const AcDraw &b) { return a.distKm < b.distKm; });
    if (out.size() > 20) out.resize(20);

#if FLOW_CANVAS_ENABLED
    flow_prune(now);
    if (now - s_lastFlowRefreshMs > 30000UL) {
        s_lastFlowRefreshMs = now;
        flow_redraw_all();
    }
#endif

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        snprintf(r, sizeof(r), "%.0f km", (double)s.rangeKm);
        lv_label_set_text(s_rangeLbl, r);
    }

    // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)POLL_INTERVAL_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    s_acs.swap(out);
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

int hitTest(int x, int y) {
    int best = -1;
    long bestD = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
    const bool drg = dragon();
    int balls = 0, arrows = 0;
    for (size_t i = 0; i < s_acs.size(); ++i) {
        if (drg) {
            if (s_acs[i].inRange) { if (balls >= DRAGON_BALLS) continue; balls++; }
            else { if (arrows >= DRAGON_ARROWS) continue; arrows++; }
        } else if (!s_acs[i].inRange) continue;
        const long dx = (long)s_acs[i].pos.x - x;
        const long dy = (long)s_acs[i].pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    }
    return best;
}

static void fill_info(const AcDraw &a, AcInfo &out) {
    snprintf(out.hex, sizeof(out.hex), "%s", a.hex);
    snprintf(out.call, sizeof(out.call), "%s", a.call);
    snprintf(out.type, sizeof(out.type), "%s", a.type);
    out.altFt = a.altFt; out.onGround = a.onGround;
    out.vsFpm = a.vsFpm; out.gsKt = a.gsKt;
    out.distKm = a.distKm; out.bearingDeg = a.bearingDeg;
    out.lat = a.lat; out.lon = a.lon;
    out.squawk = a.squawk; out.emergency = a.emergency; out.military = a.military;
}

void select(int idx) {
    if (idx < 0 || idx >= (int)s_acs.size()) s_selHex.clear();
    else s_selHex = s_acs[idx].hex;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
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

int alertCountInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange && (a.emergency || a.military)) ++n;
    return n;
}

int emergencyCountInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange && a.emergency) ++n;
    return n;
}

int militaryCountInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange && a.military) ++n;
    return n;
}

bool info(int idx, AcInfo &out) {
    if (idx < 0 || idx >= (int)s_acs.size()) return false;
    fill_info(s_acs[idx], out);
    return true;
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

} // namespace radar
