#include "weather_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

bool weather_fetch(double lat, double lon, WeatherSnapshot &out) {
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&forecast_days=4&timezone=auto", lat, lon);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3500);
    http.setTimeout(7000);
    if (!http.begin(client, url)) {
        Serial.println("[weather] HTTP begin failed");
        return false;
    }
    http.addHeader("User-Agent", ADSB_USER_AGENT);

    const int status = http.GET();
    if (status != 200) {
        char tls[128] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        Serial.printf("[weather] HTTP %d: %s tls=%d '%s' heap=%u largest=%u psram=%u\n", status,
                      status < 0 ? http.errorToString(status).c_str() : "unexpected response",
                      tlsCode, tls, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        http.end();
        return false;
    }

    // Open-Meteo replies with Transfer-Encoding: chunked. HTTPClient::getString()
    // removes the chunk framing; parsing getStream() directly makes ArduinoJson
    // see the hexadecimal chunk size first and report InvalidInput.
    String payload = http.getString();
    http.end();
    if (payload.length() == 0) {
        Serial.println("[weather] empty response body");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[weather] JSON error: %s (%u bytes, starts '%.24s')\n",
                      err.c_str(), (unsigned)payload.length(), payload.c_str());
        return false;
    }

    WeatherSnapshot next = {};
    JsonObjectConst current = doc["current"].as<JsonObjectConst>();
    JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
    if (current.isNull() || daily.isNull()) {
        Serial.println("[weather] response missing current/daily data");
        return false;
    }

    const char *stamp = current["time"] | "";
    const char *clock = strchr(stamp, 'T');
    snprintf(next.updated, sizeof(next.updated), "%.5s", clock ? clock + 1 : "--:--");
    next.code = current["weather_code"] | -1;
    next.tempC = current["temperature_2m"] | 0.0f;
    next.feelsC = current["apparent_temperature"] | next.tempC;
    next.humidity = current["relative_humidity_2m"] | 0;
    next.windKmh = current["wind_speed_10m"] | 0.0f;
    next.windDeg = current["wind_direction_10m"] | 0;

    JsonArrayConst dates = daily["time"].as<JsonArrayConst>();
    JsonArrayConst codes = daily["weather_code"].as<JsonArrayConst>();
    JsonArrayConst highs = daily["temperature_2m_max"].as<JsonArrayConst>();
    JsonArrayConst lows = daily["temperature_2m_min"].as<JsonArrayConst>();
    JsonArrayConst rain = daily["precipitation_probability_max"].as<JsonArrayConst>();
    const size_t count = dates.size() < 4 ? dates.size() : 4;
    for (size_t i = 0; i < count; ++i) {
        snprintf(next.days[i].date, sizeof(next.days[i].date), "%s", dates[i] | "");
        next.days[i].code = codes[i] | -1;
        next.days[i].tempMaxC = highs[i] | 0.0f;
        next.days[i].tempMinC = lows[i] | 0.0f;
        next.days[i].rainChance = rain[i] | 0;
    }
    next.dayCount = (int)count;
    next.valid = true;
    out = next;
    return true;
}
