// Shared route state. std::mutex works on both ESP32 (Arduino/FreeRTOS) and the
// native simulator, so the same code guards the cross-thread access on the device.
#include "route.h"
#include <string.h>
#include <stdio.h>
#include <mutex>

static std::mutex s_m;
static char s_want[12]     = "";   // callsign the UI asked about
static char s_doneCall[12] = "";   // callsign the stored result belongs to
static char s_from[40]     = "";
static char s_to[40]       = "";
static double s_wantLat = 0.0;
static double s_wantLon = 0.0;

void route_request(const char *callsign, double lat, double lon) {
    std::lock_guard<std::mutex> g(s_m);
    snprintf(s_want, sizeof(s_want), "%s", callsign ? callsign : "");
    s_wantLat = lat;
    s_wantLon = lon;
}

bool route_pending(char *callOut, size_t n, double *latOut, double *lonOut) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_want[0] && strcmp(s_want, s_doneCall) != 0) {
        snprintf(callOut, n, "%s", s_want);
        if (latOut) *latOut = s_wantLat;
        if (lonOut) *lonOut = s_wantLon;
        return true;
    }
    return false;
}

// Fold UTF-8 Latin accents (á, ñ, ü...) to ASCII so the LVGL font can render them
// (Montserrat has no accented glyphs -> they would show as a missing-glyph box).
static void ascii_fold(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < n;) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out[o++] = in[i++]; continue; }
        if (c == 0xC3 && in[i + 1]) {            // Latin-1 Supplement
            const unsigned char d = (unsigned char)in[i + 1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else if (d == 0x9F)              r = 's';   // ß
            else                             r = '?';
            out[o++] = r; i += 2; continue;
        }
        ++i;                                     // other multibyte: skip the sequence
        while ((unsigned char)in[i] >= 0x80 && (unsigned char)in[i] < 0xC0) ++i;
    }
    out[o] = 0;
}

void route_store(const char *callsign, const char *from, const char *to) {
    std::lock_guard<std::mutex> g(s_m);
    snprintf(s_doneCall, sizeof(s_doneCall), "%s", callsign ? callsign : "");
    ascii_fold(from, s_from, sizeof(s_from));
    ascii_fold(to,   s_to,   sizeof(s_to));
}

bool route_get(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    std::lock_guard<std::mutex> g(s_m);
    if (callsign && s_doneCall[0] && strcmp(callsign, s_doneCall) == 0) {
        snprintf(from, fn, "%s", s_from);
        snprintf(to, tn, "%s", s_to);
        return true;
    }
    return false;
}
