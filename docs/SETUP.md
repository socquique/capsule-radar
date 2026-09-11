# Setup

## Toolchain
- VS Code + **PlatformIO** (recommended for Claude Code) OR Arduino IDE 2.x.
- USB-C cable. On first flash you may need to hold **BOOT** then tap **PWR/RST**.

## PlatformIO
Always pass `-e`: there is one environment per supported board (plus the desktop simulator).
```
pio run -e esp32-s3-amoled-175            # build for the 1.75 (reference board)
pio run -e esp32-s3-amoled-143            # build for the 1.43
pio run -e esp32-s3-amoled-175 -t upload  # flash over USB-C
pio device monitor -b 115200
```
The boot log states which board the image was built for:
`Capsule Radar boot  fw 1.4.0  board ESP32-S3-Touch-AMOLED-1.43`. If the screen stays
black, check that line first — the boards' pin maps are completely different, so the
wrong image drives the wrong GPIOs and shows nothing.

## Bringing up a new board
Pins for the two supported boards are already confirmed and live in `src/boards/`; you
only need this if you are porting to a third board. See "Adding another board" in
[HARDWARE.md](HARDWARE.md).

1. Take the pins from the vendor's factory Arduino demos (`01_HelloWorld` gives the QSPI
   databus, `03`/`04` give the shared I2C) **or** from the Arduino-ESP32 core's board
   variant in `~/.platformio/packages/framework-arduinoespressif32/variants/`. Do not guess.
2. Confirm them on hardware before committing: an I2C scan should name the parts you
   expect, and a full-panel test pattern should fill the panel edge to edge.
3. Build `06_LVGL_Widgets` and copy its working **`lv_conf.h`** into this project's include path (we set `-DLV_CONF_INCLUDE_SIMPLE`). Match the LVGL major version in `platformio.ini` to that demo.

## lv_conf.h
LVGL needs `lv_conf.h` reachable on the include path. Easiest: copy from the Waveshare `06_LVGL_Widgets` demo (it's already tuned for this panel/color depth), set `LV_COLOR_DEPTH 16`, enable PSRAM draw buffers, and keep the QMI8658/touch indev wiring from demos `03/04`.

## WiFi & location
No secrets are committed. On first boot the captive portal collects SSID/password and home lat/lon. Defaults in `src/config.h` are Dénia (38.8409, 0.1059) — change as needed.

## HTTPS note
airplanes.live is HTTPS. For a hobby build, `WiFiClientSecure::setInsecure()` is fine. For production, pin the root CA. The choice is flagged in `adsb_client.cpp`.
