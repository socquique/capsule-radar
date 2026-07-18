#include "weather.h"
#include <mutex>
#include <string.h>
#include <stdio.h>

static std::mutex s_mutex;
static WeatherSnapshot s_snapshot = {};

void weather_store(const WeatherSnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot = snapshot;
}

bool weather_get(WeatherSnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    snapshot = s_snapshot;
    return snapshot.valid;
}

const char *weather_condition(int code) {
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code >= 85 && code <= 86) return "Snow showers";
    if (code >= 95) return "Thunderstorm";
    return "Unknown";
}

const char *weather_day_name(const char *isoDate) {
    static const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (!isoDate || strlen(isoDate) < 10) return "---";
    int y = 0, m = 0, d = 0;
    if (sscanf(isoDate, "%d-%d-%d", &y, &m, &d) != 3) return "---";
    if (m < 3) { m += 12; --y; }
    const int k = y % 100, j = y / 100;
    const int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return names[(h + 6) % 7];
}
