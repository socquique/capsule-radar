#include "coastline.h"
#include "coastline_data.h"
#include "geo.h"
#include <vector>
#include <math.h>

// Cached screen-space polylines for the current scope. Rebuilt only when the
// home position or range changes (a handful of times per session), so the per-
// frame render cost is zero — the chrome layer just repaints what's here.
static std::vector<std::vector<lv_point_t>> s_lines;

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx) {
    s_lines.clear();
    if (rangeKm <= 0) return;

    const double EDGE = 1.08;                       // include a touch past the rim, then clip
    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.20;
    const double cosLat    = cos(homeLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    const int16_t *p = COAST_PTS;
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly) {
        const int n = COAST_POLY_LEN[poly];
        std::vector<lv_point_t> run;
        for (int i = 0; i < n; ++i) {
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
        p += n * 2;
    }
}

void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width,
                    const lv_area_t *clip) {
    if (s_lines.empty()) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa   = opa;
    d.round_start = d.round_end = 1;   // smooth the joints on the thicker line
    for (const auto &line : s_lines) {
        for (size_t i = 1; i < line.size(); ++i) {
            const lv_point_t a = line[i - 1];
            const lv_point_t b = line[i];
            if (clip) {                // cheap reject: a segment that cannot touch the
                const lv_coord_t pad = (lv_coord_t)(width + 1);   // anti-aliased stroke
                if (LV_MAX(a.x, b.x) + pad < clip->x1 || LV_MIN(a.x, b.x) - pad > clip->x2 ||
                    LV_MAX(a.y, b.y) + pad < clip->y1 || LV_MIN(a.y, b.y) - pad > clip->y2) continue;
            }
            lv_draw_line(ctx, &d, &a, &b);
        }
    }
}
