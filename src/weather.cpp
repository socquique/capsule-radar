#include "weather.h"
#include "config.h"
#include <mutex>
#include <stdio.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#else
#include <stdlib.h>
#endif

#define WX_MAXW SCREEN_W
#define WX_MAXH SCREEN_H

static std::mutex s_m;
static lv_color_t *s_buf = nullptr;
static WeatherRequest s_want = {0, 0.0, 0.0, 0.0f};
static uint32_t s_doneId = 0;
static int s_w = 0, s_h = 0;
static char s_label[48] = "";
static char s_status[72] = "Tap Refresh";
static WeatherView s_view = {0.0, 0.0, 0.0f, 0};
static bool s_ready = false;
static bool s_dirty = true;

static lv_color_t *ensure_buf() {
    if (!s_buf) {
        const size_t sz = (size_t)WX_MAXW * WX_MAXH * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_buf = (lv_color_t *)malloc(sz);
#endif
    }
    return s_buf;
}

void weather_request(double lat, double lon, float rangeKm) {
    std::lock_guard<std::mutex> g(s_m);
    s_want.id++;
    if (s_want.id == 0) s_want.id = 1;
    s_want.lat = lat;
    s_want.lon = lon;
    s_want.rangeKm = rangeKm;
    s_ready = false;
    s_w = 0;
    s_h = 0;
    snprintf(s_status, sizeof(s_status), "Loading weather radar...");
    s_dirty = true;
}

bool weather_pending(WeatherRequest *out) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_want.id && s_want.id != s_doneId) {
        if (out) *out = s_want;
        return true;
    }
    return false;
}

lv_color_t *weather_buffer(int *mw, int *mh) {
    if (mw) *mw = WX_MAXW;
    if (mh) *mh = WX_MAXH;
    return ensure_buf();
}

void weather_commit(uint32_t id, int w, int h, const char *label, const char *status,
                    double lat, double lon, float rangeKm, int zoom) {
    std::lock_guard<std::mutex> g(s_m);
    s_doneId = id;
    s_w = w;
    s_h = h;
    s_ready = (w > 0 && h > 0);
    if (s_ready && zoom > 0) {
        s_view.lat = lat;
        s_view.lon = lon;
        s_view.rangeKm = rangeKm;
        s_view.zoom = zoom;
    }
    snprintf(s_label, sizeof(s_label), "%s", label ? label : "");
    snprintf(s_status, sizeof(s_status), "%s", status ? status : (s_ready ? "Radar updated" : "Weather unavailable"));
    s_dirty = true;
}

bool weather_get(int *w, int *h, char *label, size_t labelN, char *status, size_t statusN) {
    std::lock_guard<std::mutex> g(s_m);
    if (w) *w = s_ready ? s_w : 0;
    if (h) *h = s_ready ? s_h : 0;
    if (label && labelN) snprintf(label, labelN, "%s", s_label);
    if (status && statusN) snprintf(status, statusN, "%s", s_status);
    return s_ready;
}

bool weather_view(WeatherView *out) {
    std::lock_guard<std::mutex> g(s_m);
    if (!out || !s_ready || s_view.zoom <= 0) return false;
    *out = s_view;
    return true;
}

bool weather_take_dirty(void) {
    std::lock_guard<std::mutex> g(s_m);
    const bool d = s_dirty;
    s_dirty = false;
    return d;
}
