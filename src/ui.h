#pragma once
// M3 UI: swipeable views (radar / list / stats / weather) + tap-to-inspect detail card.
// Pure LVGL, portable (device + SDL simulator). Builds on top of radar_view.
void ui_create(void);            // build the whole UI on the active screen
void ui_on_data_updated(void);   // refresh card/list/stats after radar::update()
void ui_show_view(int idx);      // 0 = radar, 1 = weather, 2 = list, 3 = stats
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock);  // HUD: signal bars (count=RSSI, colour: red=down, amber=stale feed, white=ok) + clock
void ui_set_battery(int pct, bool charging, bool present);  // top HUD battery indicator
void ui_set_date(const char *date);  // top HUD date line (e.g. "08 Jun 2026")
void ui_set_netinfo(const char *line);  // stats view footer: how to reach the config page
void ui_splash_show(void);  // branded boot splash on top of the UI
void ui_splash_hide(void);  // fade/remove the boot splash when setup is ready
void ui_set_range_cb(void (*cb)(float km));  // on-screen zoom button -> notify main
void ui_set_range_km(float km);              // update the zoom button label / sync the cycle
void ui_set_pan_cb(void (*cb)(int dx, int dy, bool done));  // two-finger drag map center
void ui_set_units(int preset);               // 0 = Aviation (ft,kt,km) · 1 = Metric (m,km/h,km) · 2 = Imperial (ft,mph,mi)
void ui_set_weather_refresh_cb(void (*cb)(void));  // Weather page Refresh button
void ui_weather_updated(void);                      // repaint weather page after fetch state changes
