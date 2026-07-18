#include "wx_radar_client.h"
#include "wx_radar.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>

// PNG contains a ~46 KB inflate workspace. A global PNG object places that in
// scarce internal BSS and starves TLS of a contiguous allocation, breaking every
// HTTPS client. Construct the decoder itself in PSRAM instead.
static PNG *s_png = nullptr;

static bool ensure_decoder(void) {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) { Serial.println("[wxradar] PSRAM decoder allocation failed"); return false; }
    s_png = new (mem) PNG();
    Serial.printf("[wxradar] PNG decoder in PSRAM (%u bytes)\n", (unsigned)sizeof(PNG));
    return true;
}

static int radar_png_line(PNGDRAW *draw) {
    uint16_t *dst = wx_radar_back_buffer();
    const int crop = (WX_RADAR_SOURCE_SIZE - WX_RADAR_SIZE) / 2;
    if (!dst || draw->y < crop || draw->y >= crop + WX_RADAR_SIZE) return 1;
    uint16_t line[WX_RADAR_SOURCE_SIZE];
    s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    const int outY = draw->y - crop;
    const int c = WX_RADAR_SIZE / 2;
    const int dy = outY - c;
    for (int outX = 0; outX < WX_RADAR_SIZE; ++outX) {
        const int dx = outX - c;
        dst[outY * WX_RADAR_SIZE + outX] =
            (dx * dx + dy * dy <= (c - 2) * (c - 2)) ? line[outX + crop] : 0;
    }
    return 1;
}

static bool https_get_string(const char *url, String &body, int timeoutMs) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3500);
    http.setTimeout(timeoutMs);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        char tls[128] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        Serial.printf("[wxradar] HTTP %d tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      status, tlsCode, tls, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        http.end(); return false;
    }
    body = http.getString();
    http.end();
    return body.length() > 0;
}

bool wx_radar_fetch(double lat, double lon) {
    if (WiFi.status() != WL_CONNECTED || !wx_radar_back_buffer() || !ensure_decoder()) return false;

    String meta;
    if (!https_get_string("https://api.rainviewer.com/public/weather-maps.json", meta, 6500)) {
        Serial.println("[wxradar] metadata fetch failed"); return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, meta)) { Serial.println("[wxradar] metadata JSON failed"); return false; }
    const char *host = doc["host"] | "";
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    if (!host[0] || past.size() == 0) { Serial.println("[wxradar] no radar frames"); return false; }
    JsonObjectConst latest = past[past.size() - 1].as<JsonObjectConst>();
    const char *path = latest["path"] | "";
    const uint32_t frameTime = latest["time"] | 0;

    char url[320];
    snprintf(url, sizeof(url), "%s%s/512/7/%.5f/%.5f/2/1_1.png",
             host, path, lat, lon);
    String image;
    if (!https_get_string(url, image, 8500)) { Serial.println("[wxradar] tile fetch failed"); return false; }
    const int opened = s_png->openRAM((uint8_t *)image.c_str(), image.length(), radar_png_line);
    if (opened != PNG_SUCCESS) { Serial.printf("[wxradar] PNG open error %d\n", opened); return false; }
    const int decoded = s_png->decode(nullptr, 0);
    s_png->close();
    if (decoded != PNG_SUCCESS) { Serial.printf("[wxradar] PNG decode error %d\n", decoded); return false; }
    wx_radar_commit(frameTime, lat, lon);
    Serial.printf("[wxradar] frame %lu updated (%u bytes)\n",
                  (unsigned long)frameTime, (unsigned)image.length());
    return true;
}
