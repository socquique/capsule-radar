#pragma once
// Look up a flight's route by callsign and current position.
// Device-only (uses WiFi/HTTPS). Uses the adsb.lol tar1090 routeset endpoint.
#include <stddef.h>

bool route_fetch(const char *callsign, double lat, double lon, char *from, size_t fn, char *to, size_t tn);
