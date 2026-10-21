#pragma once
// Render-path timing counters (device only), read with GET /perf.
// Each slot sums the time spent in one kind of work on the LVGL core (core 1) and keeps
// its longest single run since the last read. A poller turns two reads into "ms of work
// per second" per slot; the maxima show the stalls. GET /perf?bench=1 (X-Radar header)
// also times full-screen repaints and animated page changes (perf::bench).
#include <stddef.h>
#include <stdint.h>

namespace perf {

enum Slot : uint8_t {
    LOOP,       // one pass of loop(), without its closing delay
    LVGL,       // lv_timer_handler(): LVGL timers, rendering and flushing
    FLUSH,      // flush_cb: one strip saved, blended and pushed (its compositor tick excluded)
    PUSH,       // panel pushes: LVGL strips and compositor windows (fill + transfer)
    COMPOSE,    // a compositor tick that drew something
    FEED,       // a new aircraft snapshot: radar::update, ui_on_data_updated, alert sounds
    GPS,        // gps_poll(), including the compositor ticks it runs while it waits
    WEB,        // the web server's handleClient()
    TOUCH_GAP,  // time between two reads of the touch panel
    SLOT_COUNT
};

uint32_t nowUs();
void add(Slot s, uint32_t us);
void refresh(uint32_t ms, uint32_t px);   // LVGL monitor_cb: one screen refresh done
uint32_t refreshes();                     // screen refreshes since boot

// Times its own lifetime into a slot.
class Scope {
public:
    explicit Scope(Slot s) : s_(s), t0_(nowUs()) {}
    ~Scope() { add(s_, nowUs() - t0_); }
private:
    Slot s_;
    uint32_t t0_;
};

// Plain-text counters for GET /perf; resets the per-read maxima.
size_t report(char *out, size_t n);
// Synchronous benchmark on the LVGL core: full-screen repaints, then animated page
// changes radar -> list -> radar. Appends one line per measurement.
size_t bench(char *out, size_t n);

} // namespace perf
