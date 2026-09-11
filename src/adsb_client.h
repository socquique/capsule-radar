#pragma once
// Fetches nearby aircraft from airplanes.live (fallback adsb.lol) and parses
// the readsb JSON into a vector<Aircraft>. See docs/DATA_SOURCE.md.
#include <vector>
#include <Arduino.h>
#include "aircraft.h"

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

private:
    // `slot` indexes the per-provider cooldown below: 0 = primary, 1 = fallback.
    bool fetchFrom(const char* host, int slot, std::vector<Aircraft>& out);

    // Per-provider pacing. Two separate mechanisms, because the two failures differ:
    //
    //   403 -> a policy refusal (needs approval, or a User-Agent they reject). It will not
    //          clear in seconds, so the provider is PARKED outright. Retrying it every poll
    //          only burns a request and doubles the rate onto the surviving provider.
    //
    //   429 -> we are simply too fast. adsb.lol documents its limits as "dynamic based on
    //          the environment load", so there is no fixed rate to hard-code. Instead keep
    //          an adaptive minimum SPACING per provider: double it on every 429, and ease
    //          it back down after a run of successes. That settles on the fastest rate the
    //          provider will currently tolerate, instead of flapping between full speed and
    //          a dead stop. An explicit Retry-After still wins and parks us for that long.
    //
    // All time comparisons are millis()-rollover-safe via signed subtraction.
    uint32_t _cooldownUntil[2]  = {0, 0};
    uint32_t _spacingMs[2]      = {0, 0};
    uint32_t _lastAttemptMs[2]  = {0, 0};
    uint16_t _okStreak[2]       = {0, 0};
    bool     _cooldownLogged[2] = {false, false};

    bool cooling(int slot) const {
        if ((int32_t)(_cooldownUntil[slot] - millis()) > 0) return true;
        if (_spacingMs[slot] &&
            (int32_t)(millis() - _lastAttemptMs[slot]) < (int32_t)_spacingMs[slot]) return true;
        return false;
    }

    double _lat = 0, _lon = 0;
    float  _rangeKm = 15.0f;
    bool   _hideGround = false;
    float  _minAltFt = 0.0f;
    float  _maxAltFt = 0.0f;
    bool   _milOnly = false;
    uint32_t _lastOkMs = 0;
    bool     _lastPollSkipped = false;
};
