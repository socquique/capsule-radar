// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include "geo.h"           // haversineKm — keep the nearest N aircraft
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

// NetworkClient::readBytes() treats a transient negative TLS read as end-of-input,
// which makes ArduinoJson intermittently report IncompleteInput. Deliberately wrap
// the client without overriding readBytes(): Stream's timed byte reader retries
// temporary no-data reads until the configured timeout.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override {
        const int value = _source.read();
        if (value >= 0) ++_bytesRead;
        return value;
    }
    int peek() override { return _source.peek(); }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

// Independent providers, tried in order. Same readsb payload, different URL shapes.
// Order matters: airplanes.live stays first so an approved key is used when available
// (it parks itself in seconds if not), then adsb.fi, whose 1 req/s limit is documented
// and fixed, then adsb.lol, whose limits are dynamic and throttle hardest.
struct AdsbProvider {
    const char* host;
    const char* pathFmt;   // lat, lon, radius-in-nm
};
static const AdsbProvider kProviders[ADSB_PROVIDER_COUNT] = {
    { ADSB_PRIMARY_HOST,  "/v2/point/%.4f/%.4f/%.0f"        },
    { ADSB_OPENDATA_HOST, "/api/v3/lat/%.4f/lon/%.4f/dist/%.0f" },
    { ADSB_FALLBACK_HOST, "/v2/point/%.4f/%.4f/%.0f"        },
};

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Try each independent provider once, skipping any that is parked or still inside its
    // spacing window. Retrying a hard-failing provider every poll achieves nothing and just
    // pushes the surviving ones over their own limits.
    bool askedSomeone = false;
    for (int i = 0; i < ADSB_PROVIDER_COUNT; ++i) {
        if (cooling(i)) continue;
        askedSomeone = true;
        if (fetchFrom(i, out)) { _lastPollSkipped = false; return true; }
    }
    _lastPollSkipped = !askedSomeone;          // nothing was asked; not a failure
    return false;
}

bool AdsbClient::fetchFrom(int slot, std::vector<Aircraft>& out) {
    const char* host = kProviders[slot].host;
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char path[96];
    snprintf(path, sizeof(path), kProviders[slot].pathFmt, _lat, _lon, nm);
    char url[160];
    snprintf(url, sizeof(url), "https://%s%s", host, path);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();                              // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif

    _lastAttemptMs[slot] = millis();

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(8000);           // task (and the user's route/photo lookups) for too long
    if (!http.begin(client, url)) { Serial.printf("[adsb] begin failed (%s)\n", host); return false; }
    // MUST be setUserAgent(): addHeader() silently drops User-Agent (it is on
    // HTTPClient's "handled by code" list), leaving the default "ESP32HTTPClient".
    http.setUserAgent(ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");
    const char* wanted[] = { "Retry-After" };
    http.collectHeaders(wanted, 1);

    const int code = http.GET();
    if (code != 200) {
        char tls[128] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        // Log a bounded slice of the error body. Providers explain themselves here
        // ("contact us for access", "rate limited", ...) and throwing it away turns an
        // actionable message into a bare status code. Bounded + time-capped so a hostile
        // or hanging response can never stall the poll task.
        char body[161] = "";
        if (code > 0) {
            NetworkClient& es = http.getStream();
            size_t n = 0;
            const uint32_t t0 = millis();
            while (n < sizeof(body) - 1 && (millis() - t0) < 500) {
                if (!es.available()) {
                    if (!es.connected()) break;
                    delay(5);
                    continue;
                }
                const int c = es.read();
                if (c < 0) break;
                body[n++] = (c == '\r' || c == '\n') ? ' ' : (char)c;
            }
            body[n] = '\0';
        }
        Serial.printf("[adsb] HTTP %d (%s) tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      code, host, tlsCode, tls,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        if (body[0]) Serial.printf("[adsb]   body: %s\n", body);

        if (code == 403) {
            // Policy refusal: park it. Announce once, not on every poll.
            _cooldownUntil[slot] = millis() + ADSB_COOLDOWN_403_MS;
            if (!_cooldownLogged[slot]) {
                Serial.printf("[adsb] %s parked for %us after HTTP 403\n",
                              host, (unsigned)(ADSB_COOLDOWN_403_MS / 1000));
                _cooldownLogged[slot] = true;
            }
        } else if (code == 429) {
            _okStreak[slot] = 0;
            const long ra = http.header("Retry-After").toInt();
            if (ra > 0) _cooldownUntil[slot] = millis() + (uint32_t)ra * 1000UL;
            // Back off multiplicatively; the limit is dynamic, so probe for what sticks.
            uint32_t sp = _spacingMs[slot] ? _spacingMs[slot] * 2 : ADSB_SPACING_STEP_MS;
            if (sp > ADSB_SPACING_MAX_MS) sp = ADSB_SPACING_MAX_MS;
            if (sp != _spacingMs[slot]) {
                _spacingMs[slot] = sp;
                Serial.printf("[adsb] %s rate-limited; spacing requests %us apart\n",
                              host, (unsigned)(sp / 1000));
            }
        }
        http.end(); return false;
    }

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
    const int expectedBytes = http.getSize();
    NetworkClient& responseStream = http.getStream();
    ReliableJsonStream jsonStream(responseStream);
    // Match the HTTP client's budget. Without this the wrapper keeps Arduino Stream's default
    // 1000 ms, so a TLS stall longer than 1 s mid-body still dropped the poll with
    // IncompleteInput — proven by tests/adsb_json_stream_test.cpp case 3 (found by @geoffg28).
    jsonStream.setTimeout(8000);
    DeserializationError err = deserializeJson(doc, jsonStream,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[adsb] JSON parse failed (%s): %s; expected=%d read=%u available=%d connected=%d\n",
                      host, err.c_str(), expectedBytes, (unsigned)jsonStream.bytesRead(),
                      responseStream.available(), responseStream.connected());
        http.end();
        return false;
    }
    http.end();
    _cooldownLogged[slot] = false;      // healthy again: allow a future park to be announced

    // Ease the imposed gap back down after a sustained good run, so a one-off busy period
    // upstream does not slow us permanently. Additive decrease against the multiplicative
    // increase above: quick to back off, cautious to speed up.
    if (_spacingMs[slot] && ++_okStreak[slot] >= ADSB_SPACING_EASE_OKS) {
        _okStreak[slot] = 0;
        _spacingMs[slot] = (_spacingMs[slot] > ADSB_SPACING_STEP_MS)
                             ? _spacingMs[slot] - ADSB_SPACING_STEP_MS : 0;
        Serial.printf("[adsb] %s steady; spacing eased to %us\n",
                      host, (unsigned)(_spacingMs[slot] / 1000));
    }

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    // Keep the ADSB_MAX_AIRCRAFT *nearest* aircraft (not just the first ones the feed happens to
    // list), so busy areas still show the traffic closest to you. We gate by distance BEFORE
    // parsing the strings, so the hundreds of far-away aircraft never allocate anything.
    std::vector<Aircraft> tmp;
    std::vector<float>     dist;             // parallel array: km from home for each kept aircraft
    tmp.reserve(ADSB_MAX_AIRCRAFT);
    dist.reserve(ADSB_MAX_AIRCRAFT);
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if (a["lat"].isNull() || a["lon"].isNull()) continue;   // need a position
        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();

        // alt_baro is the string "ground" for aircraft on the ground; skip them if hide-ground is on.
        const bool  onGround = a["alt_baro"].is<const char*>();
        const float altFt    = onGround ? 0.0f : (a["alt_baro"] | 0.0f);
        if (_hideGround && onGround) continue;
        // optional filters (applied before the cap, so slots only go to matching aircraft)
        if (_minAltFt > 0.0f && (onGround || altFt < _minAltFt)) continue;
        if (_maxAltFt > 0.0f && !onGround && altFt > _maxAltFt) continue;  // low-traffic/heli spotting
        if (_milOnly && (((a["dbFlags"] | 0u) & 0x1) == 0)) continue;

        const float d = (float)geo::haversineKm(_lat, _lon, lat, lon);

        // nearest-N gate: if the buffer is full and this one isn't closer than the farthest kept,
        // drop it now — before any string allocation.
        int farIdx = -1;
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) {
            farIdx = 0;
            for (int i = 1; i < (int)dist.size(); ++i) if (dist[i] > dist[farIdx]) farIdx = i;
            if (d >= dist[farIdx]) continue;
        }

        Aircraft ac;
        ac.hex = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat = lat; ac.lon = lon;
        ac.onGround = onGround;
        ac.altBaro  = altFt;
        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        if (farIdx >= 0) { tmp[farIdx] = std::move(ac); dist[farIdx] = d; }   // replace the farthest kept
        else             { tmp.push_back(std::move(ac)); dist.push_back(d); }
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}
