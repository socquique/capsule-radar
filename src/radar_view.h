#pragma once
// Scope rendering API (M1 scope, M2 aircraft, M3 selection). See docs/ARCHITECTURE.md.
// Visual reference: assets/plane_radar_2.0_mockup.html
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
    THEME_TCAS     = 4,   // TCAS-style traffic display (community design by @pikachu.jp)
    THEME_COUNT    = 5
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
    float bearingDeg;
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
bool sweepEnabled();
void setAirportsEnabled(bool on);                // show/hide airport markers on the scope
bool airportsEnabled();
void setTrailLength(int level);                  // 0=off 1=short 2=medium 3=long (aircraft trails + flow)
void setMaxOnScreen(int n);                       // how many (nearest) aircraft to draw on the scope
void setLargeText(bool on);                       // accessibility: bigger glyph labels. Call BEFORE init()

// TCAS theme: GPS status readouts drawn on the scope (ALT / GPS LEV / FIXED-LOST box).
// state: 0 = GPS off (shows the configured LAT/LON + "GPS OFF"), 1 = no fix (GPS LOST:
// red X, traffic hidden), 2 = fix.
void setGpsStatus(int state, int sats, float altM);
// TCAS theme: network state for the on-scope TIME readout (green fresh / amber stale / red offline).
void setNetStatus(bool wifiUp, bool feedFresh);

} // namespace radar
