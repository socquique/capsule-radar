// Capsule Radar — entry point / glue. SKELETON: TODOs mark what to implement.
// Order of work is in CLAUDE.md (milestones). Bring up the Waveshare demo first.
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include "config.h"
#include "aircraft.h"
#include "geo.h"
#include "adsb_client.h"
#include "route.h"
#include "route_client.h"
#include "photo.h"
#include "photo_client.h"
#include "weather.h"
#include "weather_client.h"
#include "net_guard.h"
#include "app_log.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include <string>
#include <string.h>
#include <WiFiManager.h>             // captive portal
#include <Preferences.h>            // NVS (persist theme/settings)
#include <time.h>                   // NTP/RTC clock + date
#include <WebServer.h>              // configuration web page
#include <ESPmDNS.h>                // http://capsuleradar.local
#include <ArduinoOTA.h>             // OTA firmware update over WiFi (PlatformIO/espota)
#include <Update.h>                 // browser OTA: self-flash an uploaded .bin
#include <esp_heap_caps.h>          // largest-free-block metric (heap health)

// ---- shared state ----
static std::vector<Aircraft> g_aircraft;      // latest snapshot
static SemaphoreHandle_t     g_ac_mutex;      // guards g_aircraft
static volatile bool         g_acDirty = false; // set when a new snapshot is ready
static AdsbClient            g_adsb;
static RadarSettings         g_settings;
static WiFiManager           g_wm;
static int                   g_brightnessDay = BRIGHTNESS_DEFAULT;   // user brightness (web/NVS)
static int                   g_volume = 60;                          // alert volume 0..100 (web/NVS)
static bool                  g_muted  = false;                       // mute alert pings
static uint32_t              g_idleDimMs = IDLE_DIM_MS;              // dim after this idle time (0 = never)
static bool                  g_animations = true;                    // sweep + center pulse animation on/off (web/NVS)
static int                   g_units = 0;                            // 0=Aviation 1=Metric 2=Imperial (web/NVS)
static bool                  g_showAirports = true;                  // airport markers on/off (web/NVS)
static bool                  g_showGroundAircraft = false;           // parked/taxiing aircraft on/off (web/NVS)
static char                  g_tzLabel[96] = "(UTC -06:00) Central Time - Chicago, Mexico City";
static char                  g_tzStr[80] = TZ_STR;
static volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
static bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
static std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
static uint8_t               g_snapStage = 0;                        // spread ADS-B UI work across loop turns
static volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
static float                 g_requeryKm = 0.0f;
static volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)
static volatile uint32_t     g_lastFeedOkMs = 0;                     // millis() of the last good poll (HUD staleness)
static volatile bool         g_centerSavePending = false;
static uint32_t              g_centerSaveAtMs = 0;
static volatile bool         g_mapPanActive = false;

static void logHeapLine(const char *tag) {
    app_log_printf("[%s] heap %u biggest %u psram %u\n",
                  tag ? tag : "mem",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)ESP.getFreePsram());
}

static void restartNow(const char *reason) {
    app_log_printf("[restart] %s\n", reason ? reason : "unspecified");
    Serial.flush();
    delay(150);
    ESP.restart();
}

static void pumpDisplay(uint32_t budgetMs = 4) {
    const uint32_t start = millis();
    const uint8_t maxPasses = (budgetMs >= 8) ? 8 : 4;
    uint8_t passes = 0;
    do {
        display::loop();
        yield();
        passes++;
    } while (passes < maxPasses && millis() - start < budgetMs);
}

// ---- networking task (core 0): fetch + parse, never touches the display ----
static void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    fresh.reserve(ADSB_MAX_AIRCRAFT);
    bool wasConnected = false;
    bool feedWasOk = true;
    uint32_t lastPoll = 0;
    uint32_t lastAdsbLog = 0;
    int lastAdsbCount = -1;
    uint32_t lastFeedOk = millis();          // self-heal: time of last good (or no-WiFi) poll
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        if (conn && !wasConnected) {
            app_log_printf("[wifi] connected: %s RSSI %d dBm\n",
                          WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            configTzTime(g_tzStr, "pool.ntp.org", "time.nist.gov");
            app_log_println("[web] config: http://capsuleradar.local/  (or the IP above)");
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        } else if (!conn && wasConnected) {
            app_log_println("[wifi] disconnected; pausing network fetches");
        }
        wasConnected = conn;
        // self-heal: a long feed outage while WiFi is up usually means the internal heap
        // fragmented and the TLS handshake can't allocate -> reboot to recover (settings persist).
        if (!conn) lastFeedOk = millis();
        else if (millis() - lastFeedOk > 180000UL) {
            restartNow("adsb feed stuck >180s with WiFi up");
        }
        if (g_mapPanActive || g_centerSavePending) {
            // Intentional pause: the center point is moving or waiting to be persisted.
            // Do not poll against stale coordinates; resume after the delayed save/requery.
            lastFeedOk = millis();
            g_lastFeedOkMs = lastFeedOk;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (g_requery) {                          // display range changed (double-tap zoom)
            g_adsb.begin(g_settings.homeLat, g_settings.homeLon, g_requeryKm);
            g_requery = false;
            lastPoll = 0;                         // poll immediately at the new radius
            app_log_printf("[adsb] query center %.5f, %.5f radius %.0f km\n",
                          g_settings.homeLat, g_settings.homeLon, g_requeryKm);
        }
        if (conn) {
            WeatherRequest wxReq;
            if (weather_pending(&wxReq)) {
                // Weather image refresh is HTTPS/TLS-heavy. Do it alone, then let
                // ADS-B resume on the next pass so the two feeds do not compete.
                lastFeedOk = millis();
                g_lastFeedOkMs = lastFeedOk;
                weather_fetch(wxReq);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            // The live aircraft feed is the primary job, so poll FIRST every cycle. That keeps
            // it refreshing even while the user taps around — a slow route/photo lookup (below)
            // can block this single network task, so it must never get ahead of the feed.
            const uint32_t nowMs = millis();
            const uint32_t pollInterval = g_onBattery ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
            if (lastPoll == 0 || nowMs - lastPoll >= pollInterval) {  // aircraft feed
                lastPoll = nowMs;
                static int failCount = 0;
                // poll() retries the primary once, then falls back to the secondary. A single
                // transient miss is absorbed by the failCount window.
                if (g_adsb.poll(fresh)) {
                    const int count = (int)fresh.size();
                    const bool recovered = !feedWasOk;
                    const bool countShift = lastAdsbCount < 0 || abs(count - lastAdsbCount) >= 5;
                    if (recovered || countShift || nowMs - lastAdsbLog >= 60000UL) {
                        app_log_printf("[adsb] ok (%s): %d aircraft%s\n",
                                      g_adsb.lastHostRole(), count, recovered ? " (recovered)" : "");
                        lastAdsbLog = nowMs;
                        lastAdsbCount = count;
                    }
                    feedWasOk = true;
                    failCount = 0;
                    g_feedOk = true;
                    lastFeedOk = nowMs;
                    g_lastFeedOkMs = nowMs;          // HUD: mark data as fresh
                    if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        g_aircraft.swap(fresh);   // O(1) handoff: no per-Aircraft copies under the lock
                        g_acDirty = true;
                        xSemaphoreGive(g_ac_mutex);
                    }
                } else {
                    if (tls_largest_internal_block() < TLS_INTERNAL_MIN_BYTES) {
                        // Memory pressure is local, not a bad feed. Back off without driving
                        // the self-heal reboot path; the HUD can still age to stale naturally.
                        lastFeedOk = nowMs;
                        failCount = 0;
                    } else {
                        failCount++;
                        if (failCount == 1 || failCount == 5) {
                            app_log_printf("[adsb] poll failed (%d consecutive): %s\n",
                                          failCount, g_adsb.lastError());
                        }
                        if (failCount >= 5) {
                            if (feedWasOk) app_log_println("[adsb] feed stale");
                            feedWasOk = false;
                            g_feedOk = false;   // sustained outage -> HUD warning
                        }
                    }
                }
            }
            // Then the on-demand lookups for the selected aircraft. Their timeouts are kept
            // short (see photo_client / route_client) so a slow photo server can't freeze the
            // feed for long; the next loop iteration polls again as soon as they return.
            char wantCall[12];
            double routeLat = 0.0, routeLon = 0.0;
            if (route_pending(wantCall, sizeof(wantCall), &routeLat, &routeLon)) {
                char from[40] = "", to[40] = "";
                if (route_fetch(wantCall, routeLat, routeLon, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);
                    app_log_printf("[route] %s (net): '%s' -> '%s'\n", wantCall, from, to);
                } else {
                    route_store(wantCall, from, to);   // empty -> don't refetch this session
                    app_log_printf("[route] %s: no route\n", wantCall);
                }
            }
            char wantHex[10];
            if (photo_pending(wantHex, sizeof(wantHex))) photo_fetch(wantHex);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void loadSettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
    g_settings.rangeKm = p.getFloat("rangeKm", RANGE_KM_DEFAULT);
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_muted            = p.getBool("mute", false);
    g_idleDimMs        = p.getUInt("idledim", IDLE_DIM_MS);
    g_units            = p.getInt("units", 0);
    char tzLabel[sizeof(g_tzLabel)] = "";
    char tzStr[sizeof(g_tzStr)] = "";
    if (p.getString("tzLabel", tzLabel, sizeof(tzLabel)) > 0) snprintf(g_tzLabel, sizeof(g_tzLabel), "%s", tzLabel);
    if (p.getString("tz", tzStr, sizeof(tzStr)) > 0) snprintf(g_tzStr, sizeof(g_tzStr), "%s", tzStr);
    p.end();
}

// Ping when a new aircraft enters range (rate-limited) or for emergency/military (always).
struct AudioHex {
    char hex[8];
};

static bool audioSeenContains(const std::vector<AudioHex> &seen, const char *hex) {
    for (const AudioHex &item : seen) {
        if (strcmp(item.hex, hex) == 0) return true;
    }
    return false;
}

static void checkAudioEvents() {
    if (!audio_present()) return;
    static std::vector<AudioHex> seen;
    static std::vector<AudioHex> now;
    static bool first = true;
    static uint32_t lastNew = 0;
    seen.reserve(ADSB_MAX_AIRCRAFT);
    now.clear();
    now.reserve(ADSB_MAX_AIRCRAFT);
    for (const Aircraft &ac : g_snap) {
        const double d = geo::haversineKm(g_settings.homeLat, g_settings.homeLon, ac.lat, ac.lon);
        if (d > g_settings.rangeKm) continue;                 // in-range only
        AudioHex key{};
        snprintf(key.hex, sizeof(key.hex), "%s", ac.hex);
        now.push_back(key);
        if (first || audioSeenContains(seen, ac.hex)) continue; // not new
        if (acIsEmergency(ac.squawk) || ac.military) {   // military flag comes from the feed (dbFlags)
            audio_play(AUDIO_ALERT);                          // urgent: always
        } else if (millis() - lastNew > 3000) {
            audio_play(AUDIO_NEW);                            // new contact: rate-limited
            lastNew = millis();
        }
    }
    seen.swap(now);
    first = false;
}

// Double-tap zoom: change the display range, persist it, and ask adsb_task to
// re-query at a matching radius (safely, on its own core). Re-render immediately.
static void onRangeChange(float km) {
    g_settings.rangeKm = km;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putFloat("rangeKm", km);
    p.end();
    g_requeryKm = constrain(km * ADSB_QUERY_FACTOR, ADSB_QUERY_MIN_KM, ADSB_QUERY_MAX_KM);
    g_requery = true;
    app_log_printf("[settings] range %.0f km; ADS-B query %.0f km\n", km, g_requeryKm);
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_set_range_km(km);
    ui_on_data_updated();
}

static void onWeatherRefresh() {
    app_log_printf("[weather] refresh requested at %.5f, %.5f\n", g_settings.homeLat, g_settings.homeLon);
    weather_request(g_settings.homeLat, g_settings.homeLon, g_settings.rangeKm);
}

static void requeryAdsbForCurrentRange() {
    g_requeryKm = constrain(g_settings.rangeKm * ADSB_QUERY_FACTOR, ADSB_QUERY_MIN_KM, ADSB_QUERY_MAX_KM);
    g_requery = true;
}

static void onMapPan(int dx, int dy, bool done) {
    static bool dirty = false;
    static int visualDx = 0;
    static int visualDy = 0;
    if (dx || dy) {
        g_mapPanActive = true;
        g_centerSavePending = false;   // a new pan supersedes any delayed save from the previous one
        const double metersPerPx = (double)MAP_PAN_GAIN * (double)g_settings.rangeKm * 1000.0 / (double)RADAR_R_OUTER_PX;
        const double latRad = g_settings.homeLat * 0.017453292519943295;
        const double mPerDegLat = 111320.0;
        const double mPerDegLon = mPerDegLat * cos(latRad);

        // Drag moves the map under your fingers: right = center moves west, down = center moves north.
        const double eastM = -(double)dx * metersPerPx;
        const double northM = (double)dy * metersPerPx;
        g_settings.homeLat += northM / mPerDegLat;
        if (fabs(mPerDegLon) > 1.0) g_settings.homeLon += eastM / mPerDegLon;
        if (g_settings.homeLat > 89.9) g_settings.homeLat = 89.9;
        if (g_settings.homeLat < -89.9) g_settings.homeLat = -89.9;
        while (g_settings.homeLon > 180.0) g_settings.homeLon -= 360.0;
        while (g_settings.homeLon < -180.0) g_settings.homeLon += 360.0;

        visualDx += (int)lround((double)dx * (double)MAP_PAN_GAIN);
        visualDy += (int)lround((double)dy * (double)MAP_PAN_GAIN);
        radar::setMapPanOffset(visualDx, visualDy);
        dirty = true;
    }
    if (done && dirty) {
        radar::update(g_snap, g_settings);   // one full reproject after the fast visual drag
        visualDx = 0;
        visualDy = 0;
        g_centerSavePending = true;
        g_centerSaveAtMs = millis() + 1500UL;
        dirty = false;
    }
    if (done) g_mapPanActive = false;
}

static void savePendingCenterIfDue() {
    if (!g_centerSavePending || millis() < g_centerSaveAtMs) return;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putDouble("homeLat", g_settings.homeLat);
    p.putDouble("homeLon", g_settings.homeLon);
    p.end();
    requeryAdsbForCurrentRange();
    g_centerSavePending = false;
    app_log_printf("[map] center saved %.5f, %.5f; requery %.0f km\n",
                  g_settings.homeLat, g_settings.homeLon, g_requeryKm);
}

// Persist the visual theme in NVS (called when the user long-presses to switch).
static void saveTheme(int t) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("theme", t);
    p.end();
}

// Convert a UTC broken-down time to time_t (mktime assumes local TZ, so flip to UTC0).
static time_t utc_to_time(struct tm *utc) {
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(utc);
    setenv("TZ", g_tzStr, 1); tzset();   // restore local TZ for getLocalTime()
    return t;
}

// Seed the ESP system clock from the RTC so the clock/date are right before NTP.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { app_log_println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    app_log_println("[rtc] system clock seeded from RTC");
}

// Brightness combines idle auto-dim and face-down sleep (sleep wins -> screen off).
static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE  < b) b = BRIGHTNESS_IDLE;   // idle only dims down
    if (g_asleep) b = 0;                                         // face-down -> screen off
    display::setBrightness(b);
}

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

struct TzPreset { const char *group; const char *label; const char *tz; };
static const TzPreset TZ_PRESETS[] = {
    {"North America", "(UTC -10:00) Hawaii Time - Honolulu", "HST10"},
    {"North America", "(UTC -09:00) Alaska Time - Anchorage", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"North America", "(UTC -08:00) Pacific Time - Los Angeles, Vancouver", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"North America", "(UTC -07:00) Mountain Time - Denver, Calgary", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"North America", "(UTC -07:00) Arizona Time - Phoenix", "MST7"},
    {"North America", "(UTC -06:00) Central Time - Chicago, Mexico City", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"North America", "(UTC -05:00) Eastern Time - New York, Toronto", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"North America", "(UTC -04:00) Atlantic Time - Halifax, San Juan", "AST4ADT,M3.2.0/2,M11.1.0/2"},
    {"North America", "(UTC -03:30) Newfoundland Time - St Johns", "NST3:30NDT,M3.2.0/2,M11.1.0/2"},
    {"South America", "(UTC -05:00) Colombia/Peru Time - Bogota, Lima", "COT5"},
    {"South America", "(UTC -04:00) Bolivia/Venezuela Time - La Paz, Caracas", "BOT4"},
    {"South America", "(UTC -03:00) Argentina/Uruguay Time - Buenos Aires, Montevideo", "ART3"},
    {"South America", "(UTC -03:00) Brazil Time - Sao Paulo, Rio de Janeiro", "BRT3"},
    {"Atlantic", "(UTC -01:00) Azores Time - Ponta Delgada", "AZOT1AZOST,M3.5.0/0,M10.5.0/1"},
    {"Europe", "(UTC +00:00) Greenwich Mean Time - Reykjavik", "GMT0"},
    {"Europe", "(UTC +00:00) UK/Ireland Time - London, Dublin", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe", "(UTC +01:00) Central Europe - Paris, Berlin, Madrid, Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe", "(UTC +02:00) Eastern Europe - Athens, Helsinki, Cairo", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe", "(UTC +03:00) Turkey Time - Istanbul", "TRT-3"},
    {"Europe", "(UTC +03:00) Moscow Time - Moscow", "MSK-3"},
    {"Africa", "(UTC +00:00) West Africa - Dakar, Accra", "GMT0"},
    {"Africa", "(UTC +01:00) West Central Africa - Lagos, Algiers", "WAT-1"},
    {"Africa", "(UTC +02:00) South Africa/Central Africa - Johannesburg, Harare", "SAST-2"},
    {"Africa", "(UTC +03:00) East Africa - Nairobi, Addis Ababa", "EAT-3"},
    {"Middle East", "(UTC +02:00) Israel Time - Tel Aviv, Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Middle East", "(UTC +03:00) Arabia Time - Riyadh, Baghdad", "AST-3"},
    {"Middle East", "(UTC +03:30) Iran Time - Tehran", "IRST-3:30"},
    {"Middle East", "(UTC +04:00) Gulf Time - Dubai, Abu Dhabi", "GST-4"},
    {"Asia", "(UTC +04:30) Afghanistan Time - Kabul", "AFT-4:30"},
    {"Asia", "(UTC +05:00) Pakistan Time - Karachi, Islamabad", "PKT-5"},
    {"Asia", "(UTC +05:30) India Time - Delhi, Mumbai, Kolkata", "IST-5:30"},
    {"Asia", "(UTC +05:45) Nepal Time - Kathmandu", "NPT-5:45"},
    {"Asia", "(UTC +06:00) Bangladesh Time - Dhaka", "BST-6"},
    {"Asia", "(UTC +06:30) Myanmar Time - Yangon", "MMT-6:30"},
    {"Asia", "(UTC +07:00) Indochina Time - Bangkok, Jakarta, Ho Chi Minh City", "ICT-7"},
    {"Asia", "(UTC +08:00) China Time - Beijing, Shanghai, Hong Kong", "CST-8"},
    {"Asia", "(UTC +08:00) Singapore/Malaysia Time - Singapore, Kuala Lumpur", "SGT-8"},
    {"Asia", "(UTC +08:00) Western Australia - Perth", "AWST-8"},
    {"Asia", "(UTC +09:00) Japan/Korea Time - Tokyo, Seoul", "JST-9"},
    {"Australia and Pacific", "(UTC +09:30) Northern Territory - Darwin", "ACST-9:30"},
    {"Australia and Pacific", "(UTC +09:30) South Australia - Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia and Pacific", "(UTC +10:00) Queensland - Brisbane", "AEST-10"},
    {"Australia and Pacific", "(UTC +10:00) Eastern Australia - Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia and Pacific", "(UTC +12:00) New Zealand - Auckland, Wellington", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Australia and Pacific", "(UTC +12:45) Chatham Islands - Chatham", "CHAST-12:45CHADT,M9.5.0/2:45,M4.1.0/3:45"},
    {"Australia and Pacific", "(UTC +13:00) Tonga/Samoa Time - Nukualofa, Apia", "TOT-13"},
    {"UTC offsets", "(UTC -12:00) Fixed Offset", "UTC12"},
    {"UTC offsets", "(UTC -11:00) Fixed Offset", "UTC11"},
    {"UTC offsets", "(UTC -10:00) Fixed Offset", "UTC10"},
    {"UTC offsets", "(UTC -09:30) Fixed Offset", "UTC9:30"},
    {"UTC offsets", "(UTC -09:00) Fixed Offset", "UTC9"},
    {"UTC offsets", "(UTC -08:00) Fixed Offset", "UTC8"},
    {"UTC offsets", "(UTC -07:00) Fixed Offset", "UTC7"},
    {"UTC offsets", "(UTC -06:00) Fixed Offset", "UTC6"},
    {"UTC offsets", "(UTC -05:00) Fixed Offset", "UTC5"},
    {"UTC offsets", "(UTC -04:00) Fixed Offset", "UTC4"},
    {"UTC offsets", "(UTC -03:30) Fixed Offset", "UTC3:30"},
    {"UTC offsets", "(UTC -03:00) Fixed Offset", "UTC3"},
    {"UTC offsets", "(UTC -02:00) Fixed Offset", "UTC2"},
    {"UTC offsets", "(UTC -01:00) Fixed Offset", "UTC1"},
    {"UTC offsets", "(UTC +00:00) Coordinated Universal Time", "UTC0"},
    {"UTC offsets", "(UTC +01:00) Fixed Offset", "UTC-1"},
    {"UTC offsets", "(UTC +02:00) Fixed Offset", "UTC-2"},
    {"UTC offsets", "(UTC +03:00) Fixed Offset", "UTC-3"},
    {"UTC offsets", "(UTC +03:30) Fixed Offset", "UTC-3:30"},
    {"UTC offsets", "(UTC +04:00) Fixed Offset", "UTC-4"},
    {"UTC offsets", "(UTC +04:30) Fixed Offset", "UTC-4:30"},
    {"UTC offsets", "(UTC +05:00) Fixed Offset", "UTC-5"},
    {"UTC offsets", "(UTC +05:30) Fixed Offset", "UTC-5:30"},
    {"UTC offsets", "(UTC +05:45) Fixed Offset", "UTC-5:45"},
    {"UTC offsets", "(UTC +06:00) Fixed Offset", "UTC-6"},
    {"UTC offsets", "(UTC +06:30) Fixed Offset", "UTC-6:30"},
    {"UTC offsets", "(UTC +07:00) Fixed Offset", "UTC-7"},
    {"UTC offsets", "(UTC +08:00) Fixed Offset", "UTC-8"},
    {"UTC offsets", "(UTC +08:45) Fixed Offset", "UTC-8:45"},
    {"UTC offsets", "(UTC +09:00) Fixed Offset", "UTC-9"},
    {"UTC offsets", "(UTC +09:30) Fixed Offset", "UTC-9:30"},
    {"UTC offsets", "(UTC +10:00) Fixed Offset", "UTC-10"},
    {"UTC offsets", "(UTC +10:30) Fixed Offset", "UTC-10:30"},
    {"UTC offsets", "(UTC +11:00) Fixed Offset", "UTC-11"},
    {"UTC offsets", "(UTC +12:00) Fixed Offset", "UTC-12"},
    {"UTC offsets", "(UTC +12:45) Fixed Offset", "UTC-12:45"},
    {"UTC offsets", "(UTC +13:00) Fixed Offset", "UTC-13"},
    {"UTC offsets", "(UTC +14:00) Fixed Offset", "UTC-14"},
};

static void handleRoot() {
    const int th = radar::theme();
    const int ranges[] = {5, 10, 15, 25, 30, 50, 100, 150, 250};
    String ropts;
    for (int r : ranges) {
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%d km</option>",
                 r, (r == (int)(g_settings.rangeKm + 0.5f)) ? " selected" : "", r);
        ropts += o;
    }
    const char *tnames[] = {"Phosphor", "Dragon (Capsule Corp)", "Amber CRT", "Military"};
    String topts;
    for (int i = 0; i < 4; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", tnames[i]);
        topts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if (sV < 60) snprintf(lbl, sizeof(lbl), "%d s", sV);
        else         snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl);
        iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    const char *unames[] = {"Aviation (ft, kt, km)", "Metric (m, km/h, km)", "Imperial (ft, mph, mi)"};
    String uopts;
    for (int i = 0; i < 3; ++i) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_units ? " selected" : "", unames[i]);
        uopts += o;
    }
    String tzopts = "<option value=''>Keep current</option>";
    const char *group = "";
    const char *currentTzLabel = g_tzLabel;
    for (const TzPreset &z : TZ_PRESETS) {
        if (strcmp(group, z.group) != 0) {
            if (group[0]) tzopts += "</optgroup>";
            group = z.group;
            tzopts += "<optgroup label='"; tzopts += group; tzopts += "'>";
        }
        tzopts += "<option value='"; tzopts += z.label; tzopts += "|"; tzopts += z.tz; tzopts += "'";
        if (strcmp(z.tz, g_tzStr) == 0) {
            tzopts += " selected";
            currentTzLabel = z.label;
        }
        tzopts += ">";
        tzopts += z.label; tzopts += "</option>";
    }
    if (group[0]) tzopts += "</optgroup>";

    const size_t bufN = 24000;
    char *buf = (char *)heap_caps_malloc(bufN, MALLOC_CAP_SPIRAM);
    if (!buf) {
        g_web.send(503, "text/plain", "Not enough memory to render portal");
        return;
    }
    snprintf(buf, bufN,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "input:focus,select:focus{outline:none;border-color:#1dff86;box-shadow:0 0 0 2px rgba(29,255,134,.18)}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>Capsule Radar</h1><p class=sub>Live ADS-B radar &middot; configuration</p></div></div>"
        "<div class=card><div class=t>Location &amp; range</div><form method=POST action=/save>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        "<label>Display range (km)</label><select name=range>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<button>Save &amp; apply</button></form></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='an(this.checked)'>Animations</label>"
        "<label><input type=checkbox class=ck %s onchange='ap(this.checked)'>Show airports</label>"
        "<label><input type=checkbox class=ck %s onchange='ga(this.checked)'>Show ground aircraft</label>"
        "<label>Units</label><select onchange='u(this.value)'>%s</select>"
        "<form method=POST action=/save>"
        "<label>Timezone</label><select name=tzPick>%s</select>"
        "<p style='color:#9affc8;font-size:13px;margin:8px 0 0'>Current: %s</p>"
        "<button>Save timezone</button></form></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input type=range min=0 max=100 value='%d' oninput='v(this.value,0)' onchange='v(this.value,1)'>"
        "<label><input type=checkbox class=ck %s onchange='m(this.checked)'>Mute alerts</label>"
        "<button type=button class=sec onclick='t()'>Test ping</button></div>"
        "<div class=card><div class=t>Network</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        "<p class=ft>Reach me at <code>capsuleradar.local</code> &middot; <a href=/update style='color:#9affc8'>Firmware update</a></p>"
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function an(c){fetch('/animations?v='+(c?1:0)+'&save=1')}"
        "function ap(c){fetch('/airports?v='+(c?1:0)+'&save=1')}"
        "function ga(c){fetch('/ground?v='+(c?1:0)+'&save=1')}"
        "function u(v){fetch('/units?v='+v+'&save=1')}"
        "</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, ropts.c_str(), topts.c_str(),
        g_brightnessDay, iopts.c_str(), g_animations ? "checked" : "",
        g_showAirports ? "checked" : "", g_showGroundAircraft ? "checked" : "", uopts.c_str(),
        tzopts.c_str(), currentTzLabel,
        g_volume, g_muted ? "checked" : "",
        g_settings.homeLat, g_settings.homeLon);
    g_web.send(200, "text/html", buf);
    heap_caps_free(buf);
}

static void handleSave() {
    bool mapChanged = false;
    bool rangeChanged = false;
    bool themeChanged = false;
    bool tzChanged = false;
    double newLat = g_settings.homeLat;
    double newLon = g_settings.homeLon;
    float newRange = g_settings.rangeKm;
    int newTheme = radar::theme();
    const char *newTzLabel = nullptr;
    const char *newTz = nullptr;

    Preferences p;
    p.begin("capsuleradar", false);
    // Reject out-of-range coordinates so a typo can't leave the radar unusable.
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) {
            newLat = lat;
            p.putDouble("homeLat", lat);
            mapChanged = true;
        }
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) {
            newLon = lon;
            p.putDouble("homeLon", lon);
            mapChanged = true;
        }
    }
    if (g_web.hasArg("range")) {
        const float range = g_web.arg("range").toFloat();
        if (range > 0.0f) {
            newRange = range;
            p.putFloat("rangeKm", range);
            rangeChanged = true;
        }
    }
    if (g_web.hasArg("theme")) {
        const int theme = g_web.arg("theme").toInt();
        if (theme >= 0 && theme < THEME_COUNT) {
            newTheme = theme;
            p.putInt("theme", theme);
            themeChanged = true;
        }
    }
    if (g_web.hasArg("tzPick")) {
        String pick = g_web.arg("tzPick");
        pick.trim();
        const int split = pick.indexOf('|');
        if (split > 0) {
            String label = pick.substring(0, split);
            String tz = pick.substring(split + 1);
            label.trim();
            tz.trim();
            for (const TzPreset &z : TZ_PRESETS) {
                if (label == z.label && tz == z.tz) {
                    p.putString("tzLabel", z.label);
                    p.putString("tz", z.tz);
                    newTzLabel = z.label;
                    newTz = z.tz;
                    tzChanged = true;
                    break;
                }
            }
        }
    }
    p.end();

    if (mapChanged || rangeChanged) {
        g_settings.homeLat = newLat;
        g_settings.homeLon = newLon;
        g_settings.rangeKm = newRange;
        radar::update(g_snap, g_settings);
        requeryAdsbForCurrentRange();
        ui_set_range_km(g_settings.rangeKm);
        ui_on_data_updated();
        app_log_printf("[settings] portal map/range: %.5f, %.5f range %.0f km query %.0f km\n",
                      g_settings.homeLat, g_settings.homeLon, g_settings.rangeKm, g_requeryKm);
    }
    if (themeChanged) {
        radar::setTheme(newTheme);
        app_log_printf("[settings] theme %d\n", newTheme);
    }
    if (tzChanged && newTzLabel && newTz) {
        snprintf(g_tzLabel, sizeof(g_tzLabel), "%s", newTzLabel);
        snprintf(g_tzStr, sizeof(g_tzStr), "%s", newTz);
        setenv("TZ", g_tzStr, 1);
        tzset();
        if (WiFi.status() == WL_CONNECTED) configTzTime(g_tzStr, "pool.ntp.org", "time.nist.gov");
        app_log_println("[settings] timezone updated");
    }

    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='1;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Applied.</body>");
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>CapsuleRadar-Setup</b> network to reconfigure.</body>");
    delay(400);
    g_wm.resetSettings();
    restartNow("wifi settings reset from web portal");
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("bright", g_brightnessDay);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.putBool("mute", g_muted);
        p.end();
    }
    if (g_web.hasArg("test")) {
        if (g_web.arg("test").toInt() == 2) audio_selftest();   // long tone, ignores mute
        else audio_play(AUDIO_NEW);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {   // idle auto-dim timeout (seconds; 0 = never)
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putUInt("idledim", g_idleDimMs);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleUnits() {   // measurement units preset (live re-render)
    if (g_web.hasArg("v")) {
        g_units = constrain((int)g_web.arg("v").toInt(), 0, 2);
        ui_set_units(g_units);
        ui_set_range_km(g_settings.rangeKm);   // refresh the zoom-button label
        ui_on_data_updated();                  // re-render card/list/stats in the new units
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("units", g_units);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAnimations() {   // show/hide radar animations: sweep + center pulse
    if (g_web.hasArg("v")) {
        g_animations = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_animations);          // loop()/core 1: safe to touch LVGL
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("sweep", g_animations);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAirports() {   // show/hide airport markers (live)
    if (g_web.hasArg("v")) {
        g_showAirports = g_web.arg("v").toInt() != 0;
        radar::setAirportsEnabled(g_showAirports);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("airports", g_showAirports);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGroundAircraft() {   // show/hide parked/taxiing aircraft (live)
    if (g_web.hasArg("v")) {
        g_showGroundAircraft = g_web.arg("v").toInt() != 0;
        radar::setGroundAircraftEnabled(g_showGroundAircraft);
        ui_on_data_updated();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("groundac", g_showGroundAircraft);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static void handleUpdatePage() {
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg{margin-top:10px;color:#9affc8;font-size:13px}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1><div class=card>"
        "<p>Upload the <b>app firmware</b> <code>CapsuleRadar-ota.bin</code> from the GitHub release. "
        "Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<p style='text-align:center;margin-top:14px'><a href=/>&larr; Back to settings</a></p>"
        "<script>function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        app_log_printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(app_log_stream());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(app_log_stream());
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) app_log_printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(app_log_stream());
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    app_log_println("Capsule Radar boot");

    if (PIN_LCD_SCLK < 0 || PIN_I2C_SDA < 0) {
        app_log_println("[!] Pins in config.h are still -1. Copy them from the Waveshare demo.");
    }
    app_log_printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    loadSettings();
    g_aircraft.reserve(ADSB_MAX_AIRCRAFT);
    g_snap.reserve(ADSB_MAX_AIRCRAFT);
    app_log_printf("[config] center %.5f, %.5f range %.0f km units %d airports %s ground %s\n",
                  g_settings.homeLat, g_settings.homeLon, g_settings.rangeKm, g_units,
                  g_showAirports ? "on" : "off", g_showGroundAircraft ? "on" : "off");
    app_log_printf("[config] timezone %s\n", g_tzLabel);
    logHeapLine("boot");

    // --- Display + LVGL (M0) ----------------------------------------------
    // CO5300 AMOLED over QSPI + LVGL draw buffers in PSRAM, then a hello screen.
    // The panel is powered from the always-on DC1 rail, so it lights without the
    // PMIC. Touch (CST9217 indev) + AXP2101 come in later milestones.
    const bool displayOk = display::begin();
    if (!displayOk) {
        app_log_println("[!] display::begin() failed — check QSPI pins / power.");
    }
    auto bootPump = [&]() {
        if (!displayOk) return;
        for (int i = 0; i < 3; ++i) {
            display::loop();
            delay(5);
        }
    };
    bootPump();

    // restore the saved theme, then persist any future change
    {
        Preferences p;
        p.begin("capsuleradar", true);
        const int t = p.getInt("theme", THEME_PHOSPHOR);
        g_animations = p.getBool("sweep", true);
        g_showAirports = p.getBool("airports", true);
        g_showGroundAircraft = p.getBool("groundac", false);
        p.end();
        radar::setTheme(t);
        radar::setSweepEnabled(g_animations);
        radar::setAirportsEnabled(g_showAirports);
        radar::setGroundAircraftEnabled(g_showGroundAircraft);
    }
    radar::setThemeChangedCb(saveTheme);
    ui_set_range_cb(onRangeChange);              // on-screen zoom button
    ui_set_pan_cb(onMapPan);                     // two-finger drag -> move/save center point
    ui_set_weather_refresh_cb(onWeatherRefresh); // weather page manual refresh
    ui_set_units(g_units);                       // apply saved unit preset
    ui_set_range_km(g_settings.rangeKm);         // show the loaded range
    radar::update(g_snap, g_settings);           // draw static map/airports before the first ADS-B poll
    bootPump();

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init
    bootPump();

    setenv("TZ", g_tzStr, 1); tzset();   // local time for display even before NTP
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
    }
    bootPump();

    // --- Radar UI ----------------------------------------------------------
    // radar::init() runs inside display::begin() (LVGL must be up first).

    // --- WiFi (captive portal, non-blocking) ------------------------------
    // First boot opens the "CapsuleRadar-Setup" AP to enter WiFi creds. Non-blocking
    // so the radar keeps animating while you configure WiFi from your phone.
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("Capsule Radar");
    // light phosphor-green theme for the captive portal (small CSS, injected into <head>)
    g_wm.setCustomHeadElement(
        "<style>"
        "body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}"
        "button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;"
        "border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}.q{filter:hue-rotate(90deg)}"
        "</style>");
    if (g_wm.autoConnect("CapsuleRadar-Setup"))
        app_log_println("[wifi] connected");
    else
        app_log_println("[wifi] config portal open - join 'CapsuleRadar-Setup' to set WiFi; UI stays live");
    bootPump();

    // --- OTA ---------------------------------------------------------------
    // ArduinoOTA is started from loop() once WiFi connects (see otaUp there).

    // --- ADS-B client + task ----------------------------------------------
    float queryKm = constrain(g_settings.rangeKm * ADSB_QUERY_FACTOR, ADSB_QUERY_MIN_KM, ADSB_QUERY_MAX_KM);
    g_adsb.begin(g_settings.homeLat, g_settings.homeLon, queryKm);
    g_ac_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(adsb_task, "adsb", 16384, nullptr, 1, nullptr, 0);  // TLS needs a big stack

    // configuration web page (http://capsuleradar.local/)
    g_web.on("/", handleRoot);
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/wifi", HTTP_POST, handleWifi);
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/idle", handleIdle);
    g_web.on("/animations", handleAnimations);
    g_web.on("/sweep", handleAnimations);      // backwards-compatible with any stale portal page
    g_web.on("/airports", handleAirports);
    g_web.on("/ground", handleGroundAircraft);
    g_web.on("/units", handleUnits);
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) restartNow("browser OTA update complete");
        },
        handleUpdateUpload);
    g_web.begin();

    ui_splash_hide();
    app_log_println("setup done");
}

void loop() {
    pumpDisplay();                  // drive LVGL (render dirty areas + run timers)
    if (g_mapPanActive) {           // keep two-finger map panning feeling responsive
        pumpDisplay(8);
    }
    g_wm.process();                 // service the WiFi config portal (non-blocking)
    g_web.handleClient();           // serve the configuration web page
    savePendingCenterIfDue();       // debounce NVS writes after two-finger map panning
    if (weather_take_dirty()) ui_weather_updated();

    // OTA: set up once WiFi is up, then service it every loop (flash over the air)
    static bool otaUp = false;
    if (!otaUp && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("capsuleradar");        // -> capsuleradar.local (registers mDNS)
        ArduinoOTA.begin();
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
        otaUp = true;
        app_log_println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
    }
    if (otaUp) ArduinoOTA.handle();
    pumpDisplay(2);

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    if (g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap.swap(g_aircraft);   // O(1) handoff under the lock; render on g_snap outside it.
            g_acDirty = false;         // g_aircraft now holds the previous snapshot (overwritten next poll)
            xSemaphoreGive(g_ac_mutex);
            g_snapStage = 1;
        }
    }
    if (g_snapStage == 1) {
        radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
        g_snapStage = 2;
        pumpDisplay(1);
    } else if (g_snapStage == 2) {
        ui_on_data_updated();              // refresh card/list/stats
        g_snapStage = 3;
        pumpDisplay(1);
    } else if (g_snapStage == 3) {
        checkAudioEvents();                // ping new-in-range / emergency / military
        g_snapStage = 0;
    }

    // periodic: HUD clock + wifi/battery indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
#if DEBUG_MEM
        static uint32_t lastFrames = 0;
        const uint32_t fr = display_frames();
        const unsigned fps = (fr - lastFrames) / 5;
        lastFrames = fr;
        app_log_printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d | fps %u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                      (int)g_snap.size(), fps);
#endif
        char clk[8] = "--:--";
        struct tm ti;
        const bool haveTime = getLocalTime(&ti, 0);
        if (haveTime) {
            snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
            char date[20];
            strftime(date, sizeof(date), "%d %b %Y", &ti);   // e.g. "08 Jun 2026"
            ui_set_date(date);
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // "fresh" = we got aircraft data recently. Catches a stalled feed (weak WiFi dropping
        // polls intermittently) that never trips the consecutive-fail counter -> aircraft
        // freeze but the icon would otherwise stay white.
        const bool feedFresh = wifiUp && (millis() - g_lastFeedOkMs < 18000UL);
        ui_set_status(wifiUp, feedFresh, rssi, clk);
        char net[80];
        if (WiFi.status() == WL_CONNECTED)
            snprintf(net, sizeof(net), "Configure at\ncapsuleradar.local\n%s", WiFi.localIP().toString().c_str());
        else
            snprintf(net, sizeof(net), "WiFi setup:\njoin CapsuleRadar-Setup");
        ui_set_netinfo(net);
        const bool bpresent = battery_present();
        ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !battery_charging();
        // once NTP has a real fix, persist it to the RTC (core 1 only)
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr);
            struct tm utc;
            gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; app_log_println("[rtc] saved NTP time"); }
        }
    }

    // face-down -> screen off (IMU); flip face-up to wake
    static uint32_t lastImu = 0;
    static int fdCount = 0;
    if (millis() - lastImu > 400) {
        lastImu = millis();
        if (imu_facedown()) { if (fdCount < 8) fdCount++; }
        else fdCount = 0;
        const bool sleep = (fdCount >= 4);   // ~1.6 s face-down
        const bool idle  = g_idleDimMs > 0 && display::inactiveMs() > g_idleDimMs;
        if (sleep != g_asleep || idle != g_idle) {
            g_asleep = sleep;
            g_idle = idle;
            applyBrightness();
        }
    }

    delay(1);
}
