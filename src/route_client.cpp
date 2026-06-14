// Route lookup via the adsb.lol tar1090-compatible routeset API.
#include "route_client.h"
#include "config.h"
#include "net_guard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

struct RoutePsramAlloc : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static RoutePsramAlloc s_jsonPsram;

static bool append_code(char *out, size_t n, const char *code) {
    if (!out || n == 0 || !code || !code[0]) return false;
    const size_t used = strlen(out);
    if (used > 0) {
        if (used + 4 >= n) return false;
        strncat(out, " -> ", n - used - 1);
    }
    const size_t nowUsed = strlen(out);
    strncat(out, code, n - nowUsed - 1);
    return true;
}

bool route_fetch(const char *callsign, double lat, double lon, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0] || WiFi.status() != WL_CONNECTED) return false;
    if (!isfinite(lat) || !isfinite(lon) || (lat == 0.0 && lon == 0.0)) return false;
    if (!tls_heap_ok("route")) return false;

    char cs[12];
    size_t j = 0;
    for (const char *p = callsign; *p && j < sizeof(cs) - 1; ++p)
        if (*p != ' ') cs[j++] = *p;
    cs[j] = 0;
    if (j == 0) return false;

    char body[96];
    snprintf(body, sizeof(body), "{\"planes\":[{\"callsign\":\"%s\",\"lat\":%.5f,\"lng\":%.5f}]}", cs, lat, lon);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);
    http.setTimeout(6000);
    if (!http.begin(client, "https://api.adsb.lol/api/0/routeset")) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json, text/javascript, */*; q=0.01");
    http.addHeader("Content-Type", "application/json; charset=utf-8");
    http.addHeader("Origin", "https://adsb.lol");
    http.addHeader("Referer", "https://adsb.lol/");

    int code = http.POST((uint8_t *)body, strlen(body));
    if (code != 200 || http.getSize() == 0) {
        http.end();
        return false;
    }

    JsonDocument filter(&s_jsonPsram);
    filter[0]["plausible"] = true;
    filter[0]["_airports"][0]["iata"] = true;
    filter[0]["_airports"][0]["icao"] = true;
    filter[0]["_airports"][1]["iata"] = true;
    filter[0]["_airports"][1]["icao"] = true;
    filter[0]["_airports"][2]["iata"] = true;
    filter[0]["_airports"][2]["icao"] = true;
    filter[0]["_airports"][3]["iata"] = true;
    filter[0]["_airports"][3]["icao"] = true;

    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonObjectConst route = doc[0].as<JsonObjectConst>();
    if (route.isNull()) return false;
    const bool questionable = route["plausible"].is<bool>() && route["plausible"].as<bool>() == false;

    JsonArrayConst airports = route["_airports"].as<JsonArrayConst>();
    if (airports.isNull() || airports.size() < 2) return false;

    bool wroteFrom = false;
    to[0] = 0;
    for (JsonObjectConst ap : airports) {
        const char *codeText = ap["iata"] | "";
        if (!codeText[0]) codeText = ap["icao"] | "";
        if (!codeText[0]) continue;
        if (!wroteFrom) {
            snprintf(from, fn, "%s%s", questionable ? "?? " : "", codeText);
            wroteFrom = true;
        } else {
            append_code(to, tn, codeText);
        }
    }
    return from[0] && to[0];
}
