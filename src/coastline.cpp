#include "coastline.h"
#include "coastline_data.h"
#include "geo.h"
#include <vector>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

// Cached screen-space polylines for the current scope. Rebuilt only when the
// home position or range changes (a handful of times per session), so the per-
// frame render cost is zero — the chrome layer just repaints what's here.
static std::vector<std::vector<lv_point_t>> s_lines;

// Bounding boxes (COAST_SCALE units), built once on first use, so a re-projection skips
// coastline far from home instead of testing all ~294k points one by one (that took ~1 s
// and froze the UI on every range change): one box per polygon, and one per chunk of
// COAST_CHUNK points, since the polygon holding home can be a whole continent.
#define COAST_CHUNK 64
struct PolyBox { int16_t latMin, latMax, lonMin, lonMax; };
static PolyBox *s_boxes = nullptr;    // per polygon
static PolyBox *s_chunks = nullptr;   // per chunk, in order (polygon 0's chunks first)

static PolyBox *alloc_boxes(size_t n) {
#ifdef ARDUINO
    return (PolyBox *)heap_caps_malloc(sizeof(PolyBox) * n, MALLOC_CAP_SPIRAM);
#else
    return (PolyBox *)malloc(sizeof(PolyBox) * n);
#endif
}

static void grow(PolyBox &b, const int16_t *pt) {
    if (pt[0] < b.latMin) b.latMin = pt[0];
    if (pt[0] > b.latMax) b.latMax = pt[0];
    if (pt[1] < b.lonMin) b.lonMin = pt[1];
    if (pt[1] > b.lonMax) b.lonMax = pt[1];
}

static void build_boxes() {
    size_t nChunks = 0;
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly)
        nChunks += (COAST_POLY_LEN[poly] + COAST_CHUNK - 1) / COAST_CHUNK;
    s_boxes = alloc_boxes(COAST_NUM_POLYS);
    s_chunks = alloc_boxes(nChunks);
    if (!s_boxes || !s_chunks) {                    // no memory: every point is tested, as before
        free(s_boxes); free(s_chunks);
        s_boxes = s_chunks = nullptr;
        return;
    }
    const PolyBox EMPTY = { INT16_MAX, INT16_MIN, INT16_MAX, INT16_MIN };
    const int16_t *p = COAST_PTS;
    PolyBox *c = s_chunks;
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly) {
        const int n = COAST_POLY_LEN[poly];
        PolyBox pb = EMPTY;
        for (int i0 = 0; i0 < n; i0 += COAST_CHUNK, ++c) {
            *c = EMPTY;
            for (int i = i0; i < n && i < i0 + COAST_CHUNK; ++i) grow(*c, p + i * 2);
            if (c->latMin < pb.latMin) pb.latMin = c->latMin;   // the polygon box spans its chunks
            if (c->latMax > pb.latMax) pb.latMax = c->latMax;
            if (c->lonMin < pb.lonMin) pb.lonMin = c->lonMin;
            if (c->lonMax > pb.lonMax) pb.lonMax = c->lonMax;
        }
        s_boxes[poly] = pb;
        p += n * 2;
    }
}

// Same window as the per-point reject in coastline_project (incl. the antimeridian wrap):
// a box that misses it holds no point in range.
static bool box_hits(const PolyBox &b, double homeLat, double homeLon, double latMargin, double lonMargin) {
    const double S = (double)COAST_SCALE;
    if (b.latMax / S < homeLat - latMargin || b.latMin / S > homeLat + latMargin) return false;
    for (double off = -360.0; off <= 360.0; off += 360.0)
        if (b.lonMax / S + off >= homeLon - lonMargin && b.lonMin / S + off <= homeLon + lonMargin) return true;
    return false;
}

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx) {
    s_lines.clear();
    if (rangeKm <= 0) return;

    const double EDGE = 1.08;                       // include a touch past the rim, then clip
    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.20;
    const double cosLat    = cos(homeLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    if (!s_boxes) build_boxes();
    const int16_t *p = COAST_PTS;
    const PolyBox *c = s_chunks;
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly) {
        const int n = COAST_POLY_LEN[poly];
        const int nc = (n + COAST_CHUNK - 1) / COAST_CHUNK;
        if (s_boxes && !box_hits(s_boxes[poly], homeLat, homeLon, latMargin, lonMargin)) {
            p += n * 2; c += nc; continue;
        }
        std::vector<lv_point_t> run;
        for (int i = 0; i < n; ++i) {
            if (s_chunks && i % COAST_CHUNK == 0 &&
                !box_hits(c[i / COAST_CHUNK], homeLat, homeLon, latMargin, lonMargin)) {
                // no point of this chunk is in range: it ends any run, like an out-of-range point
                if (run.size() >= 2) s_lines.push_back(std::move(run));
                run.clear();
                i += COAST_CHUNK - 1;
                continue;
            }
            const double lat = p[i * 2]     / (double)COAST_SCALE;
            const double lon = p[i * 2 + 1] / (double)COAST_SCALE;
            const double dlon = lon - homeLon;
            // cheap bounding-box reject (no trig) discards ~99% of the planet instantly;
            // the second dlon test wraps the antimeridian so e.g. home near 179E still works.
            const bool out = (fabs(lat - homeLat) > latMargin) ||
                             (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin);
            if (!out) {
                const double dist = geo::haversineKm(homeLat, homeLon, lat, lon);
                if (dist <= rangeKm * EDGE) {
                    const double brg = geo::bearingDeg(homeLat, homeLon, lat, lon);
                    const double rPx = (dist / rangeKm) * rOuterPx;
                    const double a   = brg * M_PI / 180.0;
                    lv_point_t sp;
                    sp.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
                    sp.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
                    run.push_back(sp);
                    continue;
                }
            }
            if (run.size() >= 2) s_lines.push_back(std::move(run));   // flush the in-range run
            run.clear();
        }
        if (run.size() >= 2) s_lines.push_back(std::move(run));
        p += n * 2; c += nc;
    }
}

void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width) {
    if (s_lines.empty()) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa   = opa;
    d.round_start = d.round_end = 1;   // smooth the joints on the thicker line
    for (const auto &line : s_lines) {
        for (size_t i = 1; i < line.size(); ++i) {
            lv_point_t a = line[i - 1];
            lv_point_t b = line[i];
            lv_draw_line(ctx, &d, &a, &b);
        }
    }
}
