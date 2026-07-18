#include "cloud_image_client.h"
#include "cloud_image.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <time.h>

static uint16_t *s_dst = nullptr;

static bool cloud_jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    if (!s_dst) return false;
    const int c = WX_RADAR_SIZE / 2;
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= WX_RADAR_SIZE) continue;
        const int dy = yy - c;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= WX_RADAR_SIZE) continue;
            const int dx = xx - c;
            s_dst[yy * WX_RADAR_SIZE + xx] =
                (dx * dx + dy * dy <= (c - 2) * (c - 2)) ? bmp[j * w + i] : 0;
        }
    }
    return true;
}

static bool get_jpeg(const char *url, uint8_t **out, size_t *outLen) {
    *out = nullptr; *outLen = 0;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4500);
    http.setTimeout(12000);
    if (!http.begin(client, url)) return false;
    http.setUserAgent(ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        char tls[96] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        Serial.printf("[clouds] HTTP %d tls=%d '%s'\n", status, tlsCode, tls);
        http.end(); return false;
    }
    String body = http.getString();
    http.end();
    if (body.length() < 100 || body.length() > 180000) return false;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(body.length(), MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    memcpy(buf, body.c_str(), body.length());
    *out = buf; *outLen = body.length();
    return true;
}

bool cloud_image_fetch(double lat, double lon) {
    if (WiFi.status() != WL_CONNECTED || !cloud_image_back_buffer()) return false;
    // A roughly 440 x 440 km aviation-context window around the configured position.
    const double latSpan = 2.0;
    const double lonSpan = 3.0;
    char url[720];
    snprintf(url, sizeof(url),
        "https://view.eumetsat.int/geoserver/wms?service=WMS&version=1.1.1&request=GetMap"
        "&layers=mtg_fd%%3Argb_cloudtype%%2Cbackgrounds%%3Ane_10m_coastline%%2Cbackgrounds%%3Ane_boundary_lines_land"
        "&srs=EPSG%%3A4326&bbox=%.5f%%2C%.5f%%2C%.5f%%2C%.5f&width=360&height=360"
        "&styles=&format=image%%2Fjpeg&bgcolor=0x000000",
        lon - lonSpan, lat - latSpan, lon + lonSpan, lat + latSpan);

    uint8_t *image = nullptr; size_t imageLen = 0;
    if (!get_jpeg(url, &image, &imageLen)) {
        Serial.println("[clouds] image fetch failed"); return false;
    }
    uint16_t jw = 0, jh = 0;
    if (TJpgDec.getJpgSize(&jw, &jh, image, imageLen) != JDR_OK ||
        jw != WX_RADAR_SIZE || jh != WX_RADAR_SIZE) {
        Serial.printf("[clouds] unexpected JPEG %ux%u\n", jw, jh);
        heap_caps_free(image); return false;
    }
    s_dst = cloud_image_back_buffer();
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(cloud_jpg_out);
    const JRESULT result = TJpgDec.drawJpg(0, 0, image, imageLen);
    heap_caps_free(image);
    s_dst = nullptr;
    if (result != JDR_OK) {
        Serial.printf("[clouds] JPEG decode error %d\n", result); return false;
    }
    const uint32_t frameTime = (uint32_t)time(nullptr);
    cloud_image_commit(frameTime, lat, lon);
    Serial.printf("[clouds] satellite image updated (%u bytes)\n", (unsigned)imageLen);
    return true;
}
