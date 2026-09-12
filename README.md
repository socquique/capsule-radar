# Capsule Radar 🛩️

<p align="center">
  <a href="https://socquique.github.io/capsule-radar/"><img src="https://img.shields.io/badge/Flash%20in%20browser-FF6D00?logo=googlechrome&logoColor=white" alt="Flash in browser"></a>
  <a href="https://makerworld.com/en/models/2907695-capsule-radar-live-flight-radar-desk-gadget"><img src="https://img.shields.io/badge/MakerWorld-3D%20case-1A8917?logo=bambulab&logoColor=white" alt="MakerWorld – 3D case"></a>
  <img src="https://img.shields.io/badge/board-ESP32--S3%20round%20AMOLED-E7352C?logo=espressif&logoColor=white" alt="Board: ESP32-S3 round AMOLED">
  <a href="https://github.com/socquique/capsule-radar/releases"><img src="https://img.shields.io/github/v/tag/socquique/capsule-radar?label=firmware&color=7B42BC" alt="Firmware version"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/code-MIT-2088FF" alt="License: MIT"></a>
  <img src="https://img.shields.io/github/languages/count/socquique/capsule-radar?label=languages&color=FFC107" alt="Languages">
  <a href="https://github.com/socquique/capsule-radar/stargazers"><img src="https://img.shields.io/github/stars/socquique/capsule-radar?style=social" alt="GitHub stars"></a>
</p>

<p align="center">
  <img src="docs/img/device.JPG" width="330" alt="Capsule Radar — a live flight on the device">
</p>
<p align="center"><sub>A real flight on the device: callsign, type, altitude/speed, <b>route</b> (Lisbon → Abu Dhabi) and the <b>aircraft photo</b> — all looked up automatically.</sub></p>

A live **ADS-B aircraft radar** for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** — a round 466×466 AMOLED with capacitive touch. It pulls nearby aircraft from a free online feed over WiFi and plots them on a touch radar scope centered on your location, with live flight details and selectable visual skins.

> Visual reference: open [`assets/plane_radar_2.0_mockup.html`](assets/plane_radar_2.0_mockup.html) in a browser.

<p align="center"><img src="docs/img/radar.gif" width="360" alt="Capsule Radar live scope"></p>

| Phosphor | Orb | Amber CRT | Military |
|:--:|:--:|:--:|:--:|
| ![Phosphor](docs/img/radar.png) | ![Orb](docs/img/orb.png) | ![Amber](docs/img/amber.png) | ![Military](docs/img/military.png) |

<sub>Captured from the bundled desktop simulator (the device screen is round; the square corners are off-panel).</sub>

## Features

- **Live traffic** from [airplanes.live](https://airplanes.live), [adsb.fi](https://adsb.fi/) and [adsb.lol](https://adsb.lol) (free, non-commercial), updated every couple of seconds. Each provider is paced independently, so one refusing or rate-limiting does not stop the feed. Memory-safe streaming parser with a hard aircraft cap.
- **Four themes** (long-press the screen to cycle, or pick on the web; remembered across reboots):
  - **Phosphor** — green-on-black radar scope: rings, animated sweep, aircraft glyphs rotated by heading and color-coded by altitude, fading trails, emergency halo.
  - **Orb** — green gradient + grid scope: the 7 nearest aircraft as yellow orbs emitting waves, off-range traffic as edge arrows pointing its way, orange target rings.
  - **Amber CRT** and **Military** — the same scope retinted (warm amber / night-vision green).
- **Touch** (CST9217): tap an aircraft → detail card (callsign, type, altitude, vertical speed, ground speed, distance, heading, squawk, and **origin → destination** looked up from adsbdb, cached in NVS). **Double-tap** to cycle zoom range. Swipe between **Radar / List / Stats** (circular layouts).
- **Boot splash** + **alert pings** (ES8311 speaker): a soft ping when a new aircraft enters range, an urgent double-beep for emergency/military — volume & mute on the web page.
- **Smooth motion**: aircraft glyphs glide between polls (interpolated) instead of jumping, using cheap partial redraws.
- **Top HUD**: WiFi status (amber if the data feed is failing), in-range aircraft count, NTP/RTC clock, **battery %** (charging bolt, red when low), and the date. The Stats view footer shows how to reach the config page (`capsuleradar.local` + IP).
- **Battery aware** (AXP2101): shows charge level, warns when low, and slows the feed poll rate on battery to save power.
- **Real-time clock** (PCF85063): keeps the time/date across power loss, so the clock is right even before/without WiFi; re-synced from NTP when online.
- **Smart brightness**: configurable idle auto-dim (no touch), and **face-down sleep** (QMI8658 IMU — flip it over to turn the screen off).
- **GPS auto-location** (optional **-G** board variant): the Waveshare `-G` board has an onboard GPS (Quectel LC76G). Turn it on from the web page and the radar **sets its own center point automatically**, with an on-screen **satellite status icon** (amber while acquiring, green once it has a fix). Standard boards simply enter their location manually.
- **Configuration web page** at `http://capsuleradar.local/` — center point (map picker), display range, theme, **time zone** (auto-detected from your browser), live brightness slider, sound, WiFi reset, and over-the-air firmware update. Settings persist in NVS.
- **First-boot WiFi setup** via a captive portal (`CapsuleRadar-Setup`).

## Hardware

The reference board is the Waveshare **ESP32-S3-Touch-AMOLED-1.75**: ESP32-S3R8 (8 MB PSRAM, 16 MB flash), **CO5300** AMOLED over QSPI, **CST9217** touch, **QMI8658** IMU, **PCF85063** RTC, **AXP2101** PMIC, **ES8311** audio + speaker, microSD.

The Waveshare **ESP32-S3-Touch-AMOLED-1.43** is also supported in-tree. Same SoC, same 466×466 CO5300 panel — but a completely different pin map, an **FT3168** touch controller, and no PMIC or audio codec (so no battery readout and no alert pings; everything else works).

| | 1.75 (reference) | 1.43 |
|---|---|---|
| PlatformIO env | `esp32-s3-amoled-175` | `esp32-s3-amoled-143` |
| QSPI CS / SCLK / D0–D3 | 12 / 38 / 4,5,6,7 | 9 / 10 / 11,12,13,14 |
| LCD reset | 39 | 21 |
| I2C SDA / SCL | 15 / 14 | 47 / 48 |
| Touch | CST9217 @ 0x5A | FT3168 @ 0x38 |
| Battery / audio | AXP2101 / ES8311 | not fitted |

Pin maps live in [`src/boards/`](src/boards/), one header per board, selected by a `-DBOARD_*` build flag. They are taken from the vendor board definition and then **confirmed on hardware** — never guessed. Shared tunables stay in [`src/config.h`](src/config.h).

## Build & flash (PlatformIO)

```bash
pio run -e esp32-s3-amoled-175 -t upload     # build + flash over USB-C (1.75)
pio run -e esp32-s3-amoled-143 -t upload     # ...or the 1.43
pio device monitor -b 115200                  # serial log
```
The boot log names the board it was built for (`Capsule Radar boot  fw … board …`) — worth checking first if the screen stays black, since the wrong board's image drives the wrong pins.
On first flash you may need to hold **BOOT** then tap **RESET**. After flashing, on first boot connect your phone to the **`CapsuleRadar-Setup`** WiFi and enter your home network — real aircraft appear within seconds.

## Flash from your browser (no toolchain)

Makers can flash without installing anything using **ESP Web Tools** (Chrome or Edge on desktop):

1. Open the **[web flasher](https://socquique.github.io/capsule-radar/)** (the project's GitHub Pages site).
2. **Pick your board** (1.75″ or 1.43″) — the wrong image boots with a black screen.
3. Plug the board in with a USB-C **data** cable and click **Install**.

The flasher is built and published automatically by GitHub Actions ([`.github/workflows/webflasher.yml`](.github/workflows/webflasher.yml)) on every push to `main` — enable it once in **Settings → Pages → Source = GitHub Actions**. Tagged releases (`git tag v1.0.0 && git push origin v1.0.0`) also attach a ready-to-flash `CapsuleRadar-esp32s3.bin` to a **GitHub Release** via [`release.yml`](.github/workflows/release.yml). To preview the flasher locally:

```bash
./scripts/build_webflasher.sh                      # build + merge into web/flash/
python3 -m http.server -d web/flash 8000           # serve (Web Serial works on localhost)
# open http://localhost:8000
```

## Desktop simulator

The whole UI is portable LVGL and runs on your computer (SDL2) — great for iterating without hardware:
```bash
pio run -e native -t exec     # opens a 466×466 window (needs SDL2: `brew install sdl2`)
```
Mouse = touch · `T` = switch theme · close the window to quit.

## Configuration

Browse to `http://capsuleradar.local/` (or the device IP) on the same WiFi to set the **center lat/lon**, **display range**, **theme** and **brightness**, or to **reset WiFi**. Saving restarts the device to apply.

## Repo layout

```
src/
  config.h           pins + tunables (Dénia, Spain by default)
  main.cpp           tasks, WiFi/NTP, web config, brightness/IMU glue
  display.*          CO5300 (Arduino_GFX) + LVGL bring-up
  radar_view.*       the radar scope, aircraft, themes
  ui.*               views (radar/list/stats) + detail card + HUD
  touch_cst9217.*    capacitive touch driver
  imu_qmi8658.*      accelerometer (face-down sleep)
  battery.*          AXP2101 battery gauge
  rtc_pcf85063.*     PCF85063 real-time clock
  adsb_client.*      airplanes.live fetch + parse
  route*.* route.*   origin→destination lookup (adsbdb)
  sim_main.cpp       native SDL simulator (not flashed)
include/lv_conf.h    LVGL config (v8)
web/flash/           browser web-flasher (ESP Web Tools) for makers
scripts/             build_webflasher.sh (merge firmware -> single .bin)
docs/                hardware / data-source / architecture notes
```

## Community ports & forks

The reference board is the **Waveshare ESP32-S3-Touch-AMOLED-1.75**, but other boards are welcome upstream: a port contributed as a pull request (its own PlatformIO env + board header in [`src/boards/`](src/boards/)) gets built by the CI on every release alongside the 1.75 — the in-tree **1.43** port is the worked example to copy, and the 2.1" port below is on that path. Ports that prefer to stay independent live as forks, linked here (MIT: fork away, and tell me so I can add yours).

- **[Capsule Radar for the Waveshare ESP32-S3-Touch-LCD-2.1](https://github.com/alexzogh/capsule-radar/tree/port/esp32-s3-lcd-21)** by **@alexzogh (STLWarehouse)** — a full port to the 2.1" round LCD (ST7701), plus new features: **double-tap to track an aircraft** (scope re-centres on it), a **clock face on idle**, and the busy-airspace **query-radius fix** now merged back into this firmware. Ships its own binaries per release (files with `lcd21` in the name are the 2.1 builds).
- **[Capsule Radar for the Waveshare 2.8" (non-touch)](https://github.com/ijord/capsule-radar)** by **@ijord** — port to the 2.8" ST7701 panel, with a smoother sweep compositor and the **PNG heap-corruption fix** that shipped upstream in v1.3.28. Thank you!
- **[Skyglass](https://github.com/SilentWolf75/skyglass)** by **@SilentWolf75** — an independently evolved sibling (190+ commits): **shaded OpenStreetMap basemap rendered on-device** and **ESP32-P4** support.

## Data & license

**Firmware / code: [MIT](LICENSE)** — fork and build on it freely (keep the notice). Aircraft data: **airplanes.live** (free, **non-commercial / educational** — exactly this project; be polite with request cadence). Routes: **adsbdb.com** (free). Personal/hobby project. The 3D-printed enclosure is published on [MakerWorld](https://makerworld.com/en/models/2907695-capsule-radar-live-flight-radar-desk-gadget) (enclosure + this firmware).
