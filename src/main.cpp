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
#include "radar_view.h"
#include "ui.h"
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include <set>                       // audio: track which contacts are in range
#include <string>
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
static volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
static bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
static std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
static volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
static float                 g_requeryKm = 0.0f;
static volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)

// ---- networking task (core 0): fetch + parse, never touches the display ----
static void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    bool wasConnected = false;
    uint32_t lastPoll = 0;
    uint32_t lastFeedOk = millis();          // self-heal: time of last good (or no-WiFi) poll
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        if (conn && !wasConnected) {
            Serial.printf("[adsb] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
            configTzTime(TZ_STR, "pool.ntp.org", "time.nist.gov");  // local time (Spain)
            Serial.println("[web] config: http://capsuleradar.local/  (or the IP above)");
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        }
        wasConnected = conn;
        // self-heal: a long feed outage while WiFi is up usually means the internal heap
        // fragmented and the TLS handshake can't allocate -> reboot to recover (settings persist).
        if (!conn) lastFeedOk = millis();
        else if (millis() - lastFeedOk > 180000UL) {
            Serial.println("[adsb] feed stuck >180s with WiFi up -> restarting to recover");
            delay(100);
            ESP.restart();
        }
        if (g_requery) {                          // display range changed (double-tap zoom)
            g_adsb.begin(g_settings.homeLat, g_settings.homeLon, g_requeryKm);
            g_requery = false;
            lastPoll = 0;                         // poll immediately at the new radius
        }
        if (conn) {
            const uint32_t nowMs = millis();
            const uint32_t pollInterval = g_onBattery ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
            if (lastPoll == 0 || nowMs - lastPoll >= pollInterval) {  // aircraft feed
                lastPoll = nowMs;
                static int failCount = 0;
                if (g_adsb.poll(fresh)) {
                    Serial.printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    g_feedOk = true;
                    lastFeedOk = nowMs;
                    if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        g_aircraft = fresh;
                        g_acDirty = true;
                        xSemaphoreGive(g_ac_mutex);
                    }
                } else {
                    Serial.println("[adsb] poll failed (fetch or parse)");
                    if (++failCount >= 3) g_feedOk = false;   // several misses -> HUD warning
                }
            }
            // on-demand route lookup for the selected aircraft (checked often, fetched once)
            char wantCall[12];
            if (route_pending(wantCall, sizeof(wantCall))) {
                char from[40] = "", to[40] = "";
                if (route_cache_get(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);                       // NVS hit, no network
                    Serial.printf("[route] %s (cache): '%s' -> '%s'\n", wantCall, from, to);
                } else if (route_fetch(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);
                    route_cache_put(wantCall, from, to);                  // remember across reboots
                    Serial.printf("[route] %s (net): '%s' -> '%s'\n", wantCall, from, to);
                } else {
                    route_store(wantCall, from, to);   // empty -> don't refetch this session
                    Serial.printf("[route] %s: no route\n", wantCall);
                }
            }
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
    p.end();
}

// Best-effort military detection by ICAO hex prefix ("AE" = US military; extend as needed).
static bool isMilitaryHex(const char *hex) {
    if (!hex || !hex[0] || !hex[1]) return false;
    const char a = hex[0] | 0x20, b = hex[1] | 0x20;
    return a == 'a' && b == 'e';
}

// Ping when a new aircraft enters range (rate-limited) or for emergency/military (always).
static void checkAudioEvents() {
    if (!audio_present()) return;
    static std::set<std::string> seen;
    static bool first = true;
    static uint32_t lastNew = 0;
    std::set<std::string> now;
    for (const Aircraft &ac : g_snap) {
        const double d = geo::haversineKm(g_settings.homeLat, g_settings.homeLon, ac.lat, ac.lon);
        if (d > g_settings.rangeKm) continue;                 // in-range only
        const std::string hex = ac.hex.c_str();
        now.insert(hex);
        if (first || seen.count(hex)) continue;               // not new
        if (acIsEmergency(ac.squawk) || isMilitaryHex(ac.hex.c_str())) {
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
    g_requeryKm = constrain(km * 1.6f, 50.0f, 200.0f);
    g_requery = true;
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_set_range_km(km);
    ui_on_data_updated();
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
    setenv("TZ", TZ_STR, 1); tzset();   // restore local TZ for getLocalTime()
    return t;
}

// Seed the ESP system clock from the RTC so the clock/date are right before NTP.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { Serial.println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[rtc] system clock seeded from RTC");
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

static void handleRoot() {
    const int th = radar::theme();
    const int ranges[] = {10, 15, 25, 30, 50, 100, 150, 250};
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
    static char buf[6000];   // static (not on the 8 KB loop-task stack) to avoid overflow
    snprintf(buf, sizeof(buf),
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
        "<button>Save &amp; restart</button></form></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select></div>"
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
        "function d(v){fetch('/idle?v='+v+'&save=1')}</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, ropts.c_str(), topts.c_str(),
        g_brightnessDay, iopts.c_str(), g_volume, g_muted ? "checked" : "",
        g_settings.homeLat, g_settings.homeLon);
    g_web.send(200, "text/html", buf);
}

static void handleSave() {
    Preferences p;
    p.begin("capsuleradar", false);
    if (g_web.hasArg("lat"))   p.putDouble("homeLat", g_web.arg("lat").toDouble());
    if (g_web.hasArg("lon"))   p.putDouble("homeLon", g_web.arg("lon").toDouble());
    if (g_web.hasArg("range")) p.putFloat("rangeKm", g_web.arg("range").toFloat());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='4;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>CapsuleRadar-Setup</b> network to reconfigure.</body>");
    delay(400);
    g_wm.resetSettings();
    ESP.restart();
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
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nCapsule Radar boot");

    if (PIN_LCD_SCLK < 0 || PIN_I2C_SDA < 0) {
        Serial.println("[!] Pins in config.h are still -1. Copy them from the Waveshare demo.");
    }
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    loadSettings();

    // --- Display + LVGL (M0) ----------------------------------------------
    // CO5300 AMOLED over QSPI + LVGL draw buffers in PSRAM, then a hello screen.
    // The panel is powered from the always-on DC1 rail, so it lights without the
    // PMIC. Touch (CST9217 indev) + AXP2101 come in later milestones.
    if (!display::begin()) {
        Serial.println("[!] display::begin() failed — check QSPI pins / power.");
    }

    // restore the saved theme, then persist any future change
    {
        Preferences p;
        p.begin("capsuleradar", true);
        const int t = p.getInt("theme", THEME_PHOSPHOR);
        p.end();
        radar::setTheme(t);
    }
    radar::setThemeChangedCb(saveTheme);
    ui_set_range_cb(onRangeChange);              // on-screen zoom button
    ui_set_range_km(g_settings.rangeKm);         // show the loaded range

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init

    setenv("TZ", TZ_STR, 1); tzset();   // local time for display even before NTP
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
    }

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
        Serial.println("[wifi] connected");
    else
        Serial.println("[wifi] config portal open - join 'CapsuleRadar-Setup' to set WiFi; UI stays live");

    // --- OTA ---------------------------------------------------------------
    // ArduinoOTA is started from loop() once WiFi connects (see otaUp there).

    // --- ADS-B client + task ----------------------------------------------
    float queryKm = g_settings.rangeKm * 1.6f;          // query wider than the display range
    if (queryKm < 50.0f)  queryKm = 50.0f;
    if (queryKm > 200.0f) queryKm = 200.0f;
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
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
        },
        handleUpdateUpload);
    g_web.begin();

    Serial.println("setup done");
}

void loop() {
    display::loop();                // drive LVGL (render dirty areas + run timers)
    g_wm.process();                 // service the WiFi config portal (non-blocking)
    g_web.handleClient();           // serve the configuration web page

    // OTA: set up once WiFi is up, then service it every loop (flash over the air)
    static bool otaUp = false;
    if (!otaUp && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("capsuleradar");        // -> capsuleradar.local (registers mDNS)
        ArduinoOTA.begin();
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
        otaUp = true;
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
    }
    if (otaUp) ArduinoOTA.handle();

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    if (g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap = g_aircraft;
            g_acDirty = false;
            xSemaphoreGive(g_ac_mutex);
            radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
            ui_on_data_updated();              // refresh card/list/stats
            checkAudioEvents();                // ping new-in-range / emergency / military
        }
    }

    // periodic: HUD clock + wifi/battery indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
        Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                      (int)g_snap.size());
        char clk[8] = "--:--";
        struct tm ti;
        const bool haveTime = getLocalTime(&ti, 0);
        if (haveTime) {
            snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
            char date[20];
            strftime(date, sizeof(date), "%d %b %Y", &ti);   // e.g. "08 Jun 2026"
            ui_set_date(date);
        }
        ui_set_status(WiFi.status() == WL_CONNECTED, g_feedOk, clk);
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
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
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

    delay(5);
}
