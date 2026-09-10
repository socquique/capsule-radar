#!/usr/bin/env bash
# Host-side tests. No hardware, no PlatformIO — just a C++17 compiler.
#   ./tests/run_host_tests.sh
#
# ArduinoJson comes from the PlatformIO lib cache, so build the device env at least once
# first (pio run -e esp32-s3-amoled-175).
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="${TMPDIR:-/tmp}/capsule-radar-tests"
mkdir -p "$OUT"
AJ=".pio/libdeps/esp32-s3-amoled-175/ArduinoJson/src"
rc=0

run() {  # run <name> <compiler args...>
    local name="$1"; shift
    printf '\n=== %s ===\n' "$name"
    if ! g++ -std=gnu++17 -O1 -Wall -o "$OUT/$name.exe" "$@"; then
        echo "  BUILD FAILED"; rc=1; return
    fi
    "$OUT/$name.exe" || rc=1
}

# The JSON stream test embeds a verbatim copy of ReliableJsonStream so it can run without
# the Arduino toolchain. Fail loudly if that copy drifts from the real one.
echo "=== drift check: ReliableJsonStream ==="
extract() { awk '/^class ReliableJsonStream/,/^};/' "$1" | sed 's/[[:space:]]*$//'; }
if diff <(extract src/adsb_client.cpp) <(extract tests/adsb_json_stream_test.cpp); then
    echo "  OK — test copy matches src/adsb_client.cpp"
else
    echo "  DRIFT — update tests/adsb_json_stream_test.cpp to match src/adsb_client.cpp"
    rc=1
fi

run snapshot_gate -Isrc tests/snapshot_gate_test.cpp

if [ -d "$AJ" ]; then
    run adsb_json_stream -Itests/stubs -I"$AJ" \
        -DARDUINOJSON_ENABLE_ARDUINO_STREAM=1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 \
        -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0 \
        -DARDUINOJSON_ENABLE_STD_STREAM=0 \
        tests/adsb_json_stream_test.cpp
else
    echo; echo "=== adsb_json_stream: SKIPPED (no $AJ) ==="
    echo "  run 'pio run -e esp32-s3-amoled-175' once to populate the library cache"
fi

printf '\n%s\n' "$([ $rc -eq 0 ] && echo 'ALL HOST TESTS PASSED' || echo 'HOST TESTS FAILED')"
exit $rc
