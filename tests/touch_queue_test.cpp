// Touch replay queue (src/touch_queue.h). LVGL reads the pointer only between frames, so the
// device samples the panel between flush strips and replays the samples in order. These
// checks guard what LVGL must see: the whole path in order, a lift where the finger was,
// no lift from a failed read, and a bounded queue that keeps the newest position.
#include "touch_queue.h"

#include <assert.h>
#include <stdint.h>

typedef TouchQueue::Sample S;

static bool same(const S &a, int x, int y, bool pressed) {
    return a.x == x && a.y == y && a.pressed == pressed;
}

int main() {
    // --- a quick swipe sampled between strips reaches LVGL whole and in order ---
    {
        TouchQueue q;
        q.add(TouchQueue::DOWN, 400, 233, 1000);
        q.add(TouchQueue::DOWN, 300, 236, 1012);
        q.add(TouchQueue::DOWN, 180, 238, 1024);
        q.add(TouchQueue::DOWN, 60, 240, 1036);
        q.add(TouchQueue::UP, 0, 0, 1048);
        S s;
        assert(q.next(s) && same(s, 400, 233, true));
        assert(q.next(s) && same(s, 300, 236, true));
        assert(q.next(s) && same(s, 180, 238, true));
        assert(q.next(s) && same(s, 60, 240, true));
        assert(!q.empty());
        assert(q.next(s) && same(s, 60, 240, false));   // the lift is where the finger was
        assert(q.empty() && !q.next(s));
        assert(same(q.delivered(), 60, 240, false));    // repeated while nothing new arrives
    }

    // --- nothing new is queued for a resting finger or a repeated lift ---
    {
        TouchQueue q;
        q.add(TouchQueue::UP, 0, 0, 0);                 // never pressed: no lift to report
        assert(q.empty());
        q.add(TouchQueue::DOWN, 200, 200, 10);
        q.add(TouchQueue::DOWN, 200, 200, 20);          // same spot
        q.add(TouchQueue::UP, 0, 0, 30);
        q.add(TouchQueue::UP, 0, 0, 40);                // still lifted
        S s;
        assert(q.next(s) && same(s, 200, 200, true));
        assert(q.next(s) && same(s, 200, 200, false));
        assert(q.empty());
    }

    // --- a failed read in the middle of a swipe is not a lift ---
    {
        TouchQueue q;
        q.add(TouchQueue::DOWN, 10, 10, 1000);
        q.add(TouchQueue::NODATA, 0, 0, 1010);
        q.add(TouchQueue::NODATA, 0, 0, 1050);
        q.add(TouchQueue::DOWN, 20, 10, 1060);
        S s;
        assert(q.next(s) && same(s, 10, 10, true));
        assert(q.next(s) && same(s, 20, 10, true));
        assert(q.empty());
    }

    // --- reads that keep failing end the press, so it cannot stick ---
    {
        TouchQueue q;
        q.add(TouchQueue::DOWN, 10, 10, 1000);
        q.add(TouchQueue::NODATA, 0, 0, 1010);          // the failing streak starts here
        q.add(TouchQueue::NODATA, 0, 0, 1010 + TOUCH_NODATA_RELEASE_MS - 1);
        S s;
        assert(q.next(s) && same(s, 10, 10, true));
        assert(q.empty());
        q.add(TouchQueue::NODATA, 0, 0, 1010 + TOUCH_NODATA_RELEASE_MS);
        assert(q.next(s) && same(s, 10, 10, false));
        q.add(TouchQueue::NODATA, 0, 0, 2000);          // lifted now: failures change nothing
        assert(q.empty());
    }

    // --- a good read restarts the failing streak ---
    {
        TouchQueue q;
        q.add(TouchQueue::DOWN, 10, 10, 1000);
        q.add(TouchQueue::NODATA, 0, 0, 1010);
        q.add(TouchQueue::DOWN, 11, 10, 1090);
        q.add(TouchQueue::NODATA, 0, 0, 1150);          // a new streak, not 140 ms of failures
        q.add(TouchQueue::NODATA, 0, 0, 1150 + TOUCH_NODATA_RELEASE_MS - 1);
        S s;
        assert(q.next(s) && same(s, 10, 10, true));
        assert(q.next(s) && same(s, 11, 10, true));
        assert(q.empty());
    }

    // --- the failing streak times out across the millis() wrap ---
    {
        TouchQueue q;
        const uint32_t t0 = 0xFFFFFFF0u;
        q.add(TouchQueue::DOWN, 5, 5, t0);
        q.add(TouchQueue::NODATA, 0, 0, t0);
        q.add(TouchQueue::NODATA, 0, 0, (uint32_t)(t0 + TOUCH_NODATA_RELEASE_MS - 1));
        S s;
        assert(q.next(s) && same(s, 5, 5, true));
        assert(q.empty());
        q.add(TouchQueue::NODATA, 0, 0, (uint32_t)(t0 + TOUCH_NODATA_RELEASE_MS));
        assert(q.next(s) && same(s, 5, 5, false));
    }

    // --- a full queue keeps the press start and the newest position, and never loses a lift ---
    {
        TouchQueue q;
        for (int i = 0; i < TOUCH_QUEUE_LEN + 5; ++i)
            q.add(TouchQueue::DOWN, (int16_t)(10 + i), 100, (uint32_t)(1000 + 10 * i));
        q.add(TouchQueue::UP, 0, 0, 2000);              // replaces the newest move
        q.add(TouchQueue::DOWN, 300, 300, 2010);        // a new press behind the lift: no room yet
        S s;
        assert(q.next(s) && same(s, 10, 100, true));    // the press start survived
        int n = 1;
        S last = s;
        while (q.next(s)) { last = s; ++n; }
        assert(n == TOUCH_QUEUE_LEN);
        assert(same(last, 10 + TOUCH_QUEUE_LEN + 4, 100, false));   // lifted at the newest point
        q.add(TouchQueue::DOWN, 300, 300, 2020);        // room again: the new press gets in
        assert(q.next(s) && same(s, 300, 300, true));
        assert(q.empty());
    }

    // --- the black corners outside the logical UI arrive as a lift (display.cpp maps them) ---
    {
        TouchQueue q;
        q.add(TouchQueue::DOWN, 230, 230, 0);
        q.add(TouchQueue::UP, 0, 0, 10);                // finger slid into a corner
        q.add(TouchQueue::DOWN, 231, 230, 20);          // and back: a new press
        S s;
        assert(q.next(s) && same(s, 230, 230, true));
        assert(q.next(s) && same(s, 230, 230, false));
        assert(q.next(s) && same(s, 231, 230, true));
    }
}
