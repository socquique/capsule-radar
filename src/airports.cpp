#include "airports.h"
#include "airports_data.h"
#include "geo.h"
#include <vector>
#include <math.h>
#include <string.h>

struct Apt { lv_point_t pos; char iata[4]; uint8_t large; };
static std::vector<Apt> s_apts;

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx) {
    s_apts.clear();
    if (rangeKm <= 0) return;

    const double rangeDeg  = rangeKm / 111.0;
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
        if (dist > rangeKm) continue;                                       // only inside the scope
        const double brg = geo::bearingDeg(homeLat, homeLon, lat, lon);
        const double rPx = (dist / rangeKm) * rOuterPx;
        const double a   = brg * M_PI / 180.0;
        Apt ap;
        ap.pos.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
        ap.pos.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
        memcpy(ap.iata, AIRPORT_IATA[i], 4);
        ap.large = AIRPORT_LARGE[i];
        s_apts.push_back(ap);
    }
}

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa) {
    if (s_apts.empty()) return;

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = color; ring.width = 2; ring.opa = opa;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color; dot.bg_opa = opa; dot.radius = LV_RADIUS_CIRCLE;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color; lbl.opa = opa; lbl.font = &lv_font_montserrat_12;

    for (const Apt &ap : s_apts) {
        if (ap.large) {
            lv_draw_arc(ctx, &ring, &ap.pos, 3, 0, 360);                    // small hollow ring
            if (ap.iata[0]) {
                lv_area_t la = { (lv_coord_t)(ap.pos.x + 5), (lv_coord_t)(ap.pos.y - 7),
                                 (lv_coord_t)(ap.pos.x + 44), (lv_coord_t)(ap.pos.y + 7) };
                lv_draw_label(ctx, &lbl, &la, ap.iata, NULL);
            }
        } else {
            lv_area_t d = { (lv_coord_t)(ap.pos.x - 1), (lv_coord_t)(ap.pos.y - 1),
                            (lv_coord_t)(ap.pos.x + 1), (lv_coord_t)(ap.pos.y + 1) };
            lv_draw_rect(ctx, &dot, &d);                                    // faint dot
        }
    }
}

bool airports_nearest_iata(double lat, double lon, float maxKm,
                           char iata[4], float *distKm, float *bearingDeg) {
    if (iata) iata[0] = 0;
    double best = maxKm;
    int bestIdx = -1;
    for (int i = 0; i < AIRPORT_NUM; ++i) {
        if (!AIRPORT_IATA[i][0]) continue;
        const double alat = AIRPORT_LAT[i] / (double)AIRPORT_SCALE;
        const double alon = AIRPORT_LON[i] / (double)AIRPORT_SCALE;
        const double d = geo::haversineKm(lat, lon, alat, alon);
        if (d < best) { best = d; bestIdx = i; }
    }
    if (bestIdx < 0) return false;
    if (iata) memcpy(iata, AIRPORT_IATA[bestIdx], 4);
    if (distKm) *distKm = (float)best;
    if (bearingDeg) {
        const double alat = AIRPORT_LAT[bestIdx] / (double)AIRPORT_SCALE;
        const double alon = AIRPORT_LON[bestIdx] / (double)AIRPORT_SCALE;
        *bearingDeg = (float)geo::bearingDeg(lat, lon, alat, alon);
    }
    return true;
}
