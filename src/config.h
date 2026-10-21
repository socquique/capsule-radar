#pragma once
// Capsule Radar — build & user configuration.

#define FW_VERSION "1.5.0"   // shown on the web config page + Stats screen; bump on release

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
// Display-range presets. The zoom button on the scope cycles RANGE_STEPS_KM (up to 100 km);
// the web page's <select> offers WEB_RANGE_KM, which goes on to RANGE_KM_MAX (150 km). Nothing
// goes past 150: the feed query clamps to ADSB_QUERY_MAX_KM, so a wider range (the old
// web-only 250 km) promised rings the feed never fills. A stored range outside
// [RANGE_KM_MIN, RANGE_KM_MAX] is clamped at boot and on save; <= 0, NaN or inf becomes
// RANGE_KM_DEFAULT (0 divides by zero in geo::projectToScreen).
static const float RANGE_STEPS_KM[] = {10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
static const float WEB_RANGE_KM[]   = {10.0f, 15.0f, 25.0f, 30.0f, 50.0f, 100.0f, 150.0f};
#define RANGE_KM_MIN        10.0f
#define RANGE_KM_MAX        150.0f
#define POLL_INTERVAL_MS    2000           // be gentle with the free API (>=1000)
#define POLL_INTERVAL_BATTERY_MS 5000      // slower polling when running on battery
#define MOTION_INTERP       1              // 1 = glyphs glide between polls; 0 = snap to new pos
#define AC_STALE_MS         15000          // keep the last contacts through brief empty feed responses

// ---------- Weather forecast (Open-Meteo, no API key) ----------
#define WEATHER_REFRESH_MS  1800000UL      // 30 minutes; forecast data changes slowly
#define WX_RADAR_REFRESH_MS 300000UL       // RainViewer frames update about every 5 minutes
#define CLOUD_IMAGE_REFRESH_MS 600000UL    // EUMETSAT MTG cloud imagery; cache for 10 minutes

// ---------- Screen geometry (AMOLED defaults; LCD 2.1 overrides below) ----------
#if !defined(BOARD_LCD21)
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
#endif
// radar sweep (radar_view.cpp draws it in LVGL; display.cpp's compositor draws it per pixel)
#define SWEEP_PERIOD_MS     8000           // one revolution (LVGL sweep)
#define SWEEP_COMP_PERIOD_MS 40000         // one revolution when the compositor draws it (a slower sweep hides the
                                           // brief pauses while LVGL rebuilds the scope after a poll)
#define SWEEP_FRAME_MS      30             // sweep tick
#define SWEEP_TRAIL_DEG     38.0f          // fading trail behind the lead line
#define SWEEP_TRAIL_STEPS   20             // LVGL sweep: lines in the trail
#define SWEEP_TRAIL_OPA     72             // trail opacity at the lead edge (fades to 0)
#define SWEEP_LEAD_OPA      217            // lead line opacity
#define SWEEP_LEAD_W        2.0f           // compositor: lead line width in pixels (anti-aliased, round ends)
#define PULSE_PERIOD_MS     2600           // centre ripple: one expansion
#define PULSE_D_MIN         10             // ring diameter at the start / end of an expansion (px)
#define PULSE_D_MAX         54
#define PULSE_W             2              // ring width (px)
#define PULSE_OPA           220            // ring opacity at the start (fades to 0)
#define LV_COLOR_DEPTH_BITS 16
// LCD_COL_OFFSET / LCD_ROW_OFFSET / LCD_QSPI_HZ are panel-specific -> board header.
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
// Face-down sleep (QMI8658, logic in facedown_sleep.h): the IMU is read every
// FACEDOWN_CHECK_MS, and FACEDOWN_COUNT face-down readings in a row with no touch in between
// turn the screen off (~1.6 s). Accelerometer Z at ±2 g full scale: 16384 LSB/g.
#define FACEDOWN_CHECK_MS   400
#define FACEDOWN_COUNT      4
#define FACEDOWN_THRESHOLD  9000           // ~0.55 g past zero, on the side opposite the resting pose
#define TZ_STR              "CET-1CEST,M3.5.0,M10.5.0/3"  // POSIX TZ (Spain) for local time/date
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         20000          // dim the screen after this long without a touch
// Idle clock: instead of dimming, the radar animates into an analog clock (clock_face.h has
// the animation, radar_view.cpp the dial and the dimmed scene, display.cpp's compositor the
// moving hands). Angles 0 = 12 o'clock; lengths are fractions of RADAR_R_OUTER_PX.
#define CLOCK_ENTER_MS        2000      // radar -> clock animation
#define CLOCK_EXIT_MS         1200      // clock -> radar animation
#define CLOCK_LOCK_FRAC       0.75f     // the spinning sweep line is on the second from here on
#define CLOCK_MIN_SPIN_DEG    120.0f    // ...after spinning forward at least this far
#define CLOCK_RADAR_OPA       150       // planes, trails, rings, map behind the clock (255 = as usual)
#define CLOCK_RADAR_OPA_STEP1 200       // ...during the first step of the animation
#define CLOCK_TRAIL_OPA       150       // sweep trail behind the second hand (255 = the radar's)
#define CLOCK_BRIGHTNESS_PCT  70        // panel brightness on the clock, % of the day brightness
#define CLOCK_HOUR_LEN        0.50f     // hands
#define CLOCK_MIN_LEN         0.76f
#define CLOCK_SEC_LEN         0.94f
#define CLOCK_SEC_TAIL        0.14f     // second hand: tail past the centre
#define CLOCK_HOUR_W          8         // hand widths, px
#define CLOCK_MIN_W           5
#define CLOCK_OUTLINE_W       4         // dark outline around the hour/minute hands (extra width, px)
#define CLOCK_OUTLINE_OPA     200
#define CLOCK_CAP_R           7         // centre cap radius, px
#define CLOCK_CAP_DOT_R       3         // lead-coloured dot on the cap, px
#define CLOCK_NUM_R           0.84f     // dial: centre of the 12/3/6/9 numbers
#define CLOCK_HTICK_R0        0.90f     // hour marks run from here to CLOCK_TICK_R1
#define CLOCK_MTICK_R0        0.965f    // minute marks run from here to CLOCK_TICK_R1
#define CLOCK_TICK_R1         0.995f
#define CLOCK_NUM_OPA         190       // the dial never moves, so it stays dim (AMOLED burn-in)
#define CLOCK_HTICK_OPA       170
#define CLOCK_MTICK_OPA       80

// ---------- Touch ----------
// LVGL reads the touch panel only between frames, and a swipe redraws the whole screen on
// every frame (~150-200 ms on the device). display.cpp also samples the panel between the
// flush strips of a frame and replays the samples to LVGL in order (touch_queue.h).
// At most one panel read per TOUCH_SAMPLE_MS between strips. Keep it at LVGL's read period
// (LV_INDEV_DEF_READ_PERIOD in include/lv_conf.h): LVGL predicts a swipe's throw from the
// movement per read, so denser samples shrink the throw and short flicks snap back.
#define TOUCH_SAMPLE_MS         20
#define TOUCH_NODATA_RELEASE_MS 100  // failed reads for this long while pressed count as a lift
#define TOUCH_QUEUE_LEN         32   // samples held between two LVGL reads (a full frame adds ~10)

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
#define TLS_HANDSHAKE_S     10              // TLS handshake timeout for every HTTPS client, seconds (core default is 120)
#define ADSB_MAX_AIRCRAFT   60              // hard cap parsed per poll (protect RAM in busy areas)
// How long to stop asking a provider that refused us. 403 is a policy refusal (needs
// approval, or a User-Agent they reject) and will not clear in seconds; 429 just means
// we were too fast. Without these, a permanently-403 provider is retried every poll,
// which doubles the request rate onto the surviving provider and trips ITS rate limit.
#define ADSB_COOLDOWN_403_MS  900000UL      // first park after a policy refusal (15 min)
#define ADSB_COOLDOWN_403_MAX_MS 21600000UL // cap for the escalating 403 park (6 h) — see AdsbPacer::onRefused
#define ADSB_SPACING_STEP_MS    4000UL      // first extra gap imposed after a 429
#define ADSB_SPACING_MAX_MS    60000UL      // never space a provider out further than this
#define ADSB_SPACING_EASE_OKS       3       // successes needed before easing the gap back down
// While EVERY provider we may ask is silent (parked, or failing at the transport level),
// the silence gap (ADSB_SPACING_STEP_MS doubling up to ADSB_SPACING_MAX_MS) is capped at
// this. A network-wide outage then costs at most this long after the link returns, not a
// minute; a provider that stays dead while another one answers still waits the full gap.
// The 429 spacing is not capped. See AdsbPacer::cooling().
#define ADSB_SILENCE_OUTAGE_MAX_MS 10000UL
#define ADSB_FEED_STALE_MS     60000UL      // no successful fetch for this long -> HUD warning
// The feed-stuck watchdog (FeedWatchdog in feed_watchdog.h) calls the feed stuck when WiFi
// is up and no provider has answered for ADSB_STUCK_MS. It may then reboot only when the
// largest free INTERNAL heap block is below ADSB_STUCK_MIN_LARGEST_BLOCK: a TLS handshake
// needs a contiguous block of roughly this size (photo_client.cpp skips a whole photo fetch
// below the same figure). Above it, a stuck feed is an internet outage, not heap
// fragmentation — stay up, warn amber.
#define ADSB_STUCK_MS         180000UL
#define ADSB_STUCK_MIN_LARGEST_BLOCK 28000
// Last resort, independent of the heap check: WiFi up and no provider has answered for
// this long -> restart anyway. Covers a stuck feed whose cause the heap figure does not
// show (a wedged network stack, a socket that never closes). An outage that ends sooner
// never gets here: the first answer resets the clock.
#define ADSB_STUCK_HARD_MS    1800000UL     // 30 min

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial

// ---------- Board ----------
// The pin map, panel gaps, touch driver and which peripherals exist all live in a
// per-board header. Select one with a build flag in platformio.ini; the 1.75 is the
// default so an unflagged build behaves exactly as before.
//   -DBOARD_LCD21       -> Waveshare ESP32-S3-Touch-LCD-2.1
//   -DBOARD_AMOLED_143  -> Waveshare ESP32-S3-Touch-AMOLED-1.43
//   (none)              -> Waveshare ESP32-S3-Touch-AMOLED-1.75  (reference board)
// Never guess pins for a new board: take them from the vendor demo or the Arduino
// core board variant, then confirm them on hardware before committing.
#if defined(BOARD_LCD21)
#  include "boards/board_lcd_21.h"
#elif defined(BOARD_AMOLED_143)
#  include "boards/board_amoled_143.h"
#else
#  include "boards/board_amoled_175.h"
#endif

// Safety net: catches a board header that still has placeholder pins in it.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "board header: QSPI/I2C pins are placeholders (-1). Fill in the real values."
#endif

#ifndef ADSB_TASK_STACK_BYTES
#define ADSB_TASK_STACK_BYTES 16384
#endif
