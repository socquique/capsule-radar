# Capsule Radar — Live Flight Radar Desk Gadget

> Publication-ready English copy for MakerWorld / Printables / a product page.
> Fill the **`‹TODO›`** placeholders (print profile, fasteners, LiPo size) from your
> slicer. See the trademark note at the bottom before publishing.

---

## Tagline
**See real aircraft flying around you — live, on a round touch AMOLED. Open-source, and you flash it from your browser.**

---

## Overview

Capsule Radar is a 3D-printed desk gadget that shows **live air traffic around your
location**. It pulls nearby aircraft from a free online ADS-B feed over Wi-Fi and plots
them on a glowing round radar scope, rotated by heading and colour-coded by altitude,
with an animated sweep and fading trails. Tap any aircraft to see its full details — even
where it's flying **from and to**.

It's a finished, polished build: a capacitive touch UI, four selectable themes, a boot
splash, audible alerts, battery and real-time-clock support, idle auto-dim, and a built-in
web page to configure everything. **No coding required** — flash it straight from Chrome
or Edge, then enter your Wi-Fi from your phone.

The enclosure is a round "scope-orb / pocket-watch" form factor: a matte shell with a
knurled tactile ring, a crown on top, a side USB-C port, an integrated speaker, and an
engraved logo. No soldering — the Waveshare board carries all the electronics.

---

## Functions (what it does)

**Live air traffic**
- Real aircraft from a free ADS-B feed ([airplanes.live](https://airplanes.live), with
  [adsb.lol](https://adsb.lol) as a fallback), refreshed every couple of seconds.
- Aircraft are drawn as glyphs **rotated by their heading** and **colour-coded by
  altitude**, with fading trails and an animated radar sweep.
- Out-of-range traffic appears as **edge markers pointing in its direction**.
- A memory-safe streaming parser caps the number of aircraft so busy areas never
  overload the device.

**Touch & details**
- **Tap an aircraft** to open a detail card: callsign, type, altitude, vertical speed,
  ground speed, distance, bearing, squawk, and the **origin → destination route**
  (looked up online and cached so the same flight isn't re-queried). The **X** in its
  corner closes it, as does a tap just outside the card.
- **Swipe** between three circular views: **Radar**, **List**, and **Stats**.
- **On-screen zoom button** cycles the display range (10 / 20 / 30 / 50 / 100 km) and
  shows the current value.
- **Hold the radar still** for 1.5 s, then lift, to cycle the visual theme.

**Four themes** (remembered across reboots)
- **Phosphor** — classic green-on-black radar scope: rings, sweep, altitude colours.
- **Capsule / Orb radar** — green grid with glowing "ball" blips emitting sonar-style
  rings and edge arrows for distant traffic.
- **Amber CRT** — warm amber monochrome scope.
- **Military** — night-vision green scope.

**Status HUD**
- Wi-Fi indicator (turns **amber** if the data feed is failing), in-range aircraft count,
  clock, **battery %** (charging bolt; turns **red** when low), and the date.

**Sound (built-in speaker)**
- A soft **ping** when a new aircraft enters range, and an urgent **double-beep** for
  emergency / military contacts. Volume, mute and a test button are on the web page.

**Power & time**
- **Battery aware** (optional LiPo): on-screen charge %, low-battery warning, and a
  slower polling rate on battery to save power.
- **Real-time clock**: keeps the time and date across power loss, so the clock is right
  even before/without Wi-Fi; it re-syncs from the internet (NTP) when online.

**Smart dimming**
- **Idle auto-dim** after a configurable time with no touch.
- **Face-down sleep**: flip the device over to turn the screen off (motion sensor).

**Configuration & updates**
- A built-in **web page** at `http://capsuleradar.local/` to set the **centre point
  (pick it on an interactive map)**, display range, theme, brightness, sound, idle-dim
  timeout, reset Wi-Fi, and **update the firmware over Wi-Fi**.
- All settings persist on the device. First boot opens a Wi-Fi setup hotspot.

---

## What you need (Bill of Materials)

| Qty | Item | Notes |
|----:|------|-------|
| 1 | **Waveshare ESP32-S3-Touch-AMOLED-1.75** | All the electronics in one board: round 466×466 CO5300 AMOLED, capacitive touch, ESP32-S3R8 (8 MB PSRAM / 16 MB flash), motion sensor, real-time clock, power-management/charger, audio codec. |
| 1 | **Speaker** (small 8 Ω, with the board's JST connector) | For the alert pings. Optional but recommended. |
| 1 | **USB-C cable (data)** | To flash and power the device. |
| 1 | **LiPo battery** (3.7 V, JST, optional) | For cordless use; the board charges it from USB-C. Size: ‹TODO — battery that fits the enclosure›. |
| — | **Printed enclosure** | 5 parts, see below. |
| ‹n› | **Fasteners** | ‹TODO — e.g. M2 self-tapping screws / heat-set inserts, or press-fit›. |

> The Waveshare board is the only electronics you need to buy. **No soldering.**

---

## 3D printing

**Print profile** ‹fill from your slicer›

| Setting | Value |
|---|---|
| Printer | ‹TODO› |
| Plate / surface | Textured PEI (gives the matte exterior finish) |
| Material | ‹TODO — PLA / PETG› |
| Nozzle | ‹0.4 mm› |
| Layer height | ‹0.2 mm› |
| Walls / Infill | ‹3 walls / 15 %› |
| Supports | ‹TODO — likely for the rounded outer ring overhang› |
| Print time | ‹TODO› · Filament: ‹TODO g› |

**Parts (5)**
1. **Outer tactile ring** — the rounded, knurled bezel band.
2. **Front mount ring / inner bezel** — holds the board; round screen window + screw bosses.
3. **Back plate / lid** — engraved logo on the outside.
4. **Top crown** — decorative nub, press-fit.
5. **Kickstand foot** — the angled wedge that tilts the unit upright.

> Tip: print the rings in white and the inner trim in a contrasting dark filament for the
> two-tone look; the back-plate logo is engraved (or a colour swap at the right layer).

**Assembly**
1. ‹Press any heat-set inserts into the bosses, if used.›
2. Seat the board in the **front mount ring**, AMOLED aligned with the window; route the
   **USB-C port** to the side cut-out.
3. Plug the **speaker** into the board's connector (and the LiPo, if used).
4. Slip the **outer tactile ring** over the front, then close with the **back plate**
   (logo out) and fasten ‹with the screws into the bosses›.
5. Press-fit the **top crown**; attach the **kickstand foot** at the back.
6. Flash the firmware and run Wi-Fi setup (next sections).

---

## Installation — flash the firmware

You don't need any toolchain.

**Option A — flash from your browser (easiest)**
1. Open the **web flasher**: https://socquique.github.io/capsule-radar/ (Chrome or Edge on desktop).
2. Plug the board into your computer with a USB-C **data** cable.
3. Click **Install**, choose the serial port, and pick **Erase + Install** the first time.

**Option B — download the binary**
- Grab `CapsuleRadar-esp32s3.bin` from the
  [GitHub releases](https://github.com/socquique/capsule-radar/releases) and flash it to
  offset `0x0`:
  ```
  esptool.py --chip esp32s3 write_flash 0x0 CapsuleRadar-esp32s3.bin
  ```

**Option C — build from source** with PlatformIO:
```
pio run -e esp32-s3-amoled-175 -t upload
```

> Tip: if the board isn't detected, hold **BOOT**, tap **RESET**, then release **BOOT**,
> and try again.

---

## First-time setup

1. After flashing, power the device. On first boot it creates a Wi-Fi hotspot named
   **`CapsuleRadar-Setup`**.
2. Join that hotspot from your phone; a setup page opens. Enter your home Wi-Fi.
3. The radar connects and real aircraft appear within seconds.
4. Open **`http://capsuleradar.local/`** on any device on the same network to fine-tune:
   - **Centre point** — tap the **interactive map** or drag the pin (or type lat/lon).
   - **Display range**, **theme**, **brightness**, **sound** (volume / mute / test),
     **dim-screen-after** timeout.
   - **Reset Wi-Fi** to move it to a new network.

---

## Updating over Wi-Fi (no cable)

Once it's running, you can update wirelessly:

1. Open **`http://capsuleradar.local/update`** (also linked from the config page).
2. Upload **`CapsuleRadar-ota.bin`** (the *app* image from the GitHub release — **not**
   the merged flash image).
3. A progress bar runs and the device reboots into the new firmware.

(Builders can also use PlatformIO: `pio run -e esp32-s3-amoled-175-ota -t upload`.)

---

## Using it (quick reference)

| Action | Gesture |
|---|---|
| Inspect an aircraft | **Tap** it → detail card with route |
| Close the detail card | **Tap its X** (or just outside the card, or empty scope) |
| Switch view | **Swipe** left/right (Radar ↔ List ↔ Stats) |
| Change zoom range | **Tap the range button** (⟳ … km) |
| Change theme | **Hold** the radar still for 1.5 s, then lift |
| Turn the screen off | **Flip it face-down** |
| Find the config page | See the **Stats** view footer (shows `capsuleradar.local` + IP) |

---

## Troubleshooting

- **No aircraft showing** — check Wi-Fi (the HUD Wi-Fi icon is red if disconnected, amber
  if the feed is failing) and that your centre point is set near you. Quiet areas/times
  simply have little traffic.
- **Can't reach `capsuleradar.local`** — use the **IP** shown on the Stats view instead.
- **No sound** — check Volume isn't 0 / Mute is off on the web page, and that the speaker
  is connected. Use the **Test ping** button.
- **Lost/changed Wi-Fi** — open the web page → **Reset Wi-Fi**, then rejoin
  `CapsuleRadar-Setup`.

---

## Credits, data & license

- Aircraft data: **airplanes.live** (free, **non-commercial / educational** use). Routes:
  **adsbdb.com**. Please keep request rates gentle — this firmware already does.
- Hardware: **Waveshare ESP32-S3-Touch-AMOLED-1.75**.
- Firmware & web flasher (open source): **https://github.com/socquique/capsule-radar**
- Suggested model license (consistent with the non-commercial data sources):
  **CC BY-NC 4.0** (Attribution-NonCommercial).

---

> ⚠️ **Trademark note for publishing:** the green "radar" look and a comic-style wordmark
> evoke a well-known franchise. Avoid uploading official logos/artwork or using protected
> names in the title/marketing, or the model may be removed. A neutral title/wordmark
> (e.g. "Orbit Radar — Live Flight Radar") is the safe route for a public listing; the
> firmware/repo can keep their current name.
