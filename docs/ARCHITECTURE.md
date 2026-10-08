# Architecture

## Concurrency model
- **`adsb_task` (pinned to core 0)**: maintains WiFi, fetches the feed every `POLL_INTERVAL_MS`, parses it, and atomically swaps the result into a shared `std::vector<Aircraft>` protected by `g_ac_mutex`. Three providers are tried in order — airplanes.live → adsb.fi → adsb.lol (`src/config.h`), each paced independently (403 parks it, 429/timeout/empty-200 backs it off; see `adsb_pacing.h`). Stale entries expire by `seen_pos`.
- **Render loop (core 1 / Arduino `loop()`)**: drives `lv_timer_handler()`, copies the aircraft snapshot under the mutex, and renders the active view. No blocking calls here.

```
[airplanes.live · adsb.fi · adsb.lol] --HTTPS--> adsb_task (core0) --mutex--> g_aircraft[] <--mutex-- render (core1) --> LVGL/Arduino_GFX --> AMOLED
                                                                                  ^ touch (CST9217/FT3168) -> hit test
```

## Memory
- Full RGB565 framebuffer = 466×466×2 ≈ **434 KB** → PSRAM. Double-buffer fits easily in 8 MB.
- Allocate LVGL draw buffers in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`); keep partial-render buffers if full-frame proves heavy.

## Rendering the scope
The scope is an LVGL canvas in `radar_view.cpp`: rings, rose, crosshair, altitude-colored
glyphs rotated by `track`, fading trails and a persistent flow layer. The animated sweep is
composited per-pixel by `display.cpp` (angular fade over the canvas, fixed-point math,
partial-refresh masks) on a 30 ms tick (`SWEEP_FRAME_MS` in `config.h`; about 28 updates per
second measured on a 1.75).
JSON documents are parsed with a PSRAM allocator (`adsb_client.cpp`) and mbedTLS buffers are
routed to PSRAM (`mbedtls_platform_set_calloc_free`, `main.cpp`), so TLS never starves internal RAM.

## Input
- Touch → screen coordinates → nearest-glyph hit test (within a tap radius) → select + populate detail card (HDG shows the aircraft's ground track).
- Swipe gestures switch views; long-press cycles the visual theme; the on-screen zoom button cycles the range preset (10/20/30/50/100/150 km, shared with the web page).
- IMU posture from QMI8658 drives face-down sleep; touch inactivity drives the idle dim; LVGL indev for touch.

## Persistence & lifecycle
- `Preferences` (NVS), namespace `capsuleradar`; keys read in `main.cpp`: `homeLat`, `homeLon`,
  `rangeKm`, `bright`, `vol`, `mute`, `alertmode`, `proxkm`, `usegps`, `traillen`, `maxac`,
  `idledim`, `fdsleep`, `units`, `tz`, `bigtext`, `theme` (long-press or the web page), `sweep`,
  `airports`, `hideground`, `minalt`, `maxalt`, `milonly`, `lastview`, `rotDeg` (older installs:
  `rot`). The route cache has its own namespace, `routes` (`route_client.cpp`).
- First boot without saved credentials opens the WiFiManager captive portal (WiFi only; the
  centre lat/lon and every other setting are configured on the device's web page). A held
  BOOT button is NOT wired up — `PIN_BOOT_BUTTON` is defined in the board headers but unused.
- ArduinoOTA enabled after WiFi is up.
- **Power off**: the Stats view has a hold-1.5 s "Power off" button and the web server answers `POST /poweroff`, both driving the AXP2101 shutdown. The button shows only when the build has a PMIC (`BOARD_HAS_PMIC`, the 1.75) and the PMIC answered at boot; otherwise it stays hidden and `POST /poweroff` answers 501. A power-off stores the radar as the view to show at the next start (other restarts restore the last view).
- **Self-heal watchdog**: `FeedWatchdog` (`feed_watchdog.h`, driven by `adsb_task` in `main.cpp`) restarts the device when the feed has been stuck `ADSB_STUCK_MS` (180 s) with WiFi up and the largest free internal heap block is under `ADSB_STUCK_MIN_LARGEST_BLOCK` — internal-heap fragmentation starving TLS. Only a provider answering (any HTTP status) refreshes it; a poll the pacer skipped does not, because nothing was sent. A feed stuck with a healthy heap (refusals, internet down) keeps running with the amber HUD warning, up to a last-resort restart after 30 minutes without any answer (`ADSB_STUCK_HARD_MS`); that clock pauses while every provider is parked by a 403 or a Retry-After. `tests/feed_watchdog_test.cpp` drives it with the real pacer on the host.

## Files
- `geo.h` — pure math: haversine, bearing, project-to-screen.
- `aircraft.h` — `Aircraft` struct + altitude→color + squawk/emergency helpers.
- `adsb_client.h/.cpp` + `adsb_pacing.h` — HTTPS GET + streaming ArduinoJson parse for the three providers, with host-testable per-provider pacing.
- `radar_view.h/.cpp` — scope rendering: rings, glyphs, trails, themes, detail-card data.
- `display.cpp` — panel bring-up and the per-pixel sweep compositor.
- `ui.cpp` — LVGL views (radar/list/stats), detail card, HUD, zoom button, power-off button.
- `main.cpp` — tasks, WiFi/portal, web config server, NVS settings, self-heal watchdog.
