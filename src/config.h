#pragma once
// Capsule Radar — build & user configuration.

#define FW_VERSION "1.4.0"   // shown on the web config page + Stats screen; bump on release

// ---------- Home location (default: Dénia, Spain) ----------
// Overridable at runtime via the captive portal (stored in NVS).
#define HOME_LAT_DEFAULT   38.8409
#define HOME_LON_DEFAULT    0.1059

// ---------- Radar ----------
#define RANGE_KM_DEFAULT    30.0f          // display range (outer ring). Query is wider, see below.
// Feed query radius = display range × MULT, clamped to [MIN, MAX]. Querying a bit wider than
// the display shows off-range traffic as edge arrows. The floor MUST stay small: the old 50 km
// floor made small display ranges still pull a huge aircraft list in busy airspace, which timed
// out the poll (feed permanently amber near big hubs). Fix contributed by @alexzogh (STLWarehouse).
#define ADSB_QUERY_MULT     1.4f
#define ADSB_QUERY_MIN_KM   12.0f
#define ADSB_QUERY_MAX_KM   150.0f
static const float RANGE_STEPS_KM[] = {10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
#define POLL_INTERVAL_MS    2000           // be gentle with the free API (>=1000)
#define POLL_INTERVAL_BATTERY_MS 5000      // slower polling when running on battery
#define MOTION_INTERP       1              // 1 = glyphs glide between polls; 0 = snap to new pos
#define AC_STALE_MS         15000          // keep the last contacts through brief empty feed responses

// ---------- Weather forecast (Open-Meteo, no API key) ----------
#define WEATHER_REFRESH_MS  1800000UL      // 30 minutes; forecast data changes slowly
#define WX_RADAR_REFRESH_MS 300000UL       // RainViewer frames update about every 5 minutes
#define CLOUD_IMAGE_REFRESH_MS 600000UL    // EUMETSAT MTG cloud imagery; cache for 10 minutes

// ---------- Screen (CO5300 AMOLED, 466x466 on every supported board) ----------
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
#define LV_COLOR_DEPTH_BITS 16
// LCD_COL_OFFSET / LCD_ROW_OFFSET / LCD_QSPI_HZ are panel-specific -> board header.
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
#define TZ_STR              "CET-1CEST,M3.5.0,M10.5.0/3"  // POSIX TZ (Spain) for local time/date
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         20000          // dim the screen after this long without a touch

// ---------- ADS-B API (free, non-commercial) ----------
// Providers are tried in this order; each is paced independently (see AdsbClient).
// All three return the same readsb shape (an "ac" array), but NOT the same URL path,
// so each carries its own template in the provider table in adsb_client.cpp.
#define ADSB_PRIMARY_HOST   "api.airplanes.live"   // GET /v2/point/{lat}/{lon}/{radius_nm}
                                                    //   now requires prior approval by email
#define ADSB_OPENDATA_HOST  "opendata.adsb.fi"     // GET /api/v3/lat/{lat}/lon/{lon}/dist/{nm}
                                                    //   documented 1 req/s; personal use only,
                                                    //   attribution required (see docs/DATA_SOURCE.md)
#define ADSB_FALLBACK_HOST  "api.adsb.lol"          // same readsb format; limits are dynamic
#define ADSB_PROVIDER_COUNT 3
// Sent with setUserAgent(), never addHeader() — see the note in docs/DATA_SOURCE.md.
// Carries FW_VERSION: now that the header actually goes out, and adsb.lol checks it for
// valid contact info, a provider should be able to tell which build is talking to them.
#define ADSB_USER_AGENT     "CapsuleRadar/" FW_VERSION " (ESP32-S3 hobby; +https://github.com/socquique/capsule-radar)"
#define ADSB_HTTPS_INSECURE 1               // 1 = setInsecure() (hobby). 0 = use pinned root CA.
#define ADSB_MAX_AIRCRAFT   60              // hard cap parsed per poll (protect RAM in busy areas)
// How long to stop asking a provider that refused us. 403 is a policy refusal (needs
// approval, or a User-Agent they reject) and will not clear in seconds; 429 just means
// we were too fast. Without these, a permanently-403 provider is retried every poll,
// which doubles the request rate onto the surviving provider and trips ITS rate limit.
#define ADSB_COOLDOWN_403_MS  900000UL      // 15 min park after a policy refusal
#define ADSB_SPACING_STEP_MS    4000UL      // first extra gap imposed after a 429
#define ADSB_SPACING_MAX_MS    60000UL      // never space a provider out further than this
#define ADSB_SPACING_EASE_OKS       3       // successes needed before easing the gap back down
#define ADSB_FEED_STALE_MS     60000UL      // no successful fetch for this long -> HUD warning

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial

// ---------- Board ----------
// The pin map, panel gaps, touch driver and which peripherals exist all live in a
// per-board header. Select one with a build flag in platformio.ini; the 1.75 is the
// default so an unflagged build behaves exactly as before.
//   -DBOARD_AMOLED_143  -> Waveshare ESP32-S3-Touch-AMOLED-1.43
//   (none)              -> Waveshare ESP32-S3-Touch-AMOLED-1.75  (reference board)
// Never guess pins for a new board: take them from the vendor demo or the Arduino
// core board variant, then confirm them on hardware before committing.
#if defined(BOARD_AMOLED_143)
#  include "boards/board_amoled_143.h"
#else
#  include "boards/board_amoled_175.h"
#endif

// Safety net: catches a board header that still has placeholder pins in it.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "board header: QSPI/I2C pins are placeholders (-1). Fill in the real values."
#endif
