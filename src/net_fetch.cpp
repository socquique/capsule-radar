// Shared streamed-download helper (see net_fetch.h). Mirrors the proven pattern in
// photo_client.cpp: Content-Length -> stream into PSRAM; chunked -> getString decode.
#include "net_fetch.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs) {
    *out = nullptr; *outLen = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure cli;
    cli.setInsecure();                        // hobby device (matches the other clients)
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(totalTimeoutMs);
    if (!http.begin(cli, url)) return false;
    if (userAgent) http.setUserAgent(userAgent);

    const int code = http.GET();
    if (code != 200) { Serial.printf("[net] HTTP %d\n", code); http.end(); return false; }

    const int len = http.getSize();           // >0 = Content-Length; -1 = chunked/unknown
    uint8_t *buf = nullptr;
    size_t got = 0;

    if (len > 0) {
        // Known length: stream the body straight into a PSRAM buffer.
        const size_t cap = ((size_t)len <= maxLen) ? (size_t)len : maxLen;
        buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { http.end(); return false; }
        WiFiClient *stream = http.getStreamPtr();
        uint32_t last = millis();
        while (got < cap && (millis() - last) < (uint32_t)totalTimeoutMs) {
            const size_t avail = stream->available();
            if (avail) {
                const size_t want = (cap - got < avail) ? (cap - got) : avail;
                const int r = stream->readBytes(buf + got, want);
                if (r > 0) { got += r; last = millis(); }
            } else if (!http.connected()) {
                break;
            } else {
                delay(5);
            }
        }
    } else {
        // Chunked/unknown: getString() performs the chunk decode; only small payloads
        // are expected here (image CDNs send Content-Length), so the String is cheap.
        String body = http.getString();
        got = body.length();
        if (got > maxLen) got = maxLen;
        if (got > 0) {
            buf = (uint8_t *)heap_caps_malloc(got, MALLOC_CAP_SPIRAM);
            if (buf) memcpy(buf, body.c_str(), got);
            else got = 0;
        }
    }
    http.end();
    if (got == 0) { if (buf) heap_caps_free(buf); return false; }
    *out = buf; *outLen = got;
    return true;
}
