#pragma once
#include <lvgl.h>
#include <stddef.h>

struct WeatherRequest {
    uint32_t id;
    double lat;
    double lon;
    float rangeKm;
};

struct WeatherView {
    double lat;
    double lon;
    float rangeKm;
    int zoom;
};

void weather_request(double lat, double lon, float rangeKm);
bool weather_pending(WeatherRequest *out);
lv_color_t *weather_buffer(int *mw, int *mh);
void weather_commit(uint32_t id, int w, int h, const char *label, const char *status,
                    double lat = 0.0, double lon = 0.0, float rangeKm = 0.0f, int zoom = 0);
bool weather_get(int *w, int *h, char *label, size_t labelN, char *status, size_t statusN);
bool weather_view(WeatherView *out);
bool weather_take_dirty(void);
