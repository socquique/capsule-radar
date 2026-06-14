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
    THEME_DRAGON   = 1,   // DBZ "Dragon Radar": green gradient, grid, yellow blips
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
    float bearingDeg;
    double lat;
    double lon;
    int   squawk;       // -1 if unknown
    bool  emergency;
    bool  military;
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
int  alertCountInRange();                   // emergency or military aircraft within display range
int  emergencyCountInRange();               // emergency squawks within display range
int  militaryCountInRange();                // military aircraft within display range
bool info(int idx, AcInfo& out);

// Sweep self-animates via an internal timer; kept for API compatibility.
void tickSweep();

// Selectable visual skin (THEME_PHOSPHOR / THEME_DRAGON).
void setTheme(int theme);
int  theme();
void cycleTheme();
void setThemeChangedCb(void (*cb)(int theme));   // called when the theme changes (for persistence)
void setRangeLabelVisible(bool v);               // hide the built-in range label (UI shows its own)
void setSweepEnabled(bool on);                   // show/hide radar animations: sweep + center pulse
bool sweepEnabled();
void setAirportsEnabled(bool on);                // show/hide airport markers on the scope
bool airportsEnabled();
void setGroundAircraftEnabled(bool on);          // show/hide aircraft reported on ground
bool groundAircraftEnabled();
void setAircraftLayerVisible(bool on);           // temporarily hide aircraft while panning the map
void setMapPanOffset(int dx, int dy);            // lightweight visual offset while dragging the map

} // namespace radar
