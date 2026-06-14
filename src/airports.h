#pragma once
// Airport markers for the radar scope. Projects the embedded OurAirports list
// (airports_data.h) like the coastline: cull to the scope, great-circle project,
// cache screen markers, draw in the static chrome layer. Large airports get a small
// ring + ICAO label; medium airports get a dot + ICAO label. Runways are drawn
// as short oriented strokes when endpoint geometry is available. Projection is done
// only on a home/range change, never per frame.
#include <lvgl.h>
#include <stddef.h>

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx);

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa);
void airports_draw_scaled(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa,
                          float cx, float cy, float rOuterPx);
void airports_project_weather(double centerLat, double centerLon, int zoom,
                              float cx, float cy, float displayPx, float sourcePx);
void airports_draw_weather(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa);
void airports_set_offset(lv_coord_t dx, lv_coord_t dy);

// Hit-test the currently projected airport labels/markers. Returns true and copies
// the ICAO label when a tap lands near an airport.
bool airports_hit_test(int x, int y, char *label, size_t labelSize);
bool airports_info(const char *label, char *name, size_t nameSize,
                   char *runways, size_t runwaysSize,
                   char *surfaces, size_t surfacesSize,
                   char *freqs, size_t freqsSize);
