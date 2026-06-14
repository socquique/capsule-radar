#include "weather_client.h"
#include "config.h"
#include "net_guard.h"
#include "app_log.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define WX_UA "CapsuleRadar/1.0 (+https://github.com/socquique/capsule-radar)"
#define WX_MIN_ZOOM 1
#define WX_MAX_ZOOM 7

static lv_color_t *s_dst = nullptr;
static int s_dstW = 0, s_dstH = 0;

static bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= s_dstH) continue;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= s_dstW) continue;
            s_dst[yy * s_dstW + xx].full = bmp[j * w + i];
        }
    }
    return true;
}

static bool http_get(const char *url, uint8_t **out, size_t *outLen, size_t maxLen, bool allowTruncate = false) {
    *out = nullptr;
    *outLen = 0;
    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(5000);
    http.setTimeout(9000);
    if (!http.begin(cli, url)) return false;
    http.setUserAgent(WX_UA);
    const int code = http.GET();
    if (code != 200) { http.end(); return false; }

    const int len = http.getSize();
    uint8_t *buf = nullptr;
    size_t got = 0;
    if (len > 0) {
        if ((size_t)len > maxLen && !allowTruncate) { http.end(); return false; }
        const size_t cap = ((size_t)len <= maxLen) ? (size_t)len : maxLen;
        buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { http.end(); return false; }
        WiFiClient *stream = http.getStreamPtr();
        uint32_t last = millis();
        while (got < cap && millis() - last < 12000UL) {
            const size_t avail = stream->available();
            if (avail) {
                const size_t want = (cap - got < avail) ? (cap - got) : avail;
                const int r = stream->readBytes(buf + got, want);
                if (r > 0) { got += (size_t)r; last = millis(); }
            } else if (!http.connected()) {
                break;
            } else {
                delay(5);
            }
        }
    } else {
        String body = http.getString();
        got = body.length();
        if (got > maxLen && !allowTruncate) { http.end(); return false; }
        if (got > maxLen) got = maxLen;
        if (got > 0) {
            buf = (uint8_t *)heap_caps_malloc(got, MALLOC_CAP_SPIRAM);
            if (buf) memcpy(buf, body.c_str(), got);
            else got = 0;
        }
    }
    http.end();
    if (!got) { if (buf) heap_caps_free(buf); return false; }
    *out = buf;
    *outLen = got;
    return true;
}

struct WxPsramAlloc : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static WxPsramAlloc s_jsonPsram;

static bool png_header(const uint8_t *buf, size_t len) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    return buf && len >= sizeof(sig) && memcmp(buf, sig, sizeof(sig)) == 0;
}

static bool build_supported_tile_url(char *url, size_t urlN, const char *host, const char *path,
                                     double lat, double lon, float rangeKm, int *chosenZoom) {
    (void)rangeKm;
    for (int zoom = WX_MAX_ZOOM; zoom >= WX_MIN_ZOOM; --zoom) {
        snprintf(url, urlN, "%s%s/512/%d/%.5f/%.5f/2/1_1.png",
                 host, path, zoom, lat, lon);

        uint8_t *probe = nullptr;
        size_t probeLen = 0;
        const bool ok = http_get(url, &probe, &probeLen, 64, true) && png_header(probe, probeLen);
        if (probe) heap_caps_free(probe);
        if (ok) {
            if (chosenZoom) *chosenZoom = zoom;
            return true;
        }
    }
    return false;
}

bool weather_fetch(const WeatherRequest &req) {
    const uint32_t startMs = millis();
    const uint32_t heapStart = tls_largest_internal_block();
    app_log_printf("[weather] fetch start %.5f, %.5f heap %u\n",
                  req.lat, req.lon, (unsigned)heapStart);
    if (WiFi.status() != WL_CONNECTED) {
        app_log_println("[weather] skipped: WiFi down");
        weather_commit(req.id, 0, 0, "", "WiFi is down");
        return false;
    }
    if (!tls_heap_ok("weather")) {
        app_log_printf("[weather] skipped: low TLS memory heap %u\n",
                      (unsigned)tls_largest_internal_block());
        weather_commit(req.id, 0, 0, "", "Low memory, skipped weather");
        return false;
    }

    uint8_t *jbuf = nullptr;
    size_t jlen = 0;
    if (!http_get("https://api.rainviewer.com/public/weather-maps.json", &jbuf, &jlen, 16384)) {
        app_log_printf("[weather] metadata failed after %lu ms\n", (unsigned long)(millis() - startMs));
        weather_commit(req.id, 0, 0, "", "Weather metadata failed");
        return false;
    }

    JsonDocument filter(&s_jsonPsram);
    filter["host"] = true;
    filter["radar"]["past"][0]["time"] = true;
    filter["radar"]["past"][0]["path"] = true;
    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, jbuf, jlen, DeserializationOption::Filter(filter));
    heap_caps_free(jbuf);
    if (err) {
        app_log_printf("[weather] metadata parse failed: %s\n", err.c_str());
        weather_commit(req.id, 0, 0, "", "Weather metadata parse failed");
        return false;
    }

    const char *host = doc["host"] | "";
    JsonArray past = doc["radar"]["past"].as<JsonArray>();
    if (!host[0] || past.isNull() || past.size() == 0) {
        app_log_println("[weather] no radar frames available");
        weather_commit(req.id, 0, 0, "", "No radar frames available");
        return false;
    }
    JsonObject frame = past[past.size() - 1].as<JsonObject>();
    const char *path = frame["path"] | "";
    const time_t frameTime = (time_t)(frame["time"] | 0);
    if (!path[0]) {
        app_log_println("[weather] no radar path in metadata");
        weather_commit(req.id, 0, 0, "", "No radar path available");
        return false;
    }

    int zoom = 0;
    char tileUrl[240];
    if (!build_supported_tile_url(tileUrl, sizeof(tileUrl), host, path, req.lat, req.lon, req.rangeKm, &zoom)) {
        app_log_println("[weather] no supported RainViewer zoom");
        weather_commit(req.id, 0, 0, "", "No supported weather zoom");
        return false;
    }
    app_log_printf("[weather] tile zoom %d selected\n", zoom);

    const char *bare = tileUrl;
    if (strncmp(bare, "https://", 8) == 0) bare += 8;
    else if (strncmp(bare, "http://", 7) == 0) bare += 7;

    int maxW = 0, maxH = 0;
    weather_buffer(&maxW, &maxH);
    char proxUrl[320];
    snprintf(proxUrl, sizeof(proxUrl),
             "https://images.weserv.nl/?url=%s&w=%d&h=%d&fit=cover&output=jpg",
             bare, maxW, maxH);

    uint8_t *img = nullptr;
    size_t ilen = 0;
    if (!http_get(proxUrl, &img, &ilen, 200000)) {
        app_log_printf("[weather] image failed after %lu ms\n", (unsigned long)(millis() - startMs));
        weather_commit(req.id, 0, 0, "", "Weather image failed");
        return false;
    }

    lv_color_t *dst = weather_buffer(&maxW, &maxH);
    uint16_t jw = 0, jh = 0;
    if (TJpgDec.getJpgSize(&jw, &jh, img, ilen) != JDR_OK || jw == 0 || jh == 0) {
        heap_caps_free(img);
        app_log_println("[weather] JPEG invalid");
        weather_commit(req.id, 0, 0, "", "Weather JPEG invalid");
        return false;
    }
    uint8_t scale = 1;
    while ((jw / scale) > (uint16_t)maxW || (jh / scale) > (uint16_t)maxH) {
        scale <<= 1;
        if (scale >= 8) break;
    }
    s_dstW = (int)(jw / scale);
    s_dstH = (int)(jh / scale);
    if (s_dstW > maxW) s_dstW = maxW;
    if (s_dstH > maxH) s_dstH = maxH;
    s_dst = dst;
    for (int i = 0; i < maxW * maxH; ++i) s_dst[i].full = 0;

    TJpgDec.setJpgScale(scale);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(jpg_out);
    const JRESULT jr = TJpgDec.drawJpg(0, 0, img, ilen);
    heap_caps_free(img);
    if (jr != JDR_OK) {
        app_log_printf("[weather] JPEG decode failed: %d\n", (int)jr);
        weather_commit(req.id, 0, 0, "", "Weather JPEG decode failed");
        return false;
    }

    char label[48] = "Radar frame time unknown";
    if (frameTime > 0) {
        struct tm tmv;
        localtime_r(&frameTime, &tmv);
        strftime(label, sizeof(label), "Radar %H:%M", &tmv);
    }
    char status[72];
    snprintf(status, sizeof(status), "Wide-area radar, zoom %d", zoom);
    weather_commit(req.id, s_dstW, s_dstH, label, status, req.lat, req.lon, req.rangeKm, zoom);
    app_log_printf("[weather] ready %dx%d zoom %d in %lu ms heap %u->%u\n",
                  s_dstW, s_dstH, zoom, (unsigned long)(millis() - startMs),
                  (unsigned)heapStart, (unsigned)tls_largest_internal_block());
    return true;
}
