// Radar scope (M1) + aircraft (M2) + selection (M3) + selectable themes (M4).
// Pure LVGL, portable. Visual reference: assets/plane_radar_2.0_mockup.html
//   THEME_PHOSPHOR : green-on-black radar scope (rings, sweep, altitude glyphs)
//   THEME_DRAGON   : DBZ "Dragon Radar": green gradient, square grid, the 7 nearest
//                    aircraft as yellow balls (emitting waves) + off-range arrows.
#include "radar_view.h"
#include "config.h"
#include "geo.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
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
// ---- dragon palette (DBZ) ----
#define DRG_BLIP   lv_color_hex(0xFFE11A)
#define DRG_EMERG  lv_color_hex(0xFF4D2E)
#define DRG_ACCENT lv_color_hex(0xFF8A1E)
#define DRG_GRID   lv_color_hex(0x2C6B24)
#define DRG_BG_TOP lv_color_hex(0x86E03C)
#define DRG_BG_BOT lv_color_hex(0x2E8A26)
#define DRG_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config ----
#define SWEEP_PERIOD_MS   4200
#define SWEEP_FRAME_MS    30
#define SWEEP_TRAIL_DEG   38.0f
#define SWEEP_TRAIL_STEPS 20
#define SWEEP_TRAIL_OPA   72

// ---- aircraft / flow / dragon config ----
#define TRAIL_MAX         7
#define TAP_RADIUS_PX     26
#define FLOW_MAX          700
#define FLOW_REDRAW_EVERY 80
#define FLOW_OPA          55
#define DRAGON_BALLS      7
#define DRAGON_ARROWS     8
#define BALL_R            9
#define WAVE_EXPAND       16.0f

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
static lv_timer_t *s_timer    = nullptr;
static float       s_sweepDeg = 0.0f;
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = POLL_INTERVAL_MS;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static std::string s_selHex;

struct FlowSeg { lv_point_t a, b; };
static std::deque<FlowSeg> s_flow;
static int s_flowRedrawCtr = 0;

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
    std::vector<lv_point_t> trail;
};
static std::vector<AcDraw> s_acs;
static std::map<std::string, std::vector<lv_point_t>> s_trails;

static const float GX[4] = { 0.0f,  7.0f, 0.0f, -7.0f };
static const float GY[4] = { -11.0f, 5.0f, 8.0f, 5.0f };

static inline bool dragon() { return s_theme == THEME_DRAGON; }

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
static void flow_draw_seg(const FlowSeg &s) {
    if (!s_flowCanvas) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = dragon() ? DRG_FLOW : s_cRing;
    d.width = 2;
    d.opa = FLOW_OPA;
    lv_point_t pts[2] = { s.a, s.b };
    lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);
}

static void flow_redraw_all(void) {
    if (!s_flowCanvas) return;
    lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    for (const FlowSeg &s : s_flow) flow_draw_seg(s);
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
        lv_draw_polygon(d, &td, tri, 3);
        return;
    }

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
    ld.width = 5;
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
    if (++s_frameCtr % 3 == 0) interp_step();         // smooth glyph motion (~90 ms cadence)
    if (dragon()) {
        // animate the dragon-ball waves (invalidate only the ball areas)
        s_wavePhase += 0.05f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_acLayer) return;
        int balls = 0;
        for (const AcDraw &ac : s_acs) {
            if (!ac.inRange) continue;
            if (balls >= DRAGON_BALLS) break;
            balls++;
            lv_area_t a = { (lv_coord_t)(ac.pos.x - 28), (lv_coord_t)(ac.pos.y - 28),
                            (lv_coord_t)(ac.pos.x + 28), (lv_coord_t)(ac.pos.y + 28) };
            lv_obj_invalidate_area(s_acLayer, &a);
        }
        return;
    }
    s_prevSweepDeg = s_sweepDeg;
    s_sweepDeg += 360.0f * (float)SWEEP_FRAME_MS / (float)SWEEP_PERIOD_MS;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
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
    const int n = (int)ac.trail.size();
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 2;
    for (int i = 1; i < n; ++i) {
        t.opa = (lv_opa_t)(10 + 45 * i / n);
        lv_point_t a = ac.trail[i - 1], b = ac.trail[i];
        lv_draw_line(d, &t, &a, &b);
    }
}

static void draw_ball(lv_draw_ctx_t *d, const AcDraw &ac) {
    // emitted wave
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = DRG_ACCENT;
    w.width = 2;
    w.opa = (lv_opa_t)((1.0f - s_wavePhase) * 150.0f);
    if (w.opa > 4) lv_draw_arc(d, &w, &ac.pos, (uint16_t)(BALL_R + s_wavePhase * WAVE_EXPAND), 0, 360);

    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? DRG_EMERG : DRG_BLIP;
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
    b.bg_color = ac.emergency ? DRG_EMERG : DRG_BLIP;
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
            if (ac.emergency) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = COL_EMERG; h.width = 2; h.opa = 200;
                lv_draw_arc(d, &h, &ac.pos, 16, 0, 360);
            }
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
                sr.color = ac.emergency ? COL_EMERG : s_cInk;
                lv_draw_arc(d, &sr, &ac.pos, 19, 0, 360);
            }
        }

        // floating labels (phosphor only; dragon keeps clean balls + the tap card)
        if (!drg) {
            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = &lv_font_montserrat_14;
            lc.color = s_cInk;
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
    show(s_pulse, !drg);

    // retint the persistent chrome objects for the active palette
    if (s_rose[0]) lv_obj_set_style_text_color(s_rose[0], s_cInk, 0);
    for (int i = 1; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cSoft, 0);
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

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_acs.clear();
    s_trails.clear();
    s_flow.clear();
    s_selHex.clear();
    s_flowRedrawCtr = 0;

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_flowBuf) {
        const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
        s_flowBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_flowBuf = (lv_color_t *)malloc(sz);
#endif
    }
    s_flowCanvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_flowCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (s_flowBuf) {
        lv_canvas_set_buffer(s_flowCanvas, s_flowBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    }
    lv_obj_center(s_flowCanvas);

    s_gridLayer = make_layer(parent, grid_draw_cb);
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
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2600);
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

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);

    setTheme(s_theme);
}

void update(const std::vector<Aircraft> &aircraft, const RadarSettings &s) {
    std::vector<AcDraw> out;
    out.reserve(aircraft.size());
    std::set<std::string> present;
    const float R = (float)RADAR_R_OUTER_PX;

    std::map<std::string, lv_point_t> prevPos;        // smooth-motion: glide starts here
    for (const AcDraw &a : s_acs) prevPos[a.hex] = a.pos;

    for (const Aircraft &ac : aircraft) {
        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const geo::Point p = geo::projectToScreen(distKm, brg, s.rangeKm, s_cx, s_cy, R, s.rotationDeg);

        AcDraw d;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;
        {
            auto pit = prevPos.find(std::string(ac.hex.c_str()));
            if (pit != prevPos.end()) {
                const long dx = (long)target.x - pit->second.x;
                const long dy = (long)target.y - pit->second.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : pit->second;  // snap if it jumped
            } else d.from = target;                                                  // new contact: appear in place
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
        if (ac.onGround) snprintf(d.altTxt, sizeof(d.altTxt), "GND");
        else             snprintf(d.altTxt, sizeof(d.altTxt), "%.0f ft", (double)ac.altBaro);

        const std::string key = ac.hex.c_str();
        present.insert(key);
        if (d.inRange) {
            std::vector<lv_point_t> &hist = s_trails[key];
            const bool moved = hist.empty() ||
                               abs((int)hist.back().x - (int)target.x) > 0 ||
                               abs((int)hist.back().y - (int)target.y) > 0;
            if (moved) {
                if (!hist.empty()) {
                    FlowSeg seg = { hist.back(), target };
                    s_flow.push_back(seg);
                    if ((int)s_flow.size() > FLOW_MAX) s_flow.pop_front();
                    flow_draw_seg(seg);
                }
                hist.push_back(target);
                if ((int)hist.size() > TRAIL_MAX) hist.erase(hist.begin());
            }
            d.trail = hist;
        }
        out.push_back(std::move(d));
    }

    for (auto it = s_trails.begin(); it != s_trails.end();) {
        if (present.find(it->first) == present.end()) it = s_trails.erase(it);
        else ++it;
    }
    if (!s_selHex.empty() && present.find(s_selHex) == present.end()) s_selHex.clear();

    // nearest first (the dragon balls + the list); cap to keep work bounded
    std::sort(out.begin(), out.end(),
              [](const AcDraw &a, const AcDraw &b) { return a.distKm < b.distKm; });
    if (out.size() > 20) out.resize(20);

    if (++s_flowRedrawCtr >= FLOW_REDRAW_EVERY) {
        s_flowRedrawCtr = 0;
        flow_redraw_all();
    }

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        snprintf(r, sizeof(r), "%.0f km", (double)s.rangeKm);
        lv_label_set_text(s_rangeLbl, r);
    }

    const uint32_t now = lv_tick_get();              // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)POLL_INTERVAL_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    s_acs = std::move(out);
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
    out.squawk = a.squawk; out.emergency = a.emergency;
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

bool info(int idx, AcInfo &out) {
    if (idx < 0 || idx >= (int)s_acs.size()) return false;
    fill_info(s_acs[idx], out);
    return true;
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

} // namespace radar
