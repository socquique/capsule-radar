#pragma once

#include <stddef.h>

// Portable weather snapshot shared by the network task and LVGL UI.
// Temperatures are Celsius, wind is km/h and precipitation is millimetres;
// the UI converts these according to the selected unit preset.
struct WeatherDay {
    char date[11];
    int code;
    float tempMinC;
    float tempMaxC;
    int rainChance;
};

struct WeatherSnapshot {
    bool valid;
    char updated[6];
    int code;
    float tempC;
    float feelsC;
    int humidity;
    float windKmh;
    int windDeg;
    WeatherDay days[4];
    int dayCount;
};

void weather_store(const WeatherSnapshot &snapshot);
bool weather_get(WeatherSnapshot &snapshot);
const char *weather_condition(int code);
const char *weather_day_name(const char *isoDate);
