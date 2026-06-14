#pragma once
// Shared route state (origin -> destination by callsign). Portable: the UI thread
// requests a lookup, a network task fulfils it, the UI reads the result.
#include <stddef.h>

void route_request(const char *callsign, double lat = 0.0, double lon = 0.0); // UI: want a route for this callsign
bool route_pending(char *callOut, size_t n, double *latOut = nullptr, double *lonOut = nullptr); // task: lookup needed?
void route_store(const char *callsign, const char *from, const char *to);  // task/sim: store result
bool route_get(const char *callsign, char *from, size_t fn, char *to, size_t tn); // UI: read result
