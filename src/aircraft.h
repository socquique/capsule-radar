#pragma once
// Aircraft data model + small presentation helpers.
#include <math.h>
#include <stdint.h>

struct Aircraft {
    char     hex[8] = "";     // ICAO 24-bit id (stable key)
    char     flight[12] = ""; // callsign
    char     type[8] = "";    // e.g. "B738" (when available)
    double   lat = 0, lon = 0;
    float    altBaro = 0;    // ft (NAN if on ground)
    bool     onGround = false;
    float    track = NAN;    // ground track deg (fallback to true_heading)
    float    gs = NAN;       // ground speed kt
    float    baroRate = NAN; // vertical rate fpm
    int      squawk = -1;
    uint32_t seenPos = 0;    // s since last position
    bool     military = false;
    uint32_t lastUpdateMs = 0; // millis() when we last saw it (for trails/expiry)
};

inline bool acIsEmergency(int squawk) {
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

// Pack RGB into RGB565 for the panel.
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Altitude color map (matches the mockup legend).
inline uint16_t altitudeColor565(float altFt, bool onGround) {
    if (onGround)        return rgb565(0x88, 0x88, 0x88);
    if (altFt <  3000)   return rgb565(0xFF, 0x5A, 0x3C); // red
    if (altFt < 10000)   return rgb565(0xFF, 0xB2, 0x3C); // amber
    if (altFt < 20000)   return rgb565(0xC8, 0xFF, 0x3C); // lime
    if (altFt < 30000)   return rgb565(0x39, 0xFF, 0x14); // green
    return                      rgb565(0x3C, 0xE0, 0xFF); // cyan
}
