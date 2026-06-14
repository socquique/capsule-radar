#include "coastline.h"
#include "coastline_data.h"
#include "geo.h"
#include <vector>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Cached screen-space polylines for the current scope. Rebuilt only when the
// home position or range changes (a handful of times per session), so the per-
// frame render cost is zero — the chrome layer just repaints what's here.
static std::vector<std::vector<lv_point_t>> s_lines;
static std::vector<std::vector<lv_point_t>> s_weatherLines;
static lv_coord_t s_offX = 0, s_offY = 0;
static float s_cx = 0.0f, s_cy = 0.0f, s_rOuterPx = 0.0f;

static lv_point_t scale_point(lv_point_t p, float cx, float cy, float scale) {
    lv_point_t out;
    out.x = (lv_coord_t)lroundf(cx + ((float)p.x + (float)s_offX - s_cx) * scale);
    out.y = (lv_coord_t)lroundf(cy + ((float)p.y + (float)s_offY - s_cy) * scale);
    return out;
}

static void mercator_px(double lat, double lon, int zoom, double tilePx, double *x, double *y) {
    if (lat > 85.05112878) lat = 85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    const double latRad = lat * M_PI / 180.0;
    const double world = tilePx * (double)(1 << zoom);
    if (x) *x = (lon + 180.0) / 360.0 * world;
    if (y) *y = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) * 0.5 * world;
}

static lv_point_t weather_point(double lat, double lon, double centerX, double centerY,
                                int zoom, float cx, float cy, float scale) {
    double x = 0.0, y = 0.0;
    mercator_px(lat, lon, zoom, 512.0, &x, &y);
    lv_point_t out;
    out.x = (lv_coord_t)lroundf(cx + (float)((x - centerX) * scale));
    out.y = (lv_coord_t)lroundf(cy + (float)((y - centerY) * scale));
    return out;
}

static bool near_weather(lv_point_t p, float cx, float cy, float radius, float padPx) {
    const float dx = (float)p.x - cx;
    const float dy = (float)p.y - cy;
    const float r = radius + padPx;
    return dx * dx + dy * dy <= r * r;
}

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx) {
    s_lines.clear();
    s_offX = 0;
    s_offY = 0;
    s_cx = cx;
    s_cy = cy;
    s_rOuterPx = rOuterPx;
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

void coastline_draw_scaled(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa,
                           lv_coord_t width, float cx, float cy, float rOuterPx) {
    if (s_lines.empty() || s_rOuterPx <= 0.0f || rOuterPx <= 0.0f) return;
    const float scale = rOuterPx / s_rOuterPx;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    for (const auto &line : s_lines) {
        for (size_t i = 1; i < line.size(); ++i) {
            lv_point_t a = scale_point(line[i - 1], cx, cy, scale);
            lv_point_t b = scale_point(line[i], cx, cy, scale);
            lv_draw_line(ctx, &d, &a, &b);
        }
    }
}

void coastline_project_weather(double centerLat, double centerLon, int zoom,
                               float cx, float cy, float displayPx, float sourcePx) {
    s_weatherLines.clear();
    if (zoom <= 0 || displayPx <= 0.0f || sourcePx <= 0.0f) return;
    const float radius = displayPx * 0.5f;
    const float scale = displayPx / sourcePx;
    double centerX = 0.0, centerY = 0.0;
    mercator_px(centerLat, centerLon, zoom, 512.0, &centerX, &centerY);

    const double cosLat = cos(centerLat * M_PI / 180.0);
    const double sourceKm = 40075.0 * (fabs(cosLat) < 0.05 ? 0.05 : fabs(cosLat)) / (double)(1 << zoom);
    const double latMargin = sourceKm / 111.0 * 0.75;
    const double lonMargin = latMargin / (fabs(cosLat) < 0.15 ? 0.15 : fabs(cosLat));

    const int16_t *p = COAST_PTS;
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly) {
        const int n = COAST_POLY_LEN[poly];
        std::vector<lv_point_t> run;
        for (int i = 0; i < n; ++i) {
            const double lat = p[i * 2] / (double)COAST_SCALE;
            const double lon = p[i * 2 + 1] / (double)COAST_SCALE;
            const double dlon = lon - centerLon;
            const bool out = (fabs(lat - centerLat) > latMargin) ||
                             (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin);
            if (out) {
                if (run.size() >= 2) s_weatherLines.push_back(std::move(run));
                run.clear();
                continue;
            }

            const lv_point_t cur = weather_point(lat, lon, centerX, centerY, zoom, cx, cy, scale);
            if (near_weather(cur, cx, cy, radius, 8.0f)) {
                run.push_back(cur);
            } else {
                if (run.size() >= 2) s_weatherLines.push_back(std::move(run));
                run.clear();
            }
        }
        if (run.size() >= 2) s_weatherLines.push_back(std::move(run));
        p += n * 2;
    }
}

void coastline_draw_weather(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width) {
    if (!ctx || s_weatherLines.empty()) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    for (const auto &line : s_weatherLines) {
        for (size_t i = 1; i < line.size(); ++i) {
            lv_point_t a = line[i - 1];
            lv_point_t b = line[i];
            lv_draw_line(ctx, &d, &a, &b);
        }
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
            a.x += s_offX; a.y += s_offY;
            b.x += s_offX; b.y += s_offY;
            lv_draw_line(ctx, &d, &a, &b);
        }
    }
}

void coastline_set_offset(lv_coord_t dx, lv_coord_t dy) {
    s_offX = dx;
    s_offY = dy;
}
