// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we read the body into capped PSRAM,
// use an ArduinoJson field filter so only the ~12 fields we need are kept, and
// hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar then keeps only
// the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include "net_guard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void copy_trim(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    while (src && (*src == ' ' || *src == '\t')) ++src;
    snprintf(dst, n, "%s", src ? src : "");
    size_t len = strlen(dst);
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\t')) dst[--len] = 0;
}

void AdsbClient::setError(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_lastError, sizeof(_lastError), fmt ? fmt : "unknown", ap);
    va_end(ap);
}

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

bool AdsbClient::readBody(HTTPClient& http, const char *host, uint8_t **out, size_t *outLen) {
    *out = nullptr;
    *outLen = 0;

    const int len = http.getSize();
    if (len > (int)ADSB_MAX_JSON_BYTES) {
        setError("%s: JSON too large %d > %u", host, len, (unsigned)ADSB_MAX_JSON_BYTES);
        return false;
    }

    const size_t cap = (len > 0) ? (size_t)len : (size_t)ADSB_MAX_JSON_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        setError("%s: PSRAM alloc failed for %u bytes", host, (unsigned)cap);
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t last = millis();
    while (got < cap && millis() - last < 14000UL) {
        const size_t avail = stream->available();
        if (avail) {
            const size_t want = (cap - got < avail) ? (cap - got) : avail;
            const int r = stream->readBytes(buf + got, want);
            if (r > 0) {
                got += (size_t)r;
                last = millis();
            }
        } else if (!http.connected()) {
            break;
        } else {
            delay(5);
        }
    }

    if (len > 0 && got != (size_t)len) {
        setError("%s: short JSON body %u/%d bytes", host, (unsigned)got, len);
        heap_caps_free(buf);
        return false;
    }
    if (len <= 0 && got >= ADSB_MAX_JSON_BYTES) {
        setError("%s: JSON exceeded %u bytes", host, (unsigned)ADSB_MAX_JSON_BYTES);
        heap_caps_free(buf);
        return false;
    }
    if (!got) {
        setError("%s: empty JSON body", host);
        heap_caps_free(buf);
        return false;
    }

    *out = buf;
    *outLen = got;
    return true;
}

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) { setError("WiFi disconnected"); return false; }
    if (!tls_heap_ok("adsb")) {
        setError("low TLS memory; biggest block %u", (unsigned)tls_largest_internal_block());
        return false;
    }
    // Prefer the primary host, and give it a quick second try before touching the fallback:
    // the primary is reliable in practice, while the fallback can be slow to time out from
    // some networks (turning one transient primary blip into a long no-data gap + amber HUD).
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;   // transient blip -> retry the healthy host
    return fetchFrom(ADSB_FALLBACK_HOST, out);            // last resort
}

bool AdsbClient::fetchFrom(const char* host, std::vector<Aircraft>& out) {
    if (!tls_heap_ok("adsb")) {
        setError("%s: low TLS memory; biggest block %u", host, (unsigned)tls_largest_internal_block());
        return false;
    }
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];
    snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();                              // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(12000);          // ADS-B JSON can be slow/chunky right after boot
    if (!http.begin(client, url)) { setError("%s: http.begin failed", host); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != 200) { setError("%s: HTTP %d", host, code); http.end(); return false; }

    uint8_t *body = nullptr;
    size_t bodyLen = 0;
    if (!readBody(http, host, &body, &bodyLen)) {
        http.end();
        return false;
    }
    http.end();

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, (const char *)body, bodyLen,
                                               DeserializationOption::Filter(filter));
    heap_caps_free(body);
    if (err) { setError("%s: JSON %s (%u bytes)", host, err.c_str(), (unsigned)bodyLen); return false; }

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) { setError("%s: no aircraft array", host); return false; }

    out.clear();
    out.reserve(ADSB_MAX_AIRCRAFT);
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if ((int)out.size() >= ADSB_MAX_AIRCRAFT) break;   // hard cap: protect RAM

        if (a["lat"].isNull() || a["lon"].isNull()) continue;  // need a position

        Aircraft ac;
        copy_trim(ac.hex, sizeof(ac.hex), a["hex"] | "");
        if (ac.hex[0] == 0) continue;
        copy_trim(ac.flight, sizeof(ac.flight), a["flight"] | "");
        copy_trim(ac.type, sizeof(ac.type), a["t"] | "");
        ac.lat    = a["lat"].as<double>();
        ac.lon    = a["lon"].as<double>();

        if (a["alt_baro"].is<const char*>()) { ac.onGround = true; ac.altBaro = 0; }  // "ground"
        else                                  ac.altBaro = a["alt_baro"] | 0.0f;

        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        out.push_back(ac);
    }

    _lastOkMs = now;
    _lastHostRole = (strcmp(host, ADSB_PRIMARY_HOST) == 0) ? "primary" : "secondary";
    snprintf(_lastError, sizeof(_lastError), "ok");
    return true;
}
