#pragma once
// Strict parsing + snapping of numbers that arrive from the config web page.
//
// Extracted so tests/web_input_test.cpp can exercise it on the host. Arduino's
// String::toDouble() stops at the first non-numeric character, so "38,84" silently
// becomes 38 and "" becomes 0 — both then pass a simple range check, get saved, and
// the device restarts centred on the wrong point. Everything here therefore validates
// the WHOLE string before returning a value.
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

// Nearest entry of RANGE_STEPS_KM (ties go to the lower step). Also repairs a stored
// value the presets don't contain — 0 would divide by zero in geo::projectToScreen,
// and the old web page's 250 km promise is clamped away by ADSB_QUERY_MAX_KM anyway.
inline float snapRangeKm(float km) {
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    int best = 0;
    float bestD = km - RANGE_STEPS_KM[0];
    if (bestD < 0) bestD = -bestD;
    for (int i = 1; i < n; ++i) {
        float d = km - RANGE_STEPS_KM[i];
        if (d < 0) d = -d;
        if (d < bestD) { bestD = d; best = i; }
    }
    return RANGE_STEPS_KM[best];
}
