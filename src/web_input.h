#pragma once
// Strict parsing + repair of numbers that arrive from the config web page.
//
// Extracted so tests/web_input_test.cpp can exercise it on the host. Arduino's
// String::toDouble() stops at the first non-numeric character, so "38,84" silently
// becomes 38 and "" becomes 0 — both then pass a simple range check, get saved, and
// the device restarts centred on the wrong point. Everything here therefore validates
// the WHOLE string before returning a value.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

// Parse one decimal number: optional sign, digits, optional fractional part, the
// separator being '.' OR ',' (comma is the decimal key on Spanish keyboards). Rejects
// empty input, trailing garbage, hex, "inf"/"nan", and anything over 31 chars.
// Returns false (leaving *out untouched) on failure.
inline bool parseDoubleStrict(const char *s, double *out) {
    if (!s) return false;
    const char *p = s;
    if (*p == '+' || *p == '-') ++p;
    bool digits = false;
    while (*p >= '0' && *p <= '9') { ++p; digits = true; }
    if (*p == '.' || *p == ',') {          // at most one separator, nothing before/after it needed
        ++p;
        while (*p >= '0' && *p <= '9') { ++p; digits = true; }
    }
    if (!digits || *p != '\0') return false;   // empty, or characters left over
    if (p - s > 31) return false;               // longer than any sane coordinate
    char buf[32];
    for (const char *q = s; *q; ++q) buf[q - s] = (*q == ',') ? '.' : *q;  // strtod wants '.'
    buf[p - s] = '\0';
    *out = strtod(buf, nullptr);
    return true;
}

// Repair a display range that cannot work, and leave every other value as the user set
// it (a saved 15 or 25 km from the web page, 20 km from the zoom button). Not finite or
// <= 0 (0 divides by zero in geo::projectToScreen) -> RANGE_KM_DEFAULT; anything else is
// clamped to [RANGE_KM_MIN, RANGE_KM_MAX] — an old web-page 250 km becomes 150, which is
// as far as the feed query goes (ADSB_QUERY_MAX_KM).
inline float repairRangeKm(float km) {
    if (!isfinite(km) || km <= 0.0f) return RANGE_KM_DEFAULT;
    if (km < RANGE_KM_MIN) return RANGE_KM_MIN;
    if (km > RANGE_KM_MAX) return RANGE_KM_MAX;
    return km;
}
