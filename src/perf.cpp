// Render-path timing counters and the on-device benchmark (see perf.h).
#include "perf.h"
#include "config.h"
#include "display.h"
#include "ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <stdio.h>

namespace perf {

static const char *const NAMES[SLOT_COUNT] = {
    "loop", "lvgl", "flush", "push", "compose", "feed", "gps", "web", "touch_gap",
};

struct Acc { uint64_t totalUs; uint32_t count; uint32_t maxUs; };
static Acc      s_acc[SLOT_COUNT];
static uint32_t s_refreshes = 0, s_refreshMaxMs = 0;
static uint64_t s_refreshMs = 0, s_refreshPx = 0;

uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }

void add(Slot s, uint32_t us) {
    Acc &a = s_acc[s];
    a.totalUs += us;
    a.count++;
    if (us > a.maxUs) a.maxUs = us;
}

void refresh(uint32_t ms, uint32_t px) {
    s_refreshes++;
    s_refreshMs += ms;
    s_refreshPx += px;
    if (ms > s_refreshMaxMs) s_refreshMaxMs = ms;
}

uint32_t refreshes() { return s_refreshes; }

size_t report(char *out, size_t n) {
    size_t k = 0;
    const auto put = [&](const char *fmt, auto... args) {
        if (k < n) k += (size_t)snprintf(out + k, n - k, fmt, args...);
    };
#if defined(LCD_QSPI_HZ)     // the LCD 2.1 has no QSPI panel bus: its report omits the clock
    put("up_ms %lu qspi_khz %d\n", (unsigned long)millis(), (int)(LCD_QSPI_HZ / 1000));
#else
    put("up_ms %lu\n", (unsigned long)millis());
#endif
    put("# slot count total_us max_us (max since the last read)\n");
    for (int i = 0; i < SLOT_COUNT; ++i) {
        put("%s %lu %llu %lu\n", NAMES[i], (unsigned long)s_acc[i].count,
            (unsigned long long)s_acc[i].totalUs, (unsigned long)s_acc[i].maxUs);
        s_acc[i].maxUs = 0;
    }
    put("refresh %lu %llu %lu px %llu\n", (unsigned long)s_refreshes,
        (unsigned long long)s_refreshMs, (unsigned long)s_refreshMaxMs, (unsigned long long)s_refreshPx);
    s_refreshMaxMs = 0;
    put("heap_internal free %u largest %u min %u psram_free %u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return k < n ? k : n;
}

// Where a measured stretch went: compositor ticks, flushing (LVGL strips to the panel) and
// the rest (LVGL's own rendering and timers), in ms.
struct Split { uint64_t compose, flush; uint32_t flushes, t0; };
static Split split_start() {
    return Split{ s_acc[COMPOSE].totalUs, s_acc[FLUSH].totalUs, s_acc[FLUSH].count, nowUs() };
}
static void split_put(const Split &s, uint32_t *total, uint32_t *compose, uint32_t *flush) {
    *total = (nowUs() - s.t0 + 500) / 1000;
    *compose = (uint32_t)((s_acc[COMPOSE].totalUs - s.compose + 500) / 1000);
    *flush = (uint32_t)((s_acc[FLUSH].totalUs - s.flush + 500) / 1000);
}

// One animated page change, driven like loop() drives LVGL, until the tileview stops.
static void bench_page(int idx, uint32_t *ms, uint32_t *frames, uint32_t *compose, uint32_t *flush,
                       Split *sp = nullptr) {
    const uint32_t f0 = s_refreshes, t0 = millis();
    const Split s = split_start();
    if (sp) *sp = s;
    ui_bench_page(idx);
    while (ui_bench_moving() && millis() - t0 < 3000) display::loop();
    display::loop();                                   // the frame that lands on the page
    uint32_t total;
    split_put(s, &total, compose, flush);
    *ms = millis() - t0;
    *frames = s_refreshes - f0;
}

size_t bench(char *out, size_t n) {
    size_t k = 0;
    const auto put = [&](const char *fmt, auto... args) {
        if (k < n) k += (size_t)snprintf(out + k, n - k, fmt, args...);
    };
    const int view = ui_bench_view();
    if (view != 0) ui_show_view(0);
    display::loop();
    put("bench_full_ms");
    uint32_t sumT = 0, sumC = 0, sumF = 0;
    for (int i = 0; i < 8; ++i) {
        lv_obj_invalidate(lv_scr_act());
        const Split sp = split_start();
        lv_refr_now(NULL);
        uint32_t t, c, f;
        split_put(sp, &t, &c, &f);
        sumT += t; sumC += c; sumF += f;
        put(" %lu", (unsigned long)t);
    }
    put("\nbench_full_avg_ms total %lu compose %lu flush %lu render %lu\n", (unsigned long)(sumT / 8),
        (unsigned long)(sumC / 8), (unsigned long)(sumF / 8), (unsigned long)((sumT - sumC - sumF) / 8));
    for (int r = 0; r < 2; ++r) {
        uint32_t ms, fr, c, f;
        Split s;
        bench_page(1, &ms, &fr, &c, &f, &s);
        put("bench_page radar->list ms %lu frames %lu compose %lu flush %lu (strips %lu)\n",
            (unsigned long)ms, (unsigned long)fr, (unsigned long)c, (unsigned long)f,
            (unsigned long)(s_acc[FLUSH].count - s.flushes));
        bench_page(0, &ms, &fr, &c, &f, &s);
        put("bench_page list->radar ms %lu frames %lu compose %lu flush %lu (strips %lu)\n",
            (unsigned long)ms, (unsigned long)fr, (unsigned long)c, (unsigned long)f,
            (unsigned long)(s_acc[FLUSH].count - s.flushes));
    }
    if (view != 0) ui_show_view(view);
    return k < n ? k : n;
}

} // namespace perf
