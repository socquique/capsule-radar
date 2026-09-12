#pragma once
// Fetches nearby aircraft from airplanes.live (fallback adsb.lol) and parses
// the readsb JSON into a vector<Aircraft>. See docs/DATA_SOURCE.md.
#include <vector>
#include <Arduino.h>
#include "aircraft.h"
#include "config.h"
#include "adsb_pacing.h"

class AdsbClient {
public:
    void begin(double homeLat, double homeLon, float rangeKm);
    void setHome(double lat, double lon) { _lat = lat; _lon = lon; }
    void setRange(float km) { _rangeKm = km; }
    void setHideGround(bool h) { _hideGround = h; }   // skip on-ground aircraft during parse
    void setMinAltFt(float ft) { _minAltFt = ft; }    // skip aircraft below this altitude (0 = off)
    void setMaxAltFt(float ft) { _maxAltFt = ft; }    // skip aircraft above this altitude (0 = off)
    void setMilitaryOnly(bool m) { _milOnly = m; }    // keep only military-flagged aircraft

    // Fetch + parse. Returns true on success and fills `out` (replaces contents).
    // On failure, leaves `out` untouched and returns false (caller keeps last good).
    bool poll(std::vector<Aircraft>& out);

    uint32_t lastOkMs() const { return _lastOkMs; }

    // True when the last poll() issued no request at all because every provider was
    // parked or still inside its adaptive spacing window. That is deliberate pacing,
    // not an outage, and the caller must not treat it as a failed fetch.
    bool lastPollSkipped() const { return _lastPollSkipped; }

    // Which provider served the most recent successful fetch.
    const char* lastHost() const { return _lastHost ? _lastHost : "?"; }

    // millis() of the last completed HTTP exchange with any provider, refusals included.
    // A 403 arrives over a working TLS session, so it proves the heap and the network stack
    // are healthy — which is all the caller's "feed wedged -> reboot" watchdog is asking.
    // Without this a permanently refused feed reboots the device every three minutes, and
    // every boot asks all three providers again.
    uint32_t lastResponseMs() const { return _lastResponseMs; }

private:
    // `slot` indexes both the provider table in adsb_client.cpp and the pacing state below.
    bool fetchFrom(int slot, std::vector<Aircraft>& out);

    // Per-provider request pacing (park on 403, adaptive spacing on 429). Lives in
    // adsb_pacing.h so the policy can be driven on the host — see tests/adsb_pacing_test.cpp.
    AdsbPacer _pacer;

    bool cooling(int slot) const { return _pacer.cooling(slot, millis()); }

    double _lat = 0, _lon = 0;
    float  _rangeKm = 15.0f;
    bool   _hideGround = false;
    float  _minAltFt = 0.0f;
    float  _maxAltFt = 0.0f;
    bool   _milOnly = false;
    uint32_t _lastOkMs = 0;
    bool     _lastPollSkipped = false;
    uint32_t _lastResponseMs = 0;
    const char* _lastHost = nullptr;
};
