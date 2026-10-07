#pragma once
// Scope rendering API (M1 scope, M2 aircraft, M3 selection). See docs/ARCHITECTURE.md.
// Visual reference: assets/plane_radar_2.0_mockup.html
#include <stdint.h>
#include <vector>
#include "aircraft.h"

struct RadarSettings {
    double homeLat, homeLon;
    float  rangeKm;
    double rotationDeg = 0.0;   // 0 = north-up
    bool   mute = false;
};

// Selectable visual skins.
enum RadarTheme {
    THEME_PHOSPHOR = 0,   // green-on-black radar scope (the mockup look)
    THEME_ORB   = 1,   // Orb scope: green gradient, grid, yellow blips
    THEME_AMBER    = 2,   // amber CRT scope (warm monochrome chrome)
    THEME_MILITARY = 3,   // night-vision / military green scope
    THEME_COUNT    = 4
};

// Flattened, display-ready info for one aircraft (detail card / list view).
struct AcInfo {
    char  hex[8];
    char  call[12];
    char  type[8];
    float altFt;
    bool  onGround;
    float vsFpm;        // NaN if unknown
    float gsKt;         // NaN if unknown
    float distKm;
    float bearingDeg;   // bearing from home (deg)
    float track;        // ground track deg (NaN if unknown) — the heading the glyph points
    int   squawk;       // -1 if unknown
    bool  emergency;
};

namespace radar {

// Build the radar scope (rings, crosshair, rose, sweep, center) under `parent`.
void init(void* lv_parent);                 // pass lv_obj_t*

// Rebuild the aircraft layer from the latest snapshot. Call at poll cadence.
void update(const std::vector<Aircraft>& aircraft, const RadarSettings& s);

// Nearest aircraft to (x,y) within a tap radius -> snapshot index, or -1.
int  hitTest(int x, int y);

// Selection (tracked by hex so it survives data updates). idx < 0 clears.
void select(int idx);
// Select by hex: the list captures it at touch-down, because polls rewrite its rows in
// place. False (selection unchanged) when that aircraft has left the feed.
bool selectHex(const char* hex);
bool selected(AcInfo& out);                 // false if nothing selected/visible

// Snapshot access for the list / stats views.
int  count();
int  countInRange();                        // aircraft within the display range (for the HUD)
bool info(int idx, AcInfo& out);

// Sweep self-animates via an internal timer; kept for API compatibility.
void tickSweep();

// Selectable visual skin (THEME_PHOSPHOR / THEME_ORB).
void setTheme(int theme);
int  theme();
void cycleTheme();
void setThemeChangedCb(void (*cb)(int theme));   // called when the theme changes (for persistence)
void setRangeLabelVisible(bool v);               // hide the built-in range label (UI shows its own)
void setSweepEnabled(bool on);                   // show/hide the rotating sweep line
// Hand the sweep to a per-pixel compositor (device: display.cpp). config gets the theme's
// ring/lead colours (RGB565) and whether a sweep should show; tick is called during long
// rebuilds so the sweep keeps moving. Without it, LVGL draws the sweep.
void setSweepCompositor(void (*config)(uint16_t ring565, uint16_t lead565, uint16_t ink565, bool sweep, bool pulse), void (*tick)());
bool sweepEnabled();
void setAirportsEnabled(bool on);                // show/hide airport markers on the scope
bool airportsEnabled();
void setTrailLength(int level);                  // 0=off 1=short 2=medium 3=long (aircraft trails + flow)
void setMaxOnScreen(int n);                       // how many (nearest) aircraft to draw on the scope
void setLargeText(bool on);                       // accessibility: bigger glyph labels. Call BEFORE init()
void setUnits(int preset);                       // 0 = feet · 1 = metres (scope altitude labels; same presets as ui_set_units)

} // namespace radar
