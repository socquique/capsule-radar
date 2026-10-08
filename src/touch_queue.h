#pragma once
// Touch samples taken between LVGL's pointer reads, handed to LVGL in order.
//
// LVGL reads the pointer from a timer inside lv_timer_handler(), and no timer runs while a
// frame renders. A swipe redraws the whole screen on every frame, and on the device a full
// frame takes ~150-200 ms, so LVGL saw the finger about five times a second. A quick swipe
// reached it as a press and a release at one spot: a tap on whatever was under the finger
// (a plane, a list row, the weather panel). A slower one arrived as a few big jumps, and the
// tileview often snapped back. display.cpp now also samples the panel between the flush
// strips of a frame, and its read callback hands LVGL every sample in order
// (continue_reading), so LVGL's scroll, snap and click logic sees the path the finger took.
//
// Pure (no LVGL, no Arduino): tests/touch_queue_test.cpp drives it on the host.
#include <stdint.h>
#include "config.h"

static_assert(TOUCH_QUEUE_LEN > 0 && TOUCH_QUEUE_LEN <= 255, "TouchQueue keeps its indices in uint8_t");

class TouchQueue {
public:
    enum Kind { UP, DOWN, NODATA };          // NODATA: the read failed, nothing was learned
    struct Sample { int16_t x, y; bool pressed; };

    // One reading of the panel, taken at nowMs.
    void add(Kind kind, int16_t x, int16_t y, uint32_t nowMs) {
        if (kind == NODATA) {
            // A failed read is not a lift. Only reads that keep failing (the controller
            // stopped answering) end the press, so a press can never stick for good.
            if (!_lastPressed) return;
            if (!_failing) { _failing = true; _failSinceMs = nowMs; return; }
            if (nowMs - _failSinceMs < TOUCH_NODATA_RELEASE_MS) return;
            kind = UP;
        }
        _failing = false;
        if (kind == UP) {
            if (!_lastPressed) return;                  // already lifted: nothing new
            push(Sample{_lastX, _lastY, false});        // the lift happens where the finger was
            return;
        }
        if (_lastPressed && x == _lastX && y == _lastY) return;   // the finger did not move
        push(Sample{x, y, true});
    }

    // The oldest waiting sample. False when none is waiting.
    bool next(Sample &out) {
        if (_count == 0) return false;
        out = _q[_head];
        _head = (uint8_t)((_head + 1) % TOUCH_QUEUE_LEN);
        --_count;
        _delivered = out;
        return true;
    }

    bool empty() const { return _count == 0; }

    // The sample LVGL received last. Reported again when nothing new arrived.
    Sample delivered() const { return _delivered; }

private:
    void push(const Sample &s) {
        if (_count == TOUCH_QUEUE_LEN) {
            // Full: LVGL has not read for a long time. Keep the newest position: a move
            // replaces the move before it, and a lift replaces the move it ends. A new press
            // behind a lift waits for the next reading (the state stays "lifted").
            Sample &tail = _q[(_head + _count - 1) % TOUCH_QUEUE_LEN];
            if (!tail.pressed) return;
            tail = s;
        } else {
            _q[(_head + _count) % TOUCH_QUEUE_LEN] = s;
            ++_count;
        }
        _lastPressed = s.pressed;
        _lastX = s.x;
        _lastY = s.y;
    }

    Sample   _q[TOUCH_QUEUE_LEN];
    uint8_t  _head = 0, _count = 0;
    Sample   _delivered = {0, 0, false};
    bool     _lastPressed = false;              // the last state queued
    int16_t  _lastX = 0, _lastY = 0;
    bool     _failing = false;                  // reads are failing while pressed...
    uint32_t _failSinceMs = 0;                  // ...since this time
};
