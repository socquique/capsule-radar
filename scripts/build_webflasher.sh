#!/usr/bin/env bash
# Build the firmware and produce a single merged binary for the browser web-flasher
# (web/flash/). Run locally to preview the flasher before pushing; CI does the same.
#
#   ./scripts/build_webflasher.sh
#   then serve it over HTTPS/localhost, e.g.:  python3 -m http.server -d web/flash 8000
#   and open http://localhost:8000  (Web Serial works on localhost & HTTPS only)
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> Locating tools"
PY="$HOME/.platformio/penv/bin/python"
[ -x "$PY" ] || PY=python3

build_one() {   # build_one <pio-env> <output-bin>
  local env="$1" out="$2" b=".pio/build/$1"
  echo "==> Building firmware ($env)"
  pio run -e "$env"
  # locate AFTER the build: on a fresh machine the packages dir appears with the first pio run
  local boot_app0
  boot_app0="$(find "$HOME/.platformio/packages" -name boot_app0.bin -path '*framework-arduinoespressif32*' 2>/dev/null | head -1)"
  # PlatformIO's bundled tool-esptoolpy can be ancient (no esp32s3); prefer the pip module.
  if ! "$PY" -m esptool version >/dev/null 2>&1; then
    echo "==> Installing esptool into the PlatformIO penv"
    "$PY" -m pip install -q esptool
  fi
  echo "==> Merging bootloader + partitions + app -> $out"
  mkdir -p web/flash
  "$PY" -m esptool --chip esp32s3 merge_bin -o "$out" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0     "$b/bootloader.bin" \
    0x8000  "$b/partitions.bin" \
    0xe000  "$boot_app0" \
    0x10000 "$b/firmware.bin"
  echo "==> Done: $out ($(du -h "$out" | cut -f1))"
}

build_one esp32-s3-amoled-175 web/flash/CapsuleRadar-esp32s3.bin
build_one esp32-s3-amoled-143 web/flash/CapsuleRadar-esp32s3-amoled143.bin
