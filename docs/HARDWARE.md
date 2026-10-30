# Hardware

Capsule Radar supports two Waveshare round-AMOLED boards and the round LCD-1.85C V2.
The AMOLED boards use the same SoC and the same 466×466 CO5300 panel, but their **pin
maps have nothing in common** — flashing one board's image onto another gives a black
screen and a silent I2C bus.

Pin maps live one-per-board in [`../src/boards/`](../src/boards/) and are selected by a
build flag (see [`../src/config.h`](../src/config.h)):

| Board | PlatformIO env | Build flag | Header |
|---|---|---|---|
| ESP32-S3-Touch-AMOLED-1.75 (reference) | `esp32-s3-amoled-175` | *(default)* | `board_amoled_175.h` |
| ESP32-S3-Touch-AMOLED-1.43 | `esp32-s3-amoled-143` | `-DBOARD_AMOLED_143` | `board_amoled_143.h` |
| ESP32-S3-Touch-LCD-1.85C V2 | `esp32-s3-lcd-185c-v2` | `-DBOARD_LCD185C_V2` | `board_lcd_185c_v2.h` |

---

## ESP32-S3-Touch-AMOLED-1.75 (reference board)

- **MCU**: ESP32-S3R8 (Xtensa LX7 dual-core @240 MHz), 512 KB SRAM, **8 MB PSRAM**, **16 MB flash**.
- **Wireless**: 2.4 GHz WiFi (b/g/n) + Bluetooth 5 (LE), onboard antenna (IPEX option).
- **Display**: 1.75" AMOLED, **466×466**, driver **CO5300** over **QSPI**. Brightness via panel command (no PWM backlight line).
- **Touch**: **CST9217** capacitive, I2C.
- **IMU**: **QMI8658** 6-axis, I2C. **RTC**: **PCF85063**, I2C.
- **PMIC**: **AXP2101**, I2C — LiPo charge + rails.
- **Audio**: **ES8311** codec + onboard speaker; dual-mic array (ES7210 on the -C variant).
- **Storage**: microSD (TF). **Buttons**: PWR + BOOT. **GPS** (`-G` variant only): LC76G.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| LCD CS | 12 | | I2C SDA | 15 |
| LCD RST | 39 | | I2C SCL | 14 |
| LCD QSPI SCLK | 38 | | I2S MCLK | 42 |
| LCD QSPI D0–D3 | 4, 5, 6, 7 | | I2S BCLK | 9 |
| Touch INT | 11 | | I2S LRCLK | 45 |
| Touch RST | 40 | | I2S DOUT / DIN | 8 / 10 |
| Touch transform | mirror_x + mirror_y | | Speaker amp enable | 46 |

Panel column gap **6**, row gap 0, QSPI clock 80 MHz.
I2C: CST9217 `0x5A`, QMI8658 `0x6B`, PCF85063 `0x51`, AXP2101 `0x34`, ES8311 `0x18`.

Sources: the board's ESPHome definition, the Waveshare board definition in xiaozhi-esp32,
and a working Arduino_GFX port for this panel.

---

## ESP32-S3-Touch-AMOLED-1.43

Same 466×466 CO5300 panel, 8 MB PSRAM / 16 MB flash. Differences that matter:

- **Touch is an FT3168**, not a CST9217, at a different address and with a different protocol.
- **No AXP2101 and no ES8311** — no battery reporting and no alert pings. Both compile out
  via `BOARD_HAS_PMIC` / `BOARD_HAS_AUDIO`. This is not merely cosmetic: the 1.75's I2S
  BCLK/DIN (GPIO 9/10) are *this* board's QSPI CS and SCLK, so leaving audio enabled here
  would drive the panel's own bus lines.
- The FT3168 has **no reset line of its own**; it is tied to the panel reset (GPIO 21) and
  does not appear on I2C at all until the display has been initialised. `display::begin()`
  therefore must keep `gfx->begin()` ahead of `touch_begin()`.
- IMU and RTC are the same parts at the same addresses as the 1.75.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| LCD CS | 9 | | I2C SDA | 47 |
| LCD RST | 21 | | I2C SCL | 48 |
| LCD QSPI SCLK | 10 | | Touch INT / RST | not broken out |
| LCD QSPI D0–D3 | 11, 12, 13, 14 | | Touch transform | none (raw coords match) |
| Battery ADC | 4 (unused — see below) | | IMU INT / RTC INT | 8 / 15 |

Panel column gap **6** (same as the 1.75), row gap 0, QSPI clock 40 MHz (the vendor rate;
80 MHz is untested here).
I2C: FT3168 `0x38` (chip-id register `0xA3` reads `0x64`), QMI8658 `0x6B`, PCF85063 `0x51`.

Battery: there is a divided battery voltage on GPIO 4, but the divider ratio is not
documented, so the firmware reports "no battery" rather than an invented percentage.

**Sources.** Pins come from the Arduino-ESP32 core's own board variant
(`variants/waveshare_esp32_s3_touch_amoled_143/pins_arduino.h`, shared with the 1.64), and
each was then confirmed on hardware: an I2C scan on 47/48 answers `0x51` + `0x6B` + `0x38`,
and the panel drives correctly on CS=9 / SCK=10 / D0–D3=11–14 / RST=21. The touch orientation
was verified with an on-screen dot tracking a fingertip, and the column gap by nudging a
full-panel rim-ring test card one pixel at a time until it centred.

> Do **not** "correct" these against the Waveshare wiki pin table: that table is misaligned
> and lists an I2C SCL of GPIO 49, which does not exist on an ESP32-S3.

---

## ESP32-S3-Touch-LCD-1.85C V2

ESP32-S3R8 with 8 MB octal PSRAM and 16 MB flash, like the AMOLED boards. Only the V2
board (PCB Rev2.0) is covered; V1 has different audio hardware. Differences that matter:

- **The display is a 360×360 ST77916 LCD** over QSPI, driven by Arduino_GFX's
  `Arduino_ST77916`. Brightness is an LEDC PWM on GPIO 5, not a panel command.
- **The UI stays 466×466.** `src/panel_scale.h` resamples every write to the panel
  (bilinear). Touch drivers report panel pixels, and `display.cpp` scales them back up
  to UI pixels. The panel has no column or row gap.
- **Two glass revisions** need different init tables. At boot the firmware reads the
  panel ID (command 0x04 at 3 MHz): `00 7F 7F 7F` selects Waveshare's default table and
  `00 02 7F 7F` its newer one. Any other ID leaves the display off rather than guess.
  `tools/gen_st77916_init.py` generates `src/st77916_ws185_init.h` from those tables.
- **Panel and touch resets are on a TCA9554** at `0x20`: P0 resets the touch and P1 the
  panel. The firmware makes only P0 and P1 outputs and leaves the other pins alone.
- **Touch is a CST816** at `0x15` (chip ID `0xB5`), polled. It uses the CST820 driver.
- **No PMIC, no IMU, no GPS.** Battery, face-down sleep and GPS compile out
  (`BOARD_HAS_PMIC`, `BOARD_HAS_IMU`, `BOARD_HAS_GPS`). The PCF85063 RTC is fitted.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| LCD CS | 21 | | I2C SDA | 11 |
| LCD QSPI SCLK | 40 | | I2C SCL | 10 |
| LCD QSPI D0–D3 | 46, 45, 42, 41 | | I2S MCLK / BCLK | 2 / 48 |
| LCD backlight | 5 (LEDC) | | I2S LRCLK | 38 |
| LCD / touch reset | TCA9554 P1 / P0 | | I2S DOUT / DIN | 47 / 39 |
| Touch INT | not used (polled) | | Speaker amp enable | 15 |

I2C: CST816 `0x15`, ES8311 `0x18`, TCA9554 `0x20`, ES7210 `0x40` (microphones, not
used), PCF85063 `0x51`. QSPI clock 80 MHz.

**Sources.** Pins, the reset wiring and the two ST77916 init tables come from
Waveshare's [factory demo](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C/tree/8ead4a96bf3a278fc4ebd8ef4768657e17fa2880)
and [V2 schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C/ESP32-S3-Touch-LCD-1.85C_V2.pdf),
as used by the working port in [Hermes Gadget SDK](https://github.com/Adolanium/hermes-gadget-sdk)
(MIT). The panel-ID check and the expander reset are adapted from that port. The init
tables are Apache-2.0 (Espressif Systems); their notice is in `src/st77916_ws185_init.h`.
Each was then confirmed on hardware: the boot log shows the expander, the panel ID
`00 02 7F 7F` and touch chip ID `0xB5`; an I2C scan answers `0x15 0x18 0x20 0x40 0x51`;
a full-panel rim-ring test card was centred with no edge cut off and the colours in
order; and an on-screen dot followed a fingertip.

---

## Adding another board

1. Copy a header in `src/boards/`, fill in the pins **from the vendor demo or the Arduino
   core board variant** — never guess them.
2. Add an `[env:…]` in `platformio.ini` that `extends = amoled_common` and sets `-DBOARD_…`.
3. Set `BOARD_HAS_PMIC` / `BOARD_HAS_AUDIO` / `BOARD_HAS_IMU` /
   `BOARD_HAS_GPS` / `TOUCH_DRIVER_…` for what is actually fitted;
   add a touch driver alongside `touch_cst9217.cpp` / `touch_ft3168.cpp` if needed.
4. Confirm on hardware before committing: I2C scan, a full-panel test pattern for the gap,
   and a touch dot that lands under your fingertip.
5. Add the env to `.github/workflows/release.yml` so it ships with every release.

## Reference
- 1.75 wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75
- 1.43 wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43
- ESPHome device page (1.75 pin source): https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/
