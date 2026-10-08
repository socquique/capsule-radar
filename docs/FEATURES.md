# Features — Capsule Radar

Visual target: `assets/plane_radar_2.0_mockup.html`.

## Look (the "prettier")
- True-black AMOLED background; phosphor-green scope, glow on the sweep edge.
- Concentric range rings + crosshair + N/E/S/W rose; outer-ring range label (e.g. "15 km").
- Rotating sweep with a trailing alpha gradient.
- Aircraft as **plane glyphs rotated by track/heading** (not dots).
- **Altitude color map**: ≤3k ft red → 3–10k amber → 10–20k lime → 20–30k green → 30k+ cyan.
- **Fading trail** behind each aircraft (last N positions / direction tail).
- Center "you" dot with a soft pulse.
- Top HUD: WiFi strength, aircraft count, clock.

## Functions (the "more")
1. **Touch to inspect** — tap nearest glyph → detail card: callsign, type, altitude, ground speed, vertical speed, distance, heading (the aircraft's ground track), squawk, and origin → destination. Registration on the card is *not built yet*.
2. **Views** (swipe): Radar · List (sorted by distance) · Stats (count, closest, max altitude and range) · Weather. Tap an aircraft to open its detail card.
3. **Range zoom** — the on-screen zoom button cycles 10 / 20 / 30 / 50 / 100 / 150 km (the same presets as the web page) and re-queries the aircraft feed accordingly.
4. **Orientation** — north-up ↔ track-up toggle. *Not built yet: the scope is north-up only.*
5. **Alerts** — highlight + speaker **ping** for emergency squawks (7500/7600/7700) and military (`dbFlags`). Card flashes red (see RESCUE51 in the mockup). A user watch-list of types (A380, B52…) is *not built yet*.
6. **Night auto-dim** — lower brightness after dusk using the PCF85063 RTC. *Not built yet; today only the idle auto-dim (no touch for a while) exists.*
7. **IMU gestures** (QMI8658) — face-down → screen sleep; face-up → wake. Shake-to-force-refresh is *not built yet*.
8. **Setup & maintenance** — first-boot **captive portal** (WiFi credentials only; centre lat/lon, range and everything else are set on the web page); settings in NVS; **OTA** updates. On boards with a PMIC (the 1.75), the Stats view has a hold-to-power-off button and the web server answers `POST /poweroff`.
9. **Three-day weather forecast** — a round-display layout with current temperature and condition, apparent temperature, humidity and wind, followed by three aligned daily columns for condition, high/low temperature and rain probability. Data comes from [Open-Meteo](https://open-meteo.com/), uses the saved or GPS-derived radar centre, follows the selected unit preset and refreshes every 30 minutes.
10. **WX precipitation radar** — a north-up, nearly fullscreen circular precipitation view centred on the saved/GPS position. Aviation-style range rings, centre marker, nearest-IATA context, current-weather footer and source timestamp are rendered as overlays. The approximately 75 km-radius image is cached in PSRAM and refreshed every five minutes from [RainViewer](https://www.rainviewer.com/api.html) (personal/educational use; availability is not guaranteed). This layer shows rain, snow and hail echoes—not ordinary cloud cover.
11. **Meteosat cloud imagery** — a location-centred, approximately 400 km-wide satellite view from the official [EUMETSAT EUMETView WMS](https://user.eumetsat.int/data-access/eumetview). The Cloud Type RGB product makes cloud structure and classification visible beneath the same aviation overlays, with a separate satellite timestamp and EUMETSAT attribution. Images require no API key, are decoded directly into PSRAM and refresh every ten minutes. Cloud colours describe satellite-derived cloud properties; they do not directly indicate precipitation severity.
12. **Weather-mode control** — the on-screen control cycles **WX Radar → Sat Clouds → 3-Day Forecast**. The imagery itself can also be tapped to advance to the next mode.
13. **Graceful network handling** — weather products load independently after WiFi connects, retain their last valid image during transient failures and retry failed requests without blocking the display. Status text distinguishes forecast, precipitation-radar and satellite-data waits.
14. **Desktop simulator** — the same 466×466 LVGL interface runs locally through SDL2. It includes mock aircraft, forecast data, precipitation cells and Meteosat-style cloud bands, allowing all three weather modes and round-screen layouts to be reviewed without flashing hardware. Live network imagery is fetched only by the ESP32 firmware.

## Weather data at a glance

| Mode | What it shows | Coverage | Provider | Refresh |
|---|---|---:|---|---:|
| WX Radar | Measured precipitation echoes | ~75 km radius | RainViewer | 5 min |
| Sat Clouds | Satellite-derived cloud types | ~400 km across | EUMETSAT | 10 min |
| 3-Day Forecast | Current conditions and daily outlook | Configured/GPS point | Open-Meteo | 30 min |

## Nice-to-have / later
- Route enrichment (origin→destination) via a secondary lookup.
- Sound themes / mute.
- Multiple saved home locations.
- microSD logging of seen aircraft.

## MakerWorld packaging
- Parametric printable enclosure for the round board (bezel + stand), à la the original radar.
- Publish firmware + STLs; include a looping GIF of the live sweep. The original radar earned MakerWorld "featured/boost" badges — same formula here for points toward the P2S.
