#include "airports.h"
#include "airports_data.h"
#include "config.h"
#include "geo.h"
#include <vector>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Apt { lv_point_t pos; char label[5]; uint8_t large; uint8_t nearHome; };
struct Rwy { lv_point_t le; lv_point_t he; uint8_t large; };
struct WeatherApt { lv_point_t pos; char label[5]; uint8_t large; };
static std::vector<Apt> s_apts;
static std::vector<Rwy> s_rwys;
static std::vector<WeatherApt> s_weatherApts;
static std::vector<Rwy> s_weatherRwys;
static lv_coord_t s_offX = 0, s_offY = 0;
static float s_cx = SCREEN_CX, s_cy = SCREEN_CY, s_rOuterPx = RADAR_R_OUTER_PX;

static bool visible_point(lv_point_t p, float padPx = 0.0f) {
    const float dx = (float)p.x - s_cx;
    const float dy = (float)p.y - s_cy;
    const float r = s_rOuterPx + padPx;
    return dx * dx + dy * dy <= r * r;
}

static lv_point_t scale_point(lv_point_t p, float cx, float cy, float scale) {
    lv_point_t out;
    out.x = (lv_coord_t)lroundf(cx + ((float)p.x + (float)s_offX - s_cx) * scale);
    out.y = (lv_coord_t)lroundf(cy + ((float)p.y + (float)s_offY - s_cy) * scale);
    return out;
}

static bool visible_scaled(lv_point_t p, float cx, float cy, float rOuterPx, float padPx = 0.0f) {
    const float dx = (float)p.x - cx;
    const float dy = (float)p.y - cy;
    const float r = rOuterPx + padPx;
    return dx * dx + dy * dy <= r * r;
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

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx) {
    s_apts.clear();
    s_rwys.clear();
    s_offX = 0;
    s_offY = 0;
    s_cx = cx;
    s_cy = cy;
    s_rOuterPx = rOuterPx;
    if (rangeKm <= 0) return;

    const double cacheRangeKm = rangeKm * (double)MAP_PAN_CACHE_FACTOR;
    const double rangeDeg  = cacheRangeKm / 111.0;
    const double latMargin = rangeDeg * 1.05;
    const double cosLat    = cos(homeLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    for (int i = 0; i < AIRPORT_NUM; ++i) {
        const double lat = AIRPORT_LAT[i] / (double)AIRPORT_SCALE;
        const double lon = AIRPORT_LON[i] / (double)AIRPORT_SCALE;
        const double dlon = lon - homeLon;
        if (fabs(lat - homeLat) > latMargin) continue;                      // cheap bbox reject
        if (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin) continue;
        const double dist = geo::haversineKm(homeLat, homeLon, lat, lon);
        if (dist > cacheRangeKm) continue;                                  // cache nearby off-scope airports for panning
        const double brg = geo::bearingDeg(homeLat, homeLon, lat, lon);
        const double rPx = (dist / rangeKm) * rOuterPx;
        const double a   = brg * M_PI / 180.0;
        Apt ap;
        ap.pos.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
        ap.pos.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
        memcpy(ap.label, AIRPORT_LABEL[i], 5);
        ap.large = AIRPORT_LARGE[i];
        ap.nearHome = (rPx < 18.0) ? 1 : 0;

        const int firstRwy = AIRPORT_RUNWAY_FIRST[i];
        const int countRwy = AIRPORT_RUNWAY_COUNT[i];
        const float pxPerM = rOuterPx / (float)(rangeKm * 1000.0);
        for (int r = firstRwy; r >= 0 && r < firstRwy + countRwy; ++r) {
            Rwy rw;
            rw.le.x = (lv_coord_t)lroundf((float)ap.pos.x + RUNWAY_LE_E_M[r] * pxPerM);
            rw.le.y = (lv_coord_t)lroundf((float)ap.pos.y - RUNWAY_LE_N_M[r] * pxPerM);
            rw.he.x = (lv_coord_t)lroundf((float)ap.pos.x + RUNWAY_HE_E_M[r] * pxPerM);
            rw.he.y = (lv_coord_t)lroundf((float)ap.pos.y - RUNWAY_HE_N_M[r] * pxPerM);
            rw.large = ap.large;
            s_rwys.push_back(rw);
        }

        s_apts.push_back(ap);
    }
}

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa) {
    if (s_apts.empty()) return;

    lv_draw_line_dsc_t runway;
    lv_draw_line_dsc_init(&runway);
    runway.color = color; runway.width = 2; runway.opa = 220;
    for (const Rwy &rw : s_rwys) {
        lv_point_t le = { (lv_coord_t)(rw.le.x + s_offX), (lv_coord_t)(rw.le.y + s_offY) };
        lv_point_t he = { (lv_coord_t)(rw.he.x + s_offX), (lv_coord_t)(rw.he.y + s_offY) };
        lv_point_t mid = { (lv_coord_t)((le.x + he.x) / 2), (lv_coord_t)((le.y + he.y) / 2) };
        if (!visible_point(le, 24.0f) && !visible_point(he, 24.0f) && !visible_point(mid, 24.0f)) continue;
        runway.width = rw.large ? 3 : 2;
        lv_draw_line(ctx, &runway, &le, &he);
    }

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = color; ring.width = 2; ring.opa = opa;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color; dot.bg_opa = opa; dot.radius = LV_RADIUS_CIRCLE;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color; lbl.opa = 235; lbl.font = &lv_font_montserrat_12;

    lv_draw_label_dsc_t shadowLbl;
    lv_draw_label_dsc_init(&shadowLbl);
    shadowLbl.color = lv_color_black(); shadowLbl.opa = 190; shadowLbl.font = &lv_font_montserrat_12;

    lv_draw_label_dsc_t mediumLbl;
    lv_draw_label_dsc_init(&mediumLbl);
    mediumLbl.color = color; mediumLbl.opa = 235; mediumLbl.font = &lv_font_montserrat_12;

    for (const Apt &ap : s_apts) {
        const lv_point_t pos = { (lv_coord_t)(ap.pos.x + s_offX), (lv_coord_t)(ap.pos.y + s_offY) };
        if (!visible_point(pos, 12.0f)) continue;
        if (ap.large) {
            lv_draw_arc(ctx, &ring, &pos, 3, 0, 360);                    // small hollow ring
            if (ap.label[0]) {
                const lv_coord_t lx = ap.nearHome ? (lv_coord_t)(pos.x + 12) : (lv_coord_t)(pos.x + 5);
                const lv_coord_t ly = ap.nearHome ? (lv_coord_t)(pos.y - 24) : (lv_coord_t)(pos.y - 7);
                lv_area_t sh = { (lv_coord_t)(lx + 1), (lv_coord_t)(ly + 1),
                                 (lv_coord_t)(lx + 64), (lv_coord_t)(ly + 15) };
                lv_area_t la = { lx, ly, (lv_coord_t)(lx + 63), (lv_coord_t)(ly + 14) };
                lv_draw_label(ctx, &shadowLbl, &sh, ap.label, NULL);
                lv_draw_label(ctx, &lbl, &la, ap.label, NULL);
            }
        } else {
            lv_area_t d = { (lv_coord_t)(pos.x - 2), (lv_coord_t)(pos.y - 2),
                            (lv_coord_t)(pos.x + 2), (lv_coord_t)(pos.y + 2) };
            lv_draw_rect(ctx, &dot, &d);                                    // medium airport marker
            if (ap.label[0]) {
                const lv_coord_t lx = ap.nearHome ? (lv_coord_t)(pos.x + 12) : (lv_coord_t)(pos.x + 5);
                const lv_coord_t ly = ap.nearHome ? (lv_coord_t)(pos.y - 24) : (lv_coord_t)(pos.y - 6);
                lv_area_t sh = { (lv_coord_t)(lx + 1), (lv_coord_t)(ly + 1),
                                 (lv_coord_t)(lx + 64), (lv_coord_t)(ly + 15) };
                lv_area_t la = { lx, ly, (lv_coord_t)(lx + 63), (lv_coord_t)(ly + 14) };
                lv_draw_label(ctx, &shadowLbl, &sh, ap.label, NULL);
                lv_draw_label(ctx, &mediumLbl, &la, ap.label, NULL);
            }
        }
    }
}

void airports_draw_scaled(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa,
                          float cx, float cy, float rOuterPx) {
    if (s_apts.empty() || s_rOuterPx <= 0.0f || rOuterPx <= 0.0f) return;
    const float scale = rOuterPx / s_rOuterPx;

    lv_draw_line_dsc_t runway;
    lv_draw_line_dsc_init(&runway);
    runway.color = color;
    runway.width = 2;
    runway.opa = 185;
    for (const Rwy &rw : s_rwys) {
        const lv_point_t le = scale_point(rw.le, cx, cy, scale);
        const lv_point_t he = scale_point(rw.he, cx, cy, scale);
        const lv_point_t mid = { (lv_coord_t)((le.x + he.x) / 2), (lv_coord_t)((le.y + he.y) / 2) };
        if (!visible_scaled(le, cx, cy, rOuterPx, 18.0f) &&
            !visible_scaled(he, cx, cy, rOuterPx, 18.0f) &&
            !visible_scaled(mid, cx, cy, rOuterPx, 18.0f)) continue;
        runway.width = rw.large ? 3 : 2;
        lv_draw_line(ctx, &runway, &le, &he);
    }

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = color;
    ring.width = 2;
    ring.opa = opa;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.bg_opa = opa;
    dot.radius = LV_RADIUS_CIRCLE;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color;
    lbl.opa = 245;
    lbl.font = &lv_font_montserrat_12;

    lv_draw_label_dsc_t shadowLbl;
    lv_draw_label_dsc_init(&shadowLbl);
    shadowLbl.color = lv_color_black();
    shadowLbl.opa = 220;
    shadowLbl.font = &lv_font_montserrat_12;

    for (const Apt &ap : s_apts) {
        const lv_point_t pos = scale_point(ap.pos, cx, cy, scale);
        if (!visible_scaled(pos, cx, cy, rOuterPx, 12.0f)) continue;
        if (ap.large) {
            lv_draw_arc(ctx, &ring, &pos, 3, 0, 360);
        } else {
            lv_area_t d = { (lv_coord_t)(pos.x - 2), (lv_coord_t)(pos.y - 2),
                            (lv_coord_t)(pos.x + 2), (lv_coord_t)(pos.y + 2) };
            lv_draw_rect(ctx, &dot, &d);
        }

        if (ap.label[0]) {
            const lv_coord_t lx = ap.nearHome ? (lv_coord_t)(pos.x + 12) : (lv_coord_t)(pos.x + 5);
            const lv_coord_t ly = ap.nearHome ? (lv_coord_t)(pos.y - 24) : (lv_coord_t)(pos.y - 7);
            lv_area_t sh = { (lv_coord_t)(lx + 1), (lv_coord_t)(ly + 1),
                             (lv_coord_t)(lx + 64), (lv_coord_t)(ly + 15) };
            lv_area_t la = { lx, ly, (lv_coord_t)(lx + 63), (lv_coord_t)(ly + 14) };
            lv_draw_label(ctx, &shadowLbl, &sh, ap.label, NULL);
            lv_draw_label(ctx, &lbl, &la, ap.label, NULL);
        }
    }
}

void airports_project_weather(double centerLat, double centerLon, int zoom,
                              float cx, float cy, float displayPx, float sourcePx) {
    s_weatherApts.clear();
    s_weatherRwys.clear();
    if (zoom <= 0 || displayPx <= 0.0f || sourcePx <= 0.0f) return;
    const float radius = displayPx * 0.5f;
    const float scale = displayPx / sourcePx;
    double centerX = 0.0, centerY = 0.0;
    mercator_px(centerLat, centerLon, zoom, 512.0, &centerX, &centerY);

    const double cosCenter = cos(centerLat * M_PI / 180.0);
    const double sourceKm = 40075.0 * (fabs(cosCenter) < 0.05 ? 0.05 : fabs(cosCenter)) / (double)(1 << zoom);
    const double latMargin = sourceKm / 111.0 * 0.75;
    const double lonMargin = latMargin / (fabs(cosCenter) < 0.15 ? 0.15 : fabs(cosCenter));

    for (int i = 0; i < AIRPORT_NUM; ++i) {
        if (AIRPORT_CLASS[i] < 1) continue;   // weather page: show medium + large only
        const double lat = AIRPORT_LAT[i] / (double)AIRPORT_SCALE;
        const double lon = AIRPORT_LON[i] / (double)AIRPORT_SCALE;
        const double dlon = lon - centerLon;
        if (fabs(lat - centerLat) > latMargin) continue;
        if (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin) continue;

        const lv_point_t pos = weather_point(lat, lon, centerX, centerY, zoom, cx, cy, scale);
        if (!visible_scaled(pos, cx, cy, radius, 14.0f)) continue;

        WeatherApt ap;
        ap.pos = pos;
        memcpy(ap.label, AIRPORT_LABEL[i], 5);
        ap.large = AIRPORT_LARGE[i];
        s_weatherApts.push_back(ap);

        const int firstRwy = AIRPORT_RUNWAY_FIRST[i];
        const int countRwy = AIRPORT_RUNWAY_COUNT[i];
        const double cosLat = cos(lat * M_PI / 180.0);
        for (int r = firstRwy; r >= 0 && r < firstRwy + countRwy; ++r) {
            const double lonScale = 111320.0 * (fabs(cosLat) < 0.05 ? 0.05 : fabs(cosLat));
            const double leLat = lat + (double)RUNWAY_LE_N_M[r] / 111320.0;
            const double leLon = lon + (double)RUNWAY_LE_E_M[r] / lonScale;
            const double heLat = lat + (double)RUNWAY_HE_N_M[r] / 111320.0;
            const double heLon = lon + (double)RUNWAY_HE_E_M[r] / lonScale;
            const lv_point_t le = weather_point(leLat, leLon, centerX, centerY, zoom, cx, cy, scale);
            const lv_point_t he = weather_point(heLat, heLon, centerX, centerY, zoom, cx, cy, scale);
            const lv_point_t mid = { (lv_coord_t)((le.x + he.x) / 2), (lv_coord_t)((le.y + he.y) / 2) };
            if (!visible_scaled(le, cx, cy, radius, 18.0f) &&
                !visible_scaled(he, cx, cy, radius, 18.0f) &&
                !visible_scaled(mid, cx, cy, radius, 18.0f)) continue;
            Rwy rw;
            rw.le = le;
            rw.he = he;
            rw.large = ap.large;
            s_weatherRwys.push_back(rw);
        }
    }
}

void airports_draw_weather(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa) {
    if (!ctx || s_weatherApts.empty()) return;

    lv_draw_line_dsc_t runway;
    lv_draw_line_dsc_init(&runway);
    runway.color = color;
    runway.width = 2;
    runway.opa = 175;
    for (const Rwy &rw : s_weatherRwys) {
        runway.width = rw.large ? 3 : 2;
        lv_draw_line(ctx, &runway, &rw.le, &rw.he);
    }

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = color;
    ring.width = 2;
    ring.opa = opa;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.bg_opa = opa;
    dot.radius = LV_RADIUS_CIRCLE;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color;
    lbl.opa = 240;
    lbl.font = &lv_font_montserrat_12;

    lv_draw_label_dsc_t shadowLbl;
    lv_draw_label_dsc_init(&shadowLbl);
    shadowLbl.color = lv_color_black();
    shadowLbl.opa = 220;
    shadowLbl.font = &lv_font_montserrat_12;

    for (const WeatherApt &ap : s_weatherApts) {
        if (ap.large) {
            lv_draw_arc(ctx, &ring, &ap.pos, 3, 0, 360);
        } else {
            lv_area_t d = { (lv_coord_t)(ap.pos.x - 2), (lv_coord_t)(ap.pos.y - 2),
                            (lv_coord_t)(ap.pos.x + 2), (lv_coord_t)(ap.pos.y + 2) };
            lv_draw_rect(ctx, &dot, &d);
        }

        if (ap.label[0]) {
            const lv_coord_t lx = (lv_coord_t)(ap.pos.x + 5);
            const lv_coord_t ly = (lv_coord_t)(ap.pos.y - 7);
            lv_area_t sh = { (lv_coord_t)(lx + 1), (lv_coord_t)(ly + 1),
                             (lv_coord_t)(lx + 64), (lv_coord_t)(ly + 15) };
            lv_area_t la = { lx, ly, (lv_coord_t)(lx + 63), (lv_coord_t)(ly + 14) };
            lv_draw_label(ctx, &shadowLbl, &sh, ap.label, NULL);
            lv_draw_label(ctx, &lbl, &la, ap.label, NULL);
        }
    }
}

void airports_set_offset(lv_coord_t dx, lv_coord_t dy) {
    s_offX = dx;
    s_offY = dy;
}

bool airports_hit_test(int x, int y, char *label, size_t labelSize) {
    if (!label || labelSize == 0) return false;
    int best = -1;
    long bestD = 28L * 28L;
    for (size_t i = 0; i < s_apts.size(); ++i) {
        const Apt &ap = s_apts[i];
        const lv_point_t pos = { (lv_coord_t)(ap.pos.x + s_offX), (lv_coord_t)(ap.pos.y + s_offY) };
        if (!visible_point(pos, 12.0f)) continue;
        const long dx = (long)pos.x - x;
        const long dy = (long)pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; continue; }

        const lv_coord_t lx = ap.nearHome ? (lv_coord_t)(pos.x + 12) : (lv_coord_t)(pos.x + 5);
        const lv_coord_t ly = ap.nearHome ? (lv_coord_t)(pos.y - 24) : (lv_coord_t)(pos.y - 7);
        if (x >= lx - 6 && x <= lx + 70 && y >= ly - 6 && y <= ly + 22) {
            best = (int)i;
            break;
        }
    }
    if (best < 0) return false;
    snprintf(label, labelSize, "%s", s_apts[best].label);
    return label[0] != 0;
}

bool airports_info(const char *label, char *name, size_t nameSize,
                   char *runways, size_t runwaysSize,
                   char *surfaces, size_t surfacesSize,
                   char *freqs, size_t freqsSize) {
    if (!label) return false;
    for (int i = 0; i < AIRPORT_NUM; ++i) {
        if (strcmp(label, AIRPORT_LABEL[i]) != 0) continue;
        if (name && nameSize) snprintf(name, nameSize, "%s", AIRPORT_NAME[i]);
        if (runways && runwaysSize) snprintf(runways, runwaysSize, "%s", AIRPORT_RUNWAYS_TXT[i]);
        if (surfaces && surfacesSize) snprintf(surfaces, surfacesSize, "%s", AIRPORT_SURFACES_TXT[i]);
        if (freqs && freqsSize) snprintf(freqs, freqsSize, "%s", AIRPORT_FREQS_TXT[i]);
        return true;
    }
    return false;
}
