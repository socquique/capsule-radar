#pragma once
// World coastline background for the radar scope.
// Project the embedded Natural Earth coastline (coastline_data.h) into screen
// polylines for the current scope, then draw them under the aircraft. Projection
// is done once per home/range change (cheap bbox cull + great-circle), never per
// frame; drawing happens from the scope background cache (radar_view.cpp).
#include <lvgl.h>

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx);

// Draw the projected polylines. `clip` (optional, screen coords): skip segments whose
// 2 px line cannot touch it — the scope cache renders small rects without walking the
// whole coastline.
void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width,
                    const lv_area_t *clip = nullptr);
