# Capsule Radar — MakerWorld listing draft

Copy/paste into the MakerWorld upload form. Sections map to MakerWorld fields.
Fill the **`‹TODO›`** placeholders once the 3D enclosure is finished (photos, print
profile, assembly). Suggested preview images live in `docs/img/` (the GIF + theme
shots) until you have real photos of the printed unit.

> ⚠️ **Trademark caution:** keep all naming and artwork neutral. The green-gradient
> skin is the **"Orb"** theme — present it as a generic retro orb/sonar radar look, and
> do **not** reference any anime/franchise or upload third-party logos/artwork in the
> title, images or marketing, or the model may be taken down. The phosphor radar is the
> safe "hero" look for the listing.

---

## Title
**Capsule Radar — Live Flight Radar Desk Gadget (ESP32-S3 Round AMOLED, Touch)**

*(Safer alt title if avoiding the Capsule nod: "Orbit Radar — Live ADS-B Flight Radar, ESP32-S3 Round AMOLED")*

## Summary (one-liner)
A 3D-printed desk gadget that shows **real aircraft flying around you, live**, on a
round touch AMOLED — pulled from a free online ADS-B feed over WiFi. Open-source
firmware, **flash it from your browser** in one click.

## Category / Tags
`electronics` · `gadget` · `esp32` · `desk-toy` · `radar` · `ads-b` · `aviation` ·
`amoled` · `iot` · `arduino` · `flight-tracker`

---

## Description (long)

Capsule Radar turns a Waveshare round AMOLED dev board into a **live air-traffic
radar** for your desk. It fetches nearby aircraft from a free ADS-B feed over WiFi and
plots them on a phosphor-green radar scope centred on your location — rotated by
heading, colour-coded by altitude, with fading trails and an animated sweep. Tap any
aircraft for a detail card (callsign, type, altitude, speed, distance, heading, squawk)
and even its **origin → destination** route.

It's a finished, polished build: touch UI, multiple themes, a boot splash, alert beeps,
battery and real-time-clock support, idle auto-dim, and a built-in web page to
configure everything. No coding required — **flash it straight from Chrome/Edge**.

**The design:** a round "scope-orb" / pocket-watch form factor — a knurled tactile
ring around the bezel, a printed crown/button on top, a side USB-C port, an integrated
speaker, a small kickstand foot, and an engraved logo. Snap-/screw-together, no soldering.

### Features
- 🛩️ **Live traffic** from a free ADS-B feed (airplanes.live, fallback adsb.lol),
  refreshed every couple of seconds.
- 👆 **Touch**: tap an aircraft → full detail card + **route** (origin → destination).
  Swipe between **Radar / List / Stats** views.
- 🎨 **Four themes** (long-press to switch): phosphor scope, a retro "orb" radar skin,
  amber CRT, and night-vision green.
- 🔁 **On-screen zoom** button (10–100 km) and a **web config page** at
  `capsuleradar.local` (centre point, range, theme, brightness, sound, dim timer).
- 🔊 **Alert pings** (built-in speaker): a soft ping for new contacts, an urgent beep
  for emergency/military squawks. Volume + mute on the web.
- 🔋 **Battery-aware** (optional LiPo): on-screen %, low warning, slower polling on
  battery. 🕐 **Real-time clock** keeps time/date across power loss; **NTP** sync.
- 🌙 **Smart dimming**: an idle analog clock or auto-dim (Clock or dim after…) and **face-down
  sleep** (flip it over to turn the screen off).
- ⚡ **Browser flashing** (ESP Web Tools) — or download a single `.bin`. First boot
  opens a WiFi setup hotspot; enter your network from your phone and you're live.

---

## What you need (Bill of Materials)

| Qty | Item | Notes |
|----:|------|-------|
| 1 | **Waveshare ESP32-S3-Touch-AMOLED-1.75** | The brains + screen. Round 466×466 CO5300 AMOLED, capacitive touch, ESP32-S3R8 (8 MB PSRAM / 16 MB flash), IMU, RTC, PMIC, audio codec. |
| 1 | **Speaker** (8 Ω, small, with the board's connector) | For the alert pings. Optional but recommended. |
| 1 | **USB-C cable (data)** | To flash + power. |
| 1 | **LiPo battery** (3.7 V, JST, optional) | For cordless use; the board charges it. ‹TODO: size that fits the enclosure› |
| — | **Printed enclosure** | See print settings below. |
| ‹n› | **Screws / heat-set inserts** | ‹TODO: e.g. M2×4 inserts + M2×6 screws› |

> The Waveshare board is the only electronics you must buy — everything (display,
> touch, IMU, RTC, battery management, audio) is on it. No soldering required.

---

## Print settings  ‹TODO — fill from your slicer once the model is final›

| Setting | Value |
|---|---|
| Printer | Bambu Lab ‹model› (sliced in Bambu Studio) |
| Plate | **Textured PEI** (gives the matte exterior finish) |
| Material | ‹TODO: PLA / PETG› |
| Nozzle | ‹0.4 mm› |
| Layer height | ‹0.2 mm› |
| Walls / Infill | ‹3 walls / 15 %› |
| Supports | ‹TODO — likely needed for the rounded outer ring overhang› |
| Print time | ‹TODO› · Filament: ‹TODO g› |

**Parts (5):**
1. **Outer tactile ring** — the rounded, knurled bezel band.
2. **Front mount ring / inner bezel** — holds the board; round screen window + screw bosses.
3. **Back plate / lid** — engraved "Capsule Radar" logo on the outside.
4. **Top crown** — decorative (the "antenna" nub), press-fit.
5. **Kickstand foot** — the angled wedge that tilts the unit upright.

> Tip: print the rings in white and the inner trim ring in a contrasting dark filament
> (as shown) for the two-tone look; the back-plate logo is engraved (or a colour swap).

## Assembly
1. ‹Press any heat-set inserts into the bosses, if used.›
2. Seat the Waveshare board in the **front mount ring**, AMOLED aligned with the window;
   route the **USB-C port** to the side cut-out.
3. Plug the **speaker** into the board's connector (and the LiPo, if used).
4. Slip the **outer tactile ring** over the front, then close with the **back plate**
   (logo facing out) and fasten ‹with the screws into the bosses›.
5. Press-fit the **top crown**; attach the **kickstand foot** at the back.
6. Flash the firmware (next section) and run the WiFi setup.

---

## Flash & set up (no toolchain)
1. Open the **web flasher**: https://socquique.github.io/capsule-radar/ (Chrome/Edge desktop).
2. Plug the board in with a USB-C **data** cable, click **Install**, pick the serial port.
3. On first boot, join the **`CapsuleRadar-Setup`** WiFi from your phone and enter your
   network + home location.
4. Fine-tune anything later at **`http://capsuleradar.local/`**.

Prefer a file? Download `CapsuleRadar-esp32s3.bin` from the
[GitHub Releases](https://github.com/socquique/capsule-radar/releases) and flash to
offset `0x0` with esptool. Build from source with PlatformIO if you like.

---

## Source code & firmware
Open source (firmware + simulator + web flasher):
**https://github.com/socquique/capsule-radar**

## Credits & data
- Aircraft data: **airplanes.live** (free, **non-commercial / educational** use) ·
  routes: **adsbdb.com**. Please keep request rates gentle.
- Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75.
- Firmware: open source (see the repo for the license).

## License (pick one on MakerWorld)
Because the project relies on **non-commercial** data sources, a non-commercial model
license is the consistent choice — e.g. **CC BY-NC 4.0** (Attribution-NonCommercial).
If you want max sharing with attribution, CC BY 4.0; avoid commercial-use licenses.

---

## Preview media to upload
- **Finished promo graphics** (EN + ES) of the assembled unit — use the English one as
  the **cover image**; great call-outs (display, Wi-Fi app, USB-C/battery, tactile ring,
  beep alert, detailed flight view, interior shot).
- `docs/img/radar.gif` — animated hero shot of the live scope.
- `docs/img/radar.png`, `orb.png`, `amber.png`, `military.png` — the four themes.
- The **interior photo** (board + speaker in the shell) is perfect for the "what's
  inside / assembly" gallery.
- ‹TODO› — a couple of plain desk photos of the real unit (lit screen) also help.
