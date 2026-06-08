#pragma once
// M3 UI: swipeable views (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable (device + SDL simulator). Builds on top of radar_view.
void ui_create(void);            // build the whole UI on the active screen
void ui_on_data_updated(void);   // refresh card/list/stats after radar::update()
void ui_show_view(int idx);      // 0 = radar, 1 = list, 2 = stats
void ui_set_status(bool wifiUp, bool feedOk, const char *clock);  // HUD: wifi (red=down, amber=feed fail) + clock
void ui_set_battery(int pct, bool charging, bool present);  // top HUD battery indicator
void ui_set_date(const char *date);  // top HUD date line (e.g. "08 Jun 2026")
void ui_set_netinfo(const char *line);  // stats view footer: how to reach the config page
void ui_splash_show(void);  // branded boot splash (auto-fades, covers init time)
void ui_set_range_cb(void (*cb)(float km));  // on-screen zoom button -> notify main
void ui_set_range_km(float km);              // update the zoom button label / sync the cycle
