#pragma once

#include "weather.h"

// Fetch current conditions and a four-day forecast from Open-Meteo.
bool weather_fetch(double lat, double lon, WeatherSnapshot &out);
