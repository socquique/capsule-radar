#pragma once
// Fetches nearby aircraft from airplanes.live (fallback adsb.lol) and parses
// the readsb JSON into a vector<Aircraft>. See docs/DATA_SOURCE.md.
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "aircraft.h"

class HTTPClient;

class AdsbClient {
public:
    void begin(double homeLat, double homeLon, float rangeKm);
    void setHome(double lat, double lon) { _lat = lat; _lon = lon; }
    void setRange(float km) { _rangeKm = km; }

    // Fetch + parse. Returns true on success and fills `out` (replaces contents).
    // On failure, leaves `out` untouched and returns false (caller keeps last good).
    bool poll(std::vector<Aircraft>& out);

    uint32_t lastOkMs() const { return _lastOkMs; }
    const char *lastError() const { return _lastError; }
    const char *lastHostRole() const { return _lastHostRole; }

private:
    bool fetchFrom(const char* host, std::vector<Aircraft>& out);   // one host, one attempt
    bool readBody(HTTPClient& http, const char *host, uint8_t **out, size_t *outLen);
    void setError(const char *fmt, ...);

    double _lat = 0, _lon = 0;
    float  _rangeKm = 15.0f;
    uint32_t _lastOkMs = 0;
    char _lastError[96] = "not polled yet";
    const char *_lastHostRole = "unknown";
};
