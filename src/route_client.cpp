// Route lookup via adsbdb.com (free, no API key): GET /v0/callsign/{callsign}.
// Returns origin/destination city names (English). Device-only.
#include "route_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>   // route-cache TTL

#define ROUTE_CACHE_MAX 200   // wrap the cache before it can crowd NVS

// strip spaces -> a valid NVS key (callsigns are <= 8 chars)
static void route_key(const char *callsign, char *out, size_t on) {
    size_t j = 0;
    for (const char *p = callsign; *p && j < on - 1; ++p)
        if (*p != ' ') out[j++] = *p;
    out[j] = 0;
}

#define ROUTE_FMT_VER 2   // bump to invalidate cached routes when the label format changes

void route_cache_begin() {
    Preferences p;
    if (!p.begin("routes", false)) return;
    if (p.getUChar("__v", 0) != ROUTE_FMT_VER) { p.clear(); p.putUChar("__v", ROUTE_FMT_VER); }
    p.end();
}

bool route_cache_get(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0]) return false;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return false;
    Preferences p;
    if (!p.begin("routes", true)) return false;
    String v = p.getString(key, "");     // stored as "epoch|from|to"
    p.end();
    if (v.length() == 0) return false;
    const int b1 = v.indexOf('|');
    if (b1 < 0) return false;
    const uint32_t ts = (uint32_t)v.substring(0, b1).toInt();
    const String rest = v.substring(b1 + 1);
    const int b2 = rest.indexOf('|');
    if (b2 < 0) return false;
    const uint32_t now = (uint32_t)time(nullptr);    // expire stale routes (reused callsigns)
    if (now > 1700000000UL && ts > 1700000000UL && (now - ts) > 86400UL) return false;  // 24 h TTL
    snprintf(from, fn, "%s", rest.substring(0, b2).c_str());
    snprintf(to, tn, "%s", rest.substring(b2 + 1).c_str());
    return true;
}

void route_cache_put(const char *callsign, const char *from, const char *to) {
    if (!callsign || !callsign[0]) return;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return;
    Preferences p;
    if (!p.begin("routes", false)) return;
    int n = p.getInt("__n", 0);
    if (n >= ROUTE_CACHE_MAX) { p.clear(); n = 0; }   // wrap to bound NVS usage
    String v = String((uint32_t)time(nullptr)) + "|" + String(from ? from : "") + "|" + String(to ? to : "");
    if (p.putString(key, v) > 0) p.putInt("__n", n + 1);
    p.end();
}

// Most recognizable short airport label: a cleaned-up name ("Teesside", "Palma de
// Mallorca", "London Heathrow"), falling back to the municipality, then the IATA code.
static void pick_airport(JsonObjectConst ap, char *out, size_t n) {
    String s = (const char *)(ap["name"] | "");
    s.replace(" International Airport", "");
    s.replace(" Regional Airport", "");
    s.replace(" Airport", "");
    s.replace(" International", "");
    s.trim();
    if (s.length() == 0 || s.length() > 18) {           // name missing or too long -> municipality/IATA
        const char *muni = ap["municipality"] | "";
        const char *iata = ap["iata_code"] | "";
        snprintf(out, n, "%s", muni[0] ? muni : iata);
        return;
    }
    snprintf(out, n, "%s", s.c_str());
}

bool route_fetch(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0] || WiFi.status() != WL_CONNECTED) return false;

    // strip spaces from the callsign
    char cs[12];
    size_t j = 0;
    for (const char *p = callsign; *p && j < sizeof(cs) - 1; ++p)
        if (*p != ' ') cs[j++] = *p;
    cs[j] = 0;
    if (j == 0) return false;

    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);   // short: runs on the feed task, don't stall the live poll
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    // MUST be setUserAgent(): addHeader() silently drops User-Agent (it is on
    // HTTPClient's "handled by code" list), leaving the default "ESP32HTTPClient".
    http.setUserAgent(ADSB_USER_AGENT);

    const int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument filter;
    filter["response"]["flightroute"]["origin"]["municipality"] = true;
    filter["response"]["flightroute"]["origin"]["iata_code"] = true;
    filter["response"]["flightroute"]["origin"]["name"] = true;
    filter["response"]["flightroute"]["destination"]["municipality"] = true;
    filter["response"]["flightroute"]["destination"]["iata_code"] = true;
    filter["response"]["flightroute"]["destination"]["name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonObjectConst fr = doc["response"]["flightroute"].as<JsonObjectConst>();
    if (fr.isNull()) return false;   // "unknown callsign" etc.

    pick_airport(fr["origin"].as<JsonObjectConst>(), from, fn);
    pick_airport(fr["destination"].as<JsonObjectConst>(), to, tn);
    return (from[0] || to[0]);
}
