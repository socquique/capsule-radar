#pragma once
// Face-down sleep: the screen goes dark while the radar lies face-down. Pure logic, so
// tests/facedown_sleep_test.cpp drives it on the host with real accelerometer readings;
// imu_qmi8658.cpp only reads the sensor, and main.cpp's loop feeds this every
// FACEDOWN_CHECK_MS.
//
// Face-down is orientation-agnostic: which Z sign means "screen up" depends on how the board
// sits in the case, and it isn't the same on every unit. So a slow baseline follows the
// RESTING Z, and the radar counts as face-down when gravity swings to the far OPPOSITE side
// (FACEDOWN_THRESHOLD past zero), i.e. the board was flipped over.
//
// A touch keeps the screen on: a touch since the last check restarts the face-down count, so
// the screen goes dark only after FACEDOWN_COUNT checks face-down with no touch in between,
// and a touch on a dark screen lights it at once. A touch never moves the baseline. It used
// to: a touch on the dark screen adopted the current pose as the resting one, and the pose
// read next was usually still face-down (a hand lifting the radar, a finger on the glass
// while laying it down). The radar then took face-down for its resting pose and stayed lit
// face-down until it had stood upright long enough for the baseline to drift back.
#include <stdint.h>
#include "config.h"

class FaceDownSleep {
public:
    // One check. az: accelerometer Z (±2 g full scale, 16384 LSB/g); haveAz is false when the
    // read failed. A failed read leaves the state as it is: the shared I2C bus is noisy, and
    // treating a failure as "not face-down" kept resetting the count, so the screen never
    // slept. touchedMs: time since the last touch (LVGL inactivity). enabled: the web setting.
    void update(bool haveAz, int16_t az, uint32_t touchedMs, bool enabled) {
        if (haveAz) {
            if (!_haveRef) { _ref = az; _haveRef = true; }
            const bool down = (_ref < 0) ? (az > FACEDOWN_THRESHOLD) : (az < -FACEDOWN_THRESHOLD);
            if (down) {
                if (_count < FACEDOWN_COUNT) ++_count;
            } else {
                _count = 0;
                _ref += (az - _ref) >> 4;    // EMA toward the current (not face-down) pose
            }
        }
        if (touchedMs < FACEDOWN_CHECK_MS) _count = 0;   // a touch since the last check
        _asleep = enabled && _count >= FACEDOWN_COUNT;
    }

    bool asleep() const { return _asleep; }

private:
    int32_t _ref = 0;          // slow baseline of the resting Z
    bool    _haveRef = false;
    uint8_t _count = 0;        // face-down checks in a row with no touch
    bool    _asleep = false;
};
